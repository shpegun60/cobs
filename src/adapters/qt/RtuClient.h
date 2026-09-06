/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * adapters::qt::RtuClient — a Modbus RTU master on top of SerialAdapter, shaped
 * like QtSerialBus's QModbusRtuSerialClient and running on this repository's
 * endpoint instead of Qt's parser: one request in flight, a queue behind it, a
 * response timeout with retries, an inter-frame delay between transactions and
 * a turnaround delay after a broadcast.
 *
 *     QSerialPort port;
 *     port.setPortName("COM6"); port.setBaudRate(9600); port.open(QIODevice::ReadWrite);
 *
 *     using Link = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<>,
 *                      modbus::rtu::framing::Standard<modbus::rtu::framing::Direction::Response>>;
 *     Link link;
 *     adapters::qt::RtuClient client{port, link};
 *     client.update_timing_from_port();      // 3.5 character times below 19200 baud
 *     client.bind();
 *
 *     const uint8_t body[] = {0x00, 0x6B, 0x00, 0x03};   // start address, quantity
 *     client.send(0x11, 0x03, body, Handler{[](const adapters::qt::Response& response) {
 *         if (response.state == adapters::qt::RequestState::Completed) { use(response.data); }
 *     }});
 *
 * What it copies from QModbus, and why. One transaction at a time, because RTU
 * has no way to tell two outstanding responses apart. A response timeout per
 * attempt (1000 ms, Qt's default) with retries (3, Qt's default), because a
 * silent server is the normal failure. A clean start before every attempt —
 * QModbusRtuSerialClientPrivate::processQueue() clears its response buffer and
 * the port's — so a late response to an abandoned request cannot be read as
 * the answer to this one; here that is SerialAdapter::discard_incoming(). An
 * inter-frame delay between transactions, computed exactly as
 * QModbusDevicePrivate::calculateInterFrameDelay does: 3.5 character times of
 * eleven bits below 19200 baud, a flat 2 ms at or above it, never below a
 * floor the application set. A turnaround delay (100 ms, Qt's default) after
 * a broadcast to address 0, which is answered by nobody. Matching by address
 * and function code with the exception bit masked off, so an exception
 * response completes its own request — Qt's canMatchRequestAndResponse() and
 * QModbusPdu::functionCode() do the same.
 *
 * Gaps between transactions are deadlines, not delays: when a request
 * finishes, the earliest moment the next frame may leave is fixed BEFORE the
 * handler runs, so a handler that queues the next request from inside the
 * callback (the usual shape of a polling loop) cannot shorten the turnaround
 * after a broadcast or the inter-frame gap after a response.
 *
 * A transport error ends the transaction it hit. When the port reports a
 * write or resource error while a request is leaving or awaiting its answer,
 * the adapter has already cleared the output and taken the block back; the
 * request is finished as RequestState::WriteError at once, not after the
 * response timeout, and the queue moves on. A read error drops the response
 * in flight; the request keeps waiting for its timeout, as it does in Qt.
 *
 * What it does not copy. Qt's request/reply objects, data units and register
 * models: a request here is an address, a function and its bytes, and a
 * response is what the peer sent, so the caller keeps the application's own
 * types. The response's span is the endpoint's own packet memory and is valid
 * for the duration of the callback only; copy what must outlive it.
 *
 * Threading and lifetime: everything runs on the thread of the port's event
 * loop. The port and the endpoint outlive the client; unbind() cancels a
 * queued request with RequestState::Cancelled, while the destructor drops the
 * queue silently, since a handler must not run while its owner is being
 * destroyed; the adapter it owns unbinds the endpoint on the way out.
 */

#ifndef ADAPTERS_QT_RTU_CLIENT_H_
#define ADAPTERS_QT_RTU_CLIENT_H_

#include "adapters/qt/SerialAdapter.h"
#include "modbus/Types.h"

#include "tiny_delegate.hpp"

#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace adapters::qt {

enum class RequestState : uint8_t {
	Completed,   // a response matching address and function arrived
	Broadcast,   // address 0: the request left the port, nobody answers it
	Timeout,     // no matching response within the timeout, retries exhausted
	WriteError,  // the endpoint or the port refused the frame, or the port failed while it was leaving
	Cancelled,   // unbind() while the request was queued
};

struct Response final {
	RequestState state = RequestState::Timeout;
	uint8_t address = 0u;
	uint8_t function = 0u;            // the request's function; the exception bit is not part of it
	bool exception = false;           // the peer answered with function | 0x80
	uint8_t exception_code = 0u;      // meaningful when exception is true
	std::span<const uint8_t> data{};  // the packet's own memory: valid during the callback only
	unsigned attempts = 0u;           // transmissions it took, including the one that succeeded
};

template<class EndpointT, class PortT = QSerialPort>
class RtuClient final {
	static_assert(RtuEndpoint<EndpointT>, "RtuClient serves a modbus::rtu::Endpoint");

public:
	using Adapter = SerialAdapter<EndpointT, PortT>;
	using Endpoint = EndpointT;
	using Port = PortT;
	using Handler = tiny::delegate<void(const Response&)>;

	// QModbus's defaults, so a port that worked with QModbus works here.
	static constexpr int default_response_timeout_ms = 1000;
	static constexpr int default_retries = 3;
	static constexpr int default_turnaround_delay_ms = 100;
	// QModbusDevicePrivate::RecommendedDelay: 1.75 ms rounded up to whole
	// milliseconds, the inter-frame delay at and above 19200 baud.
	static constexpr int recommended_inter_frame_delay_ms = 2;

	RtuClient(PortT& port, EndpointT& endpoint)
		: m_adapter(port, endpoint), m_port(port), m_endpoint(endpoint)
	{
		m_response.setSingleShot(true);
		m_response.setTimerType(Qt::PreciseTimer);
		m_schedule.setSingleShot(true);
		m_schedule.setTimerType(Qt::PreciseTimer);
		QObject::connect(&m_response, &QTimer::timeout, &m_response, [this]() { on_response_timeout(); });
		QObject::connect(&m_schedule, &QTimer::timeout, &m_schedule, [this]() { process_queue(); });
	}

	RtuClient(const RtuClient&) = delete;
	RtuClient& operator=(const RtuClient&) = delete;

	~RtuClient()
	{
		m_response.stop();
		m_schedule.stop();
		m_queue.clear();   // silently: a handler must not run from a destructor
	}

	[[nodiscard]] bool bind()
	{
		if (m_adapter.bound()) {
			return true;   // already bound: nothing to reset, least of all the gap a broadcast still owes
		}
		if (!m_adapter.bind()) {
			return false;
		}
		m_adapter.set_service_handler(typename Adapter::ServiceHandler{
			tiny::bind<&RtuClient::service>(*this)});
		m_earliest_next_ms = m_adapter.now_ms();   // a fresh session owes no gap to a cancelled one
		return true;
	}

	// Cancels what is queued (each handler is called with RequestState::Cancelled)
	// and detaches. False, changing nothing, while a frame has not left the port.
	[[nodiscard]] bool unbind()
	{
		if (!m_adapter.bound()) {
			return true;
		}
		if (!m_adapter.unbind()) {
			return false;
		}
		m_response.stop();
		m_schedule.stop();
		m_state = State::Idle;
		while (!m_queue.empty()) {
			finish_head(RequestState::Cancelled, {}, false, 0u);
		}
		return true;
	}

	[[nodiscard]] bool bound() const noexcept { return m_adapter.bound(); }
	[[nodiscard]] Adapter& adapter() noexcept { return m_adapter; }

	// QModbus's knobs, with its names and its defaults.
	void set_response_timeout_ms(const int milliseconds) noexcept
	{
		m_response_timeout_ms = std::max(1, milliseconds);
	}
	void set_retries(const int retries) noexcept { m_retries = std::max(0, retries); }
	void set_turnaround_delay_ms(const int milliseconds) noexcept
	{
		m_turnaround_delay_ms = std::max(0, milliseconds);
	}
	// A floor, as in Qt: the computed delay is used when it is longer. Unlike
	// Qt's, lowering the floor lowers the delay again.
	void set_inter_frame_delay_ms(const int milliseconds) noexcept
	{
		m_inter_frame_floor_ms = std::max(0, milliseconds);
	}

	[[nodiscard]] int response_timeout_ms() const noexcept { return m_response_timeout_ms; }
	[[nodiscard]] int retries() const noexcept { return m_retries; }
	[[nodiscard]] int turnaround_delay_ms() const noexcept { return m_turnaround_delay_ms; }
	[[nodiscard]] int inter_frame_delay_ms() const noexcept
	{
		return std::max(m_computed_inter_frame_ms, m_inter_frame_floor_ms);
	}

	/*
	 * Recomputes the inter-frame delay from the port's current baud rate, the
	 * way QModbusDevicePrivate::calculateInterFrameDelay() does: 3.5 characters
	 * of eleven bits below 19200 baud, rounded up because the specification
	 * asks for at least 3.5; the recommended flat value at or above it. Call it
	 * after the port's rate changes.
	 */
	void update_timing_from_port() noexcept
	{
		int computed = recommended_inter_frame_delay_ms;
		if constexpr (requires(const PortT& port) { port.baudRate(); }) {
			const qint32 baud = m_port.baudRate();
			if (baud > 0 && baud < 19200) {
				computed = static_cast<int>(
					std::ceil(3500.0 / (static_cast<double>(baud) / 11.0)));
			}
		}
		m_computed_inter_frame_ms = computed;
	}

	/*
	 * Queues one request. Address 0 is a broadcast: it is written and completed
	 * with RequestState::Broadcast, and the next request waits the turnaround
	 * delay. The handler runs exactly once, on this thread, never before this
	 * call returns.
	 */
	[[nodiscard]] bool send(
			const uint8_t address,
			const uint8_t function,
			const std::span<const uint8_t> data,
			Handler on_finished)
	{
		if (!m_adapter.bound() || data.size() > EndpointT::max_send_size) {
			return false;
		}
		Pending pending;
		pending.address = address;
		pending.function = function;
		pending.data.assign(data.begin(), data.end());
		pending.handler = static_cast<Handler&&>(on_finished);
		pending.attempts_left = m_retries + 1;
		m_queue.push_back(static_cast<Pending&&>(pending));
		schedule_next(inter_frame_delay_ms());
		return true;
	}

	[[nodiscard]] std::size_t pending() const noexcept { return m_queue.size(); }
	[[nodiscard]] bool idle() const noexcept { return m_state == State::Idle && m_queue.empty(); }

	/*
	 * Called by the adapter whenever the endpoint's state may have changed: a
	 * chunk was consumed, a transmission finished or failed, a gap or a stale
	 * frame was announced. Settles a transport error first, drains ready
	 * packets, then advances the transaction.
	 */
	void service()
	{
		const TransportError error = m_adapter.take_transport_error();
		if ((error == TransportError::Write || error == TransportError::Resource) &&
		    (m_state == State::Sending || m_state == State::Waiting) && !m_queue.empty()) {
			// The frame did not (fully) leave and will not: the adapter cleared
			// the output and took the block back. Nothing can answer it.
			m_response.stop();
			finish_head(RequestState::WriteError, {}, false, 0u);
			schedule_next(inter_frame_delay_ms());
		}
		while (auto packet = m_endpoint.pop_packet()) {
			on_packet(packet);
		}
		if (m_state == State::Sending && !m_endpoint.tx_active()) {
			// The frame has left the port. A broadcast is finished by that
			// alone; anything else now waits for its response.
			if (m_queue.empty()) {
				m_state = State::Idle;
			} else if (m_queue.front().address == 0u) {
				finish_head(RequestState::Broadcast, {}, false, 0u);
				schedule_next(m_turnaround_delay_ms);
			} else {
				m_state = State::Waiting;
				m_response.start(m_response_timeout_ms);
			}
		}
	}

private:
	enum class State : uint8_t { Idle, Scheduled, Sending, Waiting };

	struct Pending final {
		uint8_t address = 0u;
		uint8_t function = 0u;
		std::vector<uint8_t> data;
		Handler handler;
		int attempts_left = 1;
		unsigned attempts = 0u;
	};

	// Arms the next transmission no sooner than `delay_ms` from now AND no
	// sooner than the deadline the last finished request fixed.
	void schedule_next(const int delay_ms)
	{
		if (m_state != State::Idle || m_queue.empty()) {
			return;
		}
		const int32_t until_allowed = static_cast<int32_t>(m_earliest_next_ms - m_adapter.now_ms());
		const int effective = std::max(std::max(0, delay_ms), until_allowed > 0 ? static_cast<int>(until_allowed) : 0);
		m_state = State::Scheduled;
		m_schedule.start(effective);
	}

	void process_queue()
	{
		if (m_queue.empty()) {
			m_state = State::Idle;
			return;
		}
		// Qt starts every attempt from a clean slate; so does this.
		m_adapter.discard_incoming();

		Pending& head = m_queue.front();
		auto message = m_endpoint.make_message(head.address, head.function, head.data.size());
		if (!message || !message.append_bytes(std::span<const uint8_t>{head.data})) {
			finish_head(RequestState::WriteError, {}, false, 0u);
			schedule_next(inter_frame_delay_ms());
			return;
		}
		--head.attempts_left;
		++head.attempts;
		const modbus::SendResult result = m_endpoint.send(message);
		if (result == modbus::SendResult::Sent) {
			m_state = State::Sending;
			service();   // a transport that took the frame at once is already done
		} else if (result == modbus::SendResult::Busy) {
			m_state = State::Idle;
			schedule_next(1);   // the previous frame is still leaving; look again shortly
		} else {
			(void)m_adapter.take_transport_error();   // the refusal is settled here, not in service()
			finish_head(RequestState::WriteError, {}, false, 0u);
			schedule_next(inter_frame_delay_ms());
		}
	}

	template<class PacketT>
	void on_packet(const PacketT& packet)
	{
		if (m_state != State::Waiting || m_queue.empty()) {
			return;   // nothing outstanding: an echo, a late response, or a server's traffic
		}
		const Pending& head = m_queue.front();
		const uint8_t function = static_cast<uint8_t>(packet.function() & 0x7Fu);
		if (packet.address() != head.address || function != head.function) {
			return;   // Qt: ResponseRequestMismatch, the request keeps waiting
		}
		const bool exception = (packet.function() & 0x80u) != 0u;
		const uint8_t code = (exception && !packet.data().empty()) ? packet.data()[0] : 0u;
		m_response.stop();
		finish_head(RequestState::Completed, packet.data(), exception, code);
		schedule_next(inter_frame_delay_ms());
	}

	void on_response_timeout()
	{
		if (m_state != State::Waiting || m_queue.empty()) {
			return;
		}
		if (m_queue.front().attempts_left > 0) {
			m_state = State::Idle;          // retry the same request
			schedule_next(inter_frame_delay_ms());
			return;
		}
		finish_head(RequestState::Timeout, {}, false, 0u);
		schedule_next(inter_frame_delay_ms());
	}

	// Completes the head request: the queue pops, the gap the next frame must
	// respect is fixed, and only then does the handler run — so a send() from
	// inside it cannot shorten that gap.
	void finish_head(
			const RequestState state,
			const std::span<const uint8_t> data,
			const bool exception,
			const uint8_t exception_code)
	{
		Pending head = static_cast<Pending&&>(m_queue.front());
		m_queue.pop_front();
		m_state = State::Idle;
		const int gap = state == RequestState::Broadcast ? m_turnaround_delay_ms : inter_frame_delay_ms();
		m_earliest_next_ms = m_adapter.now_ms() + static_cast<uint32_t>(gap);
		if (head.handler) {
			Response response;
			response.state = state;
			response.address = head.address;
			response.function = head.function;
			response.exception = exception;
			response.exception_code = exception_code;
			response.data = data;
			response.attempts = head.attempts;
			head.handler(response);
		}
	}

	Adapter m_adapter;
	PortT& m_port;
	EndpointT& m_endpoint;
	QTimer m_response;
	QTimer m_schedule;
	std::deque<Pending> m_queue;
	State m_state = State::Idle;
	uint32_t m_earliest_next_ms = 0u;
	int m_response_timeout_ms = default_response_timeout_ms;
	int m_retries = default_retries;
	int m_turnaround_delay_ms = default_turnaround_delay_ms;
	int m_inter_frame_floor_ms = 0;
	int m_computed_inter_frame_ms = recommended_inter_frame_delay_ms;
};

} // namespace adapters::qt

#endif /* ADAPTERS_QT_RTU_CLIENT_H_ */
