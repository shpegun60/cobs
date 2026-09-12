/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * adapters/qt against a QIODevice stand-in for QSerialPort: the same three
 * signals, a recording write side and a feedable read side, so everything runs
 * on a real event loop without a COM port. SerialAdapter first — routing, the
 * busy/release contract, the 50 ms stale rule, the error-to-gap path, a clean
 * start and the bind/unbind/destructor lifecycle, for both protocols — and
 * then RtuClient, the QModbus-shaped master: one transaction at a time, a
 * queue behind it, response timeout with retries, request/response matching,
 * exception responses, broadcasts with their turnaround delay, and Qt's own
 * inter-frame delay arithmetic; then the lifecycle and failure cases a second
 * review asked for: the adapter's destructor leaves the endpoint unbound,
 * write and resource errors while a frame is leaving finish the request at
 * once, a partial write is a failed transmission, a reentrant send() from a
 * broadcast's handler cannot shorten the turnaround, and a lowered inter-frame
 * floor lowers the delay.
 */

#include "adapters/qt/RtuClient.h"
#include "adapters/qt/SerialAdapter.h"
#include "Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "Test.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QIODevice>
#include <QTimer>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

// A QSerialPort stand-in: written bytes are recorded and stay "in flight"
// until drain() reports them written; fed bytes arrive through readyRead.
class FakePort final : public QIODevice {
	Q_OBJECT

public:
	explicit FakePort(QObject* const parent = nullptr) : QIODevice(parent)
	{
		open(QIODevice::ReadWrite);
	}

	void feed(const std::span<const uint8_t> bytes)
	{
		m_rx.append(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
		emit readyRead();
	}

	// The OS took the pending bytes: QSerialPort emits bytesWritten then.
	void drain()
	{
		const qint64 pending = m_pending;
		m_pending = 0;
		if (pending != 0) {
			emit bytesWritten(pending);
		}
	}

	// Write completion and its notification are distinct OS/Qt events.
	void settle_without_signal() { m_pending = 0; }
	void notify_written() { emit bytesWritten(0); }

	void raise(const QSerialPort::SerialPortError error) { emit errorOccurred(error); }

	// Accept at most `limit` bytes per write (-1: everything), as a port whose
	// write buffer is full does.
	void set_write_limit(const qint64 limit) noexcept { m_write_limit = limit; }

	// clear() refuses and changes nothing, as a port whose device is gone does.
	void set_clear_fails(const bool fails) noexcept { m_clear_fails = fails; }
	void set_clear_error(const QSerialPort::SerialPortError error) noexcept { m_clear_error = error; }
	[[nodiscard]] unsigned clear_calls() const noexcept { return m_clear_calls; }

	// QSerialPort::clear(): drops what the OS holds, in the asked directions.
	bool clear(const QSerialPort::Directions directions = QSerialPort::AllDirections)
	{
		++m_clear_calls;
		if (m_clear_error != QSerialPort::NoError) {
			// Bound the negative test itself: a recursive error/clear loop
			// must fail an assertion, not overflow the host process's stack.
			if (m_clear_depth < 4u) {
				++m_clear_depth;
				emit errorOccurred(m_clear_error);
				--m_clear_depth;
			}
			return false;
		}
		if (m_clear_fails) {
			return false;
		}
		if (directions & QSerialPort::Input) {
			m_rx.clear();
		}
		if (directions & QSerialPort::Output) {
			m_pending = 0;
		}
		return true;
	}

	[[nodiscard]] std::vector<uint8_t> take_written()
	{
		std::vector<uint8_t> out(m_written.begin(), m_written.end());
		m_written.clear();
		return out;
	}

	// QSerialPort's rate, which RtuClient's inter-frame arithmetic reads.
	[[nodiscard]] qint32 baudRate() const noexcept { return m_baud; }
	void setBaudRate(const qint32 baud) noexcept { m_baud = baud; }

	[[nodiscard]] qint64 bytesToWrite() const override { return m_pending; }
	[[nodiscard]] bool isSequential() const override { return true; }
	[[nodiscard]] qint64 bytesAvailable() const override
	{
		return m_rx.size() + QIODevice::bytesAvailable();
	}

signals:
	void errorOccurred(QSerialPort::SerialPortError error);

protected:
	qint64 readData(char* const data, const qint64 max_size) override
	{
		const qint64 count = std::min<qint64>(max_size, m_rx.size());
		std::memcpy(data, m_rx.constData(), static_cast<std::size_t>(count));
		m_rx.remove(0, static_cast<qsizetype>(count));
		return count;
	}

	qint64 writeData(const char* const data, const qint64 size) override
	{
		const qint64 accepted = m_write_limit < 0 ? size : std::min(size, m_write_limit);
		m_written.append(data, static_cast<qsizetype>(accepted));
		m_pending += accepted;
		return accepted;
	}

private:
	QByteArray m_rx;
	QByteArray m_written;
	qint64 m_pending = 0;
	qint64 m_write_limit = -1;
	bool m_clear_fails = false;
	QSerialPort::SerialPortError m_clear_error = QSerialPort::NoError;
	unsigned m_clear_calls = 0u;
	unsigned m_clear_depth = 0u;
	qint32 m_baud = 115200;
};

namespace framing = modbus::rtu::framing;
using Client = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<>,
	framing::Standard<framing::Direction::Response>>;
using ClientAdapter = adapters::qt::SerialAdapter<Client, FakePort>;
using CobsLink = cobs::Endpoint<>;
using CobsAdapter = adapters::qt::SerialAdapter<CobsLink, FakePort>;
using RtuClient = adapters::qt::RtuClient<Client, FakePort>;
using adapters::qt::RequestState;
using adapters::qt::Response;

static_assert(ClientAdapter::framed);
static_assert(!CobsAdapter::framed);

// Runs the event loop for `ms` milliseconds: timers fire, queued signals run.
void pump(const int ms)
{
	QEventLoop loop;
	QTimer::singleShot(ms, &loop, &QEventLoop::quit);
	loop.exec();
}

using modbus_test::check;
using modbus_test::equal;
using modbus_test::group;
using modbus_test::make_adu;

} // namespace

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);

	group("RtuClientOverSerial");
	FakePort port;
	Client client;
	ClientAdapter adapter{port, client};
	check(adapter.bind() && adapter.bound(), "an RTU client with a framing policy binds to the port");
	const auto request_adu = make_adu(0x11u, 0x03u, std::vector<uint8_t>{0x00u, 0x6Bu, 0x00u, 0x03u});
	{
		auto request = client.make_message(0x11u, 0x03u);
		check(request.append_be<uint16_t>(0x006Bu) && request.append_be<uint16_t>(0x0003u) &&
		      client.send(request) == modbus::SendResult::Sent,
		      "a read-holding request is sent through QSerialPort::write");
	}
	check(equal(port.take_written(), request_adu), "the port received exactly the ADU with its CRC");
	check(client.tx_active() && port.bytesToWrite() == 8, "the block stays borrowed while the bytes are still leaving");
	{
		auto second = client.make_message(0x11u, 0x03u);
		check(second.append_be<uint16_t>(0u) && second.append_be<uint16_t>(1u) &&
		      client.send(second) == modbus::SendResult::Busy,
		      "a second request is refused while the first is on the wire");
	}
	port.drain();
	check(!client.tx_active(), "bytesWritten lets poll() release the block");

	const auto response_adu = make_adu(0x11u, 0x03u, std::vector<uint8_t>{0x06u, 0x02u, 0x2Bu, 0x00u, 0x00u, 0x00u, 0x64u});
	const std::span<const uint8_t> response_bytes{response_adu};
	port.feed(response_bytes.first(4u));
	check(client.assembling() && adapter.deadline_armed() && !client.has_packet(),
	      "the first cut of a response leaves a frame in flight with the silence timer running");
	port.feed(response_bytes.subspan(4u));
	check(!adapter.deadline_armed(), "the completing cut disarms the timer");
	{
		auto packet = client.pop_packet();
		check(packet && packet.address() == 0x11u && packet.function() == 0x03u &&
		      equal(packet.data(), std::span<const uint8_t>{response_adu}.subspan(2u, 7u)),
		      "the response is one packet with the six register bytes behind the count");
	}

	group("RequestFragmentReplay");
	{
		// The two requests in the H7S Qt-server timeout observations. Replay
		// every cut through OUR request-side adapter with a host-delivery gap,
		// independently of QtSerialBus's fragment timer and any COM bridge.
		using Server = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<>,
			framing::Standard<framing::Direction::Request>>;
		FakePort server_port;
		Server server;
		adapters::qt::SerialAdapter<Server, FakePort> server_adapter{server_port, server};
		check(server_adapter.bind(), "the request-side serial adapter binds");
		for (const uint8_t function : {uint8_t{0x03u}, uint8_t{0x04u}}) {
			const auto adu = make_adu(0x0au, function, std::vector<uint8_t>{0u, 0u, 0u, 10u});
			const std::span<const uint8_t> bytes{adu};
			for (std::size_t cut = 1u; cut < bytes.size(); ++cut) {
				server_port.feed(bytes.first(cut));
				check(!server.has_packet(), "a request prefix is not published");
				pump(10);
				server_port.feed(bytes.subspan(cut));
				auto packet = server.pop_packet();
				check(packet && equal(packet.adu(), bytes) && !server.has_packet() &&
					!server_adapter.deadline_armed() && server.framing_stats().stale_frames == 0u,
					"10 ms host gap: exactly one intact CRC-checked request at every cut");
			}
		}
		auto corrupt = make_adu(0x0au, 0x03u, std::vector<uint8_t>{0u, 0u, 0u, 10u});
		corrupt.back() ^= 0x01u;
		server_port.feed(std::span<const uint8_t>{corrupt}.first(5u));
		pump(10);
		server_port.feed(std::span<const uint8_t>{corrupt}.subspan(5u));
		check(!server.has_packet() && server.stats().rx.crc_errors == 1u,
			"tolerating delivery gaps does not accept a corrupt CRC");
		const auto intact = make_adu(0x0au, 0x03u, std::vector<uint8_t>{0u, 0u, 0u, 10u});
		server_port.feed(std::span<const uint8_t>{intact}.first(5u));
		pump(60);
		check(!server.assembling() && server.framing_stats().stale_frames == 1u,
			"a request that really stops expires at the existing 50 ms boundary");
		server_port.feed(intact);
		check(static_cast<bool>(server.pop_packet()), "a whole request after the orphan is delivered");
	}

	group("StaleFrame");
	port.feed(response_bytes.first(4u));
	check(client.assembling() && adapter.deadline_armed(), "half a response is in flight");
	pump(30);
	check(client.assembling(), "30 ms: alive");
	port.feed(response_bytes.subspan(4u, 1u));
	pump(30);
	check(client.assembling() && client.framing_stats().stale_frames == 0u,
	      "a byte 30 ms later restarts the silence: 60 ms after the first cut the frame is still alive");
	pump(60);
	check(!client.assembling() && !adapter.deadline_armed() && client.framing_stats().stale_frames == 1u,
	      "50 ms without a byte expires the frame, counted as stale");
	port.feed(response_bytes);
	check(static_cast<bool>(client.pop_packet()), "the next whole response is delivered");

	group("ErrorsAreGaps");
	port.feed(response_bytes.first(3u));
	port.raise(QSerialPort::ResourceError);
	check(!client.assembling() && !adapter.deadline_armed() && client.stats().rx.stream_gaps == 1u,
	      "a resource error drops the frame in flight as a stream gap and disarms the timer");
	port.raise(QSerialPort::TimeoutError);
	check(client.stats().rx.stream_gaps == 1u, "a timeout error is not a gap");
	port.feed(response_bytes);
	check(static_cast<bool>(client.pop_packet()), "and the next response is delivered");

	group("Lifecycle");
	{
		auto request = client.make_message(0x11u, 0x03u);
		check(request.append_be<uint16_t>(0u) && request.append_be<uint16_t>(1u) &&
		      client.send(request) == modbus::SendResult::Sent, "a request is on the wire");
	}
	check(!adapter.unbind() && adapter.bound(), "unbind() is refused while the frame has not left the port");
	port.drain();
	(void)port.take_written();
	port.feed(response_bytes.first(3u));
	check(client.assembling(), "a frame is in flight");
	check(adapter.unbind() && !adapter.bound(), "unbind() succeeds once the port is idle");
	check(!client.assembling() && client.framing_stats().stale_frames == 1u,
	      "detaching discards the frame in flight uncounted");
	port.feed(response_bytes);
	check(!client.has_packet(), "after unbind() nothing reaches the endpoint");
	check(adapter.bind(), "bind() works again");
	// The bytes fed while nothing was bound are still in the port, exactly as
	// the OS would have buffered them; the next delivery would hand them over
	// together with the new ones.
	adapter.discard_incoming();
	port.feed(response_bytes);
	check(static_cast<bool>(client.pop_packet()) && !client.has_packet(),
	      "and delivers exactly the response fed after it");
	// One adapter per port: a Qt connection is added, not substituted, so the
	// first adapter releases the port before another one takes it.
	check(adapter.unbind() && !adapter.bound(), "the bound adapter releases the port");
	{
		ClientAdapter temporary{port, client};
		check(temporary.bind() && temporary.bound(), "a second adapter takes the idle port");
		port.feed(response_bytes.first(3u));
		check(client.assembling(), "a frame is in flight through it");
	}
	check(!client.assembling(), "the dying adapter discarded the frame in flight");
	port.feed(response_bytes);
	check(!client.has_packet(), "with no adapter bound nothing reaches the endpoint");
	check(adapter.bind(), "the first adapter binds again");
	adapter.discard_incoming();          // whatever arrived unbound is not ours
	port.feed(response_bytes);
	check(static_cast<bool>(client.pop_packet()) && !client.has_packet(),
	      "and the port belongs to it again");

	group("DiscardIncoming");
	port.feed(response_bytes.first(4u));
	port.feed(response_bytes.subspan(4u, 2u));      // a second delivery, still incomplete
	check(client.assembling() && adapter.deadline_armed(), "half a response is in flight");
	adapter.discard_incoming();
	check(!client.assembling() && !adapter.deadline_armed() &&
	      client.framing_stats().stale_frames == 1u,
	      "discard_incoming() drops the frame in flight without counting it stale");
	port.feed(response_bytes);
	{
		auto packet = client.pop_packet();
		check(packet && packet.function() == 0x03u,
		      "the next response is whole: nothing was glued onto the abandoned fragment");
	}
	port.feed(response_bytes.first(4u));
	adapter.discard_incoming();
	port.feed(response_bytes.subspan(4u));          // the tail of the discarded frame
	check(!client.has_packet() && client.framing_stats().unsupported_function >= 1u,
	      "the tail of a discarded frame is refused as an unknown function, not delivered as a packet");
	port.feed(response_bytes);
	check(static_cast<bool>(client.pop_packet()), "and the stream recovers on the next whole response");
	// A port that refuses clear(): what it buffers is read away instead, and
	// the bytes that were already delivered are discarded all the same.
	port.set_clear_fails(true);
	port.feed(response_bytes.first(4u));
	adapter.discard_incoming();
	check(!client.assembling() && port.bytesAvailable() == 0,
	      "discard_incoming() on a port that cannot clear still drops the frame in flight and empties the input");
	port.set_clear_fails(false);
	port.feed(response_bytes);
	check(static_cast<bool>(client.pop_packet()) && !client.has_packet(), "and the next response is delivered whole");

	group("ServiceHandlerBelongsToTheBinding");
	{
		struct Owner final {
			unsigned served = 0;
			void service() noexcept { ++served; }
		};
		Owner owner;
		adapter.set_service_handler(ClientAdapter::ServiceHandler{tiny::bind<&Owner::service>(owner)});
		port.feed(response_bytes);
		check(owner.served >= 1u && static_cast<bool>(client.pop_packet()), "an installed service handler is raised on a delivery");
		const unsigned before = owner.served;
		check(adapter.unbind(), "unbind()");
		check(adapter.bind(), "rebind without installing a handler again");
		port.feed(response_bytes);
		check(owner.served == before && static_cast<bool>(client.pop_packet()),
		      "the handler did not survive the binding: nothing is raised into an owner that may be gone");
		adapter.set_service_handler(ClientAdapter::ServiceHandler{tiny::bind<&Owner::service>(owner)});
		port.feed(response_bytes);
		check(owner.served == before + 1u && static_cast<bool>(client.pop_packet()), "installed again, it is raised again");
		adapter.set_service_handler(ClientAdapter::ServiceHandler{});
	}

	group("AdapterDestructorUnbinds");
	check(adapter.unbind(), "the long-lived adapter releases the port and the endpoint");
	{
		ClientAdapter scoped{port, client};
		check(scoped.bind(), "a scoped adapter binds");
		auto request = client.make_message(0x11u, 0x03u);
		check(request.append_be<uint16_t>(0u) && request.append_be<uint16_t>(1u) &&
		      client.send(request) == modbus::SendResult::Sent && client.tx_active() && port.bytesToWrite() == 8,
		      "a request is on its way out through it");
	}   // dies with the frame still leaving
	check(!client.tx_active(), "the dying adapter released the block, whose bytes Qt already holds");
	check(port.bytesToWrite() == 8, "and did not drop the bytes the port still carries");
	port.drain();
	(void)port.take_written();
	{
		auto request = client.make_message(0x11u, 0x03u);
		check(request.append_be<uint16_t>(0u) && request.append_be<uint16_t>(1u) &&
		      client.send(request) == modbus::SendResult::Unbound,
		      "send() after the adapter's death is refused as Unbound: no call into a dead adapter");
	}
	check(adapter.bind(), "the long-lived adapter binds again");

	group("QtRtuClientTiming");
	{
		FakePort timing_port;
		Client timing_link;
		RtuClient timing_client{timing_port, timing_link};
		check(timing_client.response_timeout_ms() == 1000 && timing_client.retries() == 3 &&
		      timing_client.turnaround_delay_ms() == 100,
		      "the client starts with QModbus's defaults: 1000 ms, three retries, 100 ms turnaround");
		timing_port.setBaudRate(115200);
		timing_client.update_timing_from_port();
		check(timing_client.inter_frame_delay_ms() == 2,
		      "at and above 19200 baud the inter-frame delay is Qt's flat 2 ms");
		timing_port.setBaudRate(9600);
		timing_client.update_timing_from_port();
		check(timing_client.inter_frame_delay_ms() == 5,
		      "at 9600 baud it is 3.5 characters of eleven bits, rounded up: 5 ms");
		timing_port.setBaudRate(1200);
		timing_client.update_timing_from_port();
		check(timing_client.inter_frame_delay_ms() == 33, "at 1200 baud, 33 ms");
		timing_client.set_inter_frame_delay_ms(80);
		check(timing_client.inter_frame_delay_ms() == 80, "an application floor wins when it is longer");
		timing_port.setBaudRate(115200);
		timing_client.update_timing_from_port();
		check(timing_client.inter_frame_delay_ms() == 80, "and survives a recomputation");
		timing_client.set_inter_frame_delay_ms(0);
		check(timing_client.inter_frame_delay_ms() == 2, "lowering the floor lowers the delay back to the computed value");
	}

	// One port and one endpoint for the transaction groups. The client owns its
	// adapter, so the earlier one is unbound first.
	check(adapter.unbind(), "the low-level adapter releases the port");
	FakePort client_port;
	Client client_link;
	RtuClient rtu{client_port, client_link};
	rtu.set_response_timeout_ms(40);
	rtu.set_retries(1);
	rtu.set_inter_frame_delay_ms(1);
	rtu.set_turnaround_delay_ms(20);
	check(rtu.bind() && rtu.bound(), "the client binds its adapter to the port");

	// The peer: answers the request currently on the wire, optionally wrongly.
	const auto answer = [&client_port](const uint8_t address, const uint8_t function,
	                                   const std::vector<uint8_t>& data) {
		(void)client_port.take_written();
		client_port.drain();
		client_port.feed(make_adu(address, function, data));
	};

	group("QtRtuClientTransaction");
	{
		unsigned completed = 0;
		std::vector<uint8_t> payload;
		const std::vector<uint8_t> request_body{0x00u, 0x6Bu, 0x00u, 0x03u};
		check(rtu.send(0x11u, 0x03u, request_body, RtuClient::Handler{
			[&completed, &payload](const Response& response) {
				++completed;
				if (response.state == RequestState::Completed) {
					payload.assign(response.data.begin(), response.data.end());
				}
			}}), "a read-holding request is queued");
		check(rtu.pending() == 1u && completed == 0u, "nothing happens before the event loop runs");
		pump(10);
		check(equal(client_port.take_written(),
		            make_adu(0x11u, 0x03u, request_body)),
		      "after the inter-frame delay the ADU is on the wire, CRC and all");
		client_port.drain();
		const std::vector<uint8_t> response_body{0x06u, 0x02u, 0x2Bu, 0x00u, 0x00u, 0x00u, 0x64u};
		client_port.feed(make_adu(0x11u, 0x03u, response_body));
		check(completed == 1u && equal(payload, response_body) && rtu.pending() == 0u,
		      "the matching response completes the request with its data");
	}

	group("QtRtuClientMatching");
	{
		unsigned completed = 0;
		RequestState state = RequestState::Cancelled;
		check(rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 1u, 0u, 1u}, RtuClient::Handler{
			[&completed, &state](const Response& response) { ++completed; state = response.state; }}),
		      "a request is queued");
		pump(10);
		(void)client_port.take_written();
		client_port.drain();
		client_port.feed(make_adu(0x12u, 0x03u, std::vector<uint8_t>{0x02u, 0x00u, 0x01u}));
		check(completed == 0u, "a response from another server address is ignored");
		client_port.feed(make_adu(0x11u, 0x04u, std::vector<uint8_t>{0x02u, 0x00u, 0x01u}));
		check(completed == 0u, "a response with another function code is ignored");
		client_port.feed(make_adu(0x11u, 0x03u, std::vector<uint8_t>{0x02u, 0x00u, 0x01u}));
		check(completed == 1u && state == RequestState::Completed,
		      "the response that matches address and function completes the request");
	}

	group("QtRtuClientException");
	{
		Response seen;
		unsigned completed = 0;
		check(rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0xFFu, 0xFFu, 0x00u, 0x01u}, RtuClient::Handler{
			[&seen, &completed](const Response& response) { seen = response; ++completed; }}),
		      "a request for an illegal address is queued");
		pump(10);
		answer(0x11u, 0x83u, std::vector<uint8_t>{0x02u});
		check(completed == 1u && seen.state == RequestState::Completed && seen.exception &&
		      seen.exception_code == 0x02u && seen.function == 0x03u,
		      "an exception response completes its own request, with the code and the plain function");
	}

	group("QtRtuClientTimeoutAndRetries");
	{
		unsigned completed = 0;
		Response seen;
		check(rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 0u, 0u, 1u}, RtuClient::Handler{
			[&completed, &seen](const Response& response) { ++completed; seen = response; }}),
		      "a request to a silent server is queued");
		pump(10);
		check(!client_port.take_written().empty() && completed == 0u, "the first attempt is on the wire");
		client_port.drain();
		pump(60);
		check(!client_port.take_written().empty() && completed == 0u,
		      "the timeout retries instead of failing: a second attempt goes out");
		client_port.drain();
		pump(60);
		check(completed == 1u && seen.state == RequestState::Timeout && seen.attempts == 2u,
		      "with one retry configured, two attempts and then a timeout");

		// A retry that the server does answer.
		completed = 0;
		check(rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 0u, 0u, 1u}, RtuClient::Handler{
			[&completed, &seen](const Response& response) { ++completed; seen = response; }}),
		      "another request is queued");
		pump(10);
		(void)client_port.take_written();
		client_port.drain();
		pump(60);                                   // let the first attempt time out
		answer(0x11u, 0x03u, std::vector<uint8_t>{0x02u, 0x12u, 0x34u});
		check(completed == 1u && seen.state == RequestState::Completed && seen.attempts == 2u,
		      "the answer to the second attempt completes the request");
	}

	group("QtRtuClientQueue");
	{
		std::vector<uint8_t> order;
		const auto record = [&order](const uint8_t tag) {
			return RtuClient::Handler{[&order, tag](const Response&) { order.push_back(tag); }};
		};
		check(rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 1u, 0u, 1u}, record(1u)) &&
		      rtu.send(0x11u, 0x04u, std::vector<uint8_t>{0u, 2u, 0u, 1u}, record(2u)) &&
		      rtu.pending() == 2u,
		      "two requests are queued");
		pump(10);
		const auto first_on_wire = client_port.take_written();
		check(first_on_wire.size() == 8u && first_on_wire[1] == 0x03u,
		      "only the first is on the wire: one transaction at a time");
		client_port.drain();
		client_port.feed(make_adu(0x11u, 0x03u, std::vector<uint8_t>{0x02u, 0x00u, 0x07u}));
		check(order.size() == 1u && order[0] == 1u && rtu.pending() == 1u,
		      "its response completes it and leaves the second queued");
		pump(10);
		const auto second_on_wire = client_port.take_written();
		check(second_on_wire.size() == 8u && second_on_wire[1] == 0x04u,
		      "the second goes out after the inter-frame delay");
		client_port.drain();
		client_port.feed(make_adu(0x11u, 0x04u, std::vector<uint8_t>{0x02u, 0x00u, 0x09u}));
		check(order.size() == 2u && order[1] == 2u && rtu.idle(),
		      "and completes in turn, leaving the client idle");
	}

	group("QtRtuClientBroadcast");
	{
		unsigned completed = 0;
		Response seen;
		check(rtu.send(0x00u, 0x06u, std::vector<uint8_t>{0x00u, 0x01u, 0x00u, 0x03u}, RtuClient::Handler{
			[&completed, &seen](const Response& response) { ++completed; seen = response; }}),
		      "a broadcast write is queued");
		pump(10);
		check(!client_port.take_written().empty() && completed == 0u, "it is written");
		client_port.drain();
		check(completed == 1u && seen.state == RequestState::Broadcast,
		      "and completes as a broadcast once the bytes have left: nobody answers address 0");
	}

	group("QtRtuClientTransportErrors");
	pump(25);   // the previous group ended with a broadcast: its turnaround is a deadline the next frame respects
	{
		unsigned completed = 0;
		Response seen;
		const auto capture = [&completed, &seen]() {
			return RtuClient::Handler{[&completed, &seen](const Response& response) { ++completed; seen = response; }};
		};
		check(rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 1u, 0u, 1u}, capture()), "a request is queued");
		pump(10);
		check(client_port.bytesToWrite() == 8 && completed == 0u, "it is leaving the port");
		client_port.raise(QSerialPort::WriteError);
		check(completed == 1u && seen.state == RequestState::WriteError,
		      "a write error finishes it as WriteError at once, not after the response timeout");
		check(client_port.bytesToWrite() == 0 && !client_link.tx_active(),
		      "the port's output was cleared and the block released");
		(void)client_port.take_written();
		completed = 0;
		check(rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 1u, 0u, 1u}, capture()), "the next request is queued");
		pump(10);
		check(!client_port.take_written().empty(), "and goes out: the queue moved on");
		client_port.drain();
		client_port.feed(make_adu(0x11u, 0x03u, std::vector<uint8_t>{0x02u, 0x00u, 0x01u}));
		check(completed == 1u && seen.state == RequestState::Completed, "and completes normally");

		completed = 0;
		check(rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 1u, 0u, 1u}, capture()), "another request is queued");
		pump(10);
		check(client_port.bytesToWrite() == 8, "it is leaving");
		client_port.raise(QSerialPort::ResourceError);
		check(completed == 1u && seen.state == RequestState::WriteError && !client_link.tx_active() &&
		      client_port.bytesToWrite() == 0,
		      "a resource error while sending finishes the request as WriteError with the block released");
		(void)client_port.take_written();

		// The same write error on a port that cannot clear its output (a device
		// that is gone): the block must come back all the same, or every later
		// send() would be Busy forever.
		completed = 0;
		check(rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 1u, 0u, 1u}, capture()), "a request is queued");
		pump(10);
		check(client_port.bytesToWrite() == 8, "it is leaving");
		client_port.set_clear_fails(true);
		client_port.raise(QSerialPort::WriteError);
		check(completed == 1u && seen.state == RequestState::WriteError, "the write error finishes the request");
		check(!client_link.tx_active(), "and the endpoint's block is released although the port kept its bytes");
		check(client_port.bytesToWrite() == 8, "which are the port's problem now");
		client_port.set_clear_fails(false);
		client_port.drain();
		(void)client_port.take_written();
		completed = 0;
		check(rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 1u, 0u, 1u}, capture()), "the next request is queued");
		pump(10);
		check(client_port.take_written().size() == 8u, "and is sent, not refused as Busy");
		client_port.drain();
		client_port.feed(make_adu(0x11u, 0x03u, std::vector<uint8_t>{0x02u, 0x00u, 0x01u}));
		check(completed == 1u && seen.state == RequestState::Completed, "and completes");

		completed = 0;
		client_port.set_write_limit(3);
		check(rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 1u, 0u, 1u}, capture()), "a request meets a port that takes 3 bytes");
		pump(10);
		check(completed == 1u && seen.state == RequestState::WriteError,
		      "a partial write is a write error, not a retry behind half a frame");
		check(client_port.bytesToWrite() == 0 && !client_link.tx_active(),
		      "what the port still buffered was cleared and the block released");
		client_port.set_write_limit(-1);
		(void)client_port.take_written();
		check(rtu.idle(), "the client is idle again");
	}

	group("QtRtuClientTurnaroundIsKept");
	{
		unsigned broadcasts = 0, completed = 0;
		check(rtu.send(0x00u, 0x06u, std::vector<uint8_t>{0x00u, 0x01u, 0x00u, 0x03u}, RtuClient::Handler{
			[&rtu, &broadcasts, &completed](const Response& response) {
				if (response.state == RequestState::Broadcast) {
					++broadcasts;
					// the usual polling-loop shape: the next request from inside the callback
					(void)rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 1u, 0u, 1u}, RtuClient::Handler{
						[&completed](const Response&) { ++completed; }});
				}
			}}), "a broadcast whose handler queues the next request");
		pump(10);
		check(!client_port.take_written().empty(), "the broadcast is written");
		client_port.drain();
		check(broadcasts == 1u && rtu.pending() == 1u, "it completed and the handler's request is queued");
		pump(10);
		check(client_port.take_written().empty(), "10 ms later the next request has not left: the 20 ms turnaround stands");
		pump(15);
		check(!client_port.take_written().empty(), "after the turnaround it goes out");
		client_port.drain();
		client_port.feed(make_adu(0x11u, 0x03u, std::vector<uint8_t>{0x02u, 0x00u, 0x01u}));
		check(completed == 1u, "and completes");

		// A redundant bind() on a bound client must not forgive the debt either.
		completed = 0;
		check(rtu.send(0x00u, 0x06u, std::vector<uint8_t>{0x00u, 0x02u, 0x00u, 0x01u}, RtuClient::Handler{
			[&completed](const Response&) { ++completed; }}), "another broadcast is queued");
		pump(10);
		(void)client_port.take_written();
		client_port.drain();
		check(completed == 1u, "it completed: the 20 ms turnaround starts now");
		check(rtu.bind() && rtu.bound(), "bind() on the bound client is a no-op that returns true");
		completed = 0;
		check(rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 1u, 0u, 1u}, RtuClient::Handler{
			[&completed](const Response&) { ++completed; }}), "a request follows");
		pump(10);
		check(client_port.take_written().empty(), "10 ms later it has not left: the redundant bind() did not erase the turnaround");
		pump(15);
		check(!client_port.take_written().empty(), "after the turnaround it goes out");
		client_port.drain();
		client_port.feed(make_adu(0x11u, 0x03u, std::vector<uint8_t>{0x02u, 0x00u, 0x01u}));
		check(completed == 1u, "and completes");
	}

	group("QtRtuClientLifecycle");
	{
		unsigned cancelled = 0;
		check(rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 1u, 0u, 1u}, RtuClient::Handler{
			[&cancelled](const Response& response) {
				cancelled += response.state == RequestState::Cancelled ? 1u : 0u;
			}}), "a request is queued");
		check(rtu.unbind() && !rtu.bound() && cancelled == 1u && rtu.pending() == 0u,
		      "unbind() cancels the queued request and detaches");
		pump(10);
		check(client_port.take_written().empty(), "and nothing is written afterwards");
		check(rtu.bind(), "the client binds again");
		unsigned completed = 0;
		check(rtu.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 1u, 0u, 1u}, RtuClient::Handler{
			[&completed](const Response&) { ++completed; }}), "and serves a further request");
		pump(10);
		answer(0x11u, 0x03u, std::vector<uint8_t>{0x02u, 0x00u, 0x05u});
		check(completed == 1u, "which completes normally");
		check(rtu.unbind(), "the client releases the port");
	}

	group("QtRtuClientRxBeforeWriteNotification");
	for (const bool settled : {false, true}) {
		FakePort reordered_port;
		Client reordered_link;
		RtuClient reordered{reordered_port, reordered_link};
		reordered.set_response_timeout_ms(40);
		reordered.set_retries(0);
		unsigned completed = 0;
		Response seen;
		check(reordered.bind() && reordered.send(0x11u, 0x03u,
			std::vector<uint8_t>{0u, 0u, 0u, 1u}, RtuClient::Handler{
			[&](const Response& response) { ++completed; seen = response; }}), "reordered-event request queued");
		pump(10);
		(void)reordered_port.take_written();
		if (settled) {
			reordered_port.settle_without_signal();
		}
		reordered_port.feed(make_adu(0x11u, 0x03u, std::vector<uint8_t>{2u, 0u, 7u}));
		reordered_port.settle_without_signal();
		reordered_port.notify_written();
		check(completed == 1u && seen.state == RequestState::Completed && seen.attempts == 1u,
			"RX before bytesWritten is retained, including when TX state becomes visible only later");
		pump(60);
		check(completed == 1u && reordered.idle(), "no delayed timeout follows the completed early response");
	}

	group("QtRtuClientBlockedOutput");
	{
		FakePort blocked_port;
		Client blocked_link;
		RtuClient blocked{blocked_port, blocked_link};
		blocked.set_response_timeout_ms(15);
		blocked.set_retries(0);
		unsigned completed = 0;
		Response seen;
		check(blocked.bind() && blocked.send(0x11u, 0x03u,
			std::vector<uint8_t>{0u, 0u, 0u, 1u}, RtuClient::Handler{
			[&](const Response& response) { ++completed; seen = response; }}), "a write that never drains is queued");
		pump(40);
		check(completed == 1u && seen.state == RequestState::WriteError && seen.attempts == 1u &&
			blocked.idle() && !blocked_link.tx_active() && blocked_port.bytesToWrite() == 0,
			"a stalled accepted write terminates and returns its block within the write deadline");
		(void)blocked_port.take_written();
		check(blocked.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 1u, 0u, 1u},
			RtuClient::Handler{[&](const Response& response) { ++completed; seen = response; }}),
			"the next transaction can be queued after a stalled write");
		pump(5);
		blocked_port.drain();
		blocked_port.feed(make_adu(0x11u, 0x03u, std::vector<uint8_t>{2u, 0u, 8u}));
		check(completed == 2u && seen.state == RequestState::Completed && blocked.idle(),
			"the next transaction completes after the failed write");
	}

	group("QtRtuClientBusyIsNotAnAttempt");
	{
		FakePort busy_port;
		Client busy_link;
		RtuClient busy_client{busy_port, busy_link};
		busy_client.set_response_timeout_ms(50);
		busy_client.set_retries(0);
		(void)busy_port.write("old", 3);
		unsigned completed = 0;
		Response seen;
		check(busy_client.bind() && busy_client.send(0x11u, 0x03u,
			std::vector<uint8_t>{0u, 0u, 0u, 1u}, RtuClient::Handler{
			[&](const Response& response) { ++completed; seen = response; }}), "request waits for an already busy port");
		pump(10);
		busy_port.drain();
		(void)busy_port.take_written();
		pump(5);
		busy_port.drain();
		busy_port.feed(make_adu(0x11u, 0x03u, std::vector<uint8_t>{2u, 0u, 9u}));
		check(completed == 1u && seen.state == RequestState::Completed && seen.attempts == 1u,
			"Busy polls do not consume attempts or retry budget");
	}
	{
		FakePort busy_port;
		Client busy_link;
		RtuClient busy_client{busy_port, busy_link};
		busy_client.set_response_timeout_ms(15);
		(void)busy_port.write("old", 3);
		unsigned completed = 0;
		Response seen;
		check(busy_client.bind() && busy_client.send(0x11u, 0x03u,
			std::vector<uint8_t>{0u, 0u, 0u, 1u}, RtuClient::Handler{
			[&](const Response& response) { ++completed; seen = response; }}), "permanently busy port request queued");
		pump(40);
		check(completed == 1u && seen.state == RequestState::WriteError && seen.attempts == 0u && busy_client.idle(),
			"a request cannot poll Busy forever or pretend it was transmitted");
	}

	group("QtRtuClientCancellationSnapshot");
	{
		FakePort cancel_port;
		Client cancel_link;
		RtuClient cancel_client{cancel_port, cancel_link};
		unsigned cancelled = 0;
		unsigned completed = 0;
		bool rebound = false;
		check(cancel_client.bind() && cancel_client.send(0x11u, 0x03u,
			std::vector<uint8_t>{0u, 0u, 0u, 1u}, RtuClient::Handler{[&](const Response& response) {
				cancelled += response.state == RequestState::Cancelled ? 1u : 0u;
				rebound = cancel_client.bind() && cancel_client.send(0x11u, 0x04u,
					std::vector<uint8_t>{0u, 0u, 0u, 1u}, RtuClient::Handler{[&](const Response& next) {
						completed += next.state == RequestState::Completed ? 1u : 0u;
						cancelled += next.state == RequestState::Cancelled ? 1u : 0u;
					}});
			}}), "cancellation handler is allowed to start a new binding");
		check(cancel_client.unbind() && rebound && cancelled == 1u && cancel_client.pending() == 1u,
			"unbind cancels its original queue, not a request queued by a rebound handler");
		pump(5);
		cancel_port.drain();
		cancel_port.feed(make_adu(0x11u, 0x04u, std::vector<uint8_t>{2u, 0u, 10u}));
		check(completed == 1u && cancelled == 1u && cancel_client.idle(), "the rebound request survives and completes exactly once");
	}

	group("QtRtuClientWriteDeadlineRecovery");
	{
		FakePort recovery_port;
		Client recovery_link;
		RtuClient recovery{recovery_port, recovery_link};
		recovery.set_response_timeout_ms(15);
		recovery.set_retries(0);
		std::vector<RequestState> states;
		std::vector<unsigned> attempts;
		const auto capture = [&]() {
			return RtuClient::Handler{[&](const Response& response) {
				states.push_back(response.state);
				attempts.push_back(response.attempts);
			}};
		};
		check(recovery.bind() && recovery.send(0x11u, 0x03u,
			std::vector<uint8_t>{0u, 0u, 0u, 1u}, capture()) &&
			recovery.send(0x11u, 0x04u, std::vector<uint8_t>{0u, 0u, 0u, 1u}, capture()),
			"two requests queued before a failed output clear");
		recovery_port.set_clear_fails(true);
		pump(60);
		check(states == std::vector<RequestState>{RequestState::WriteError, RequestState::WriteError} &&
			attempts == std::vector<unsigned>{1u, 0u} && recovery.idle() && !recovery_link.tx_active() &&
			recovery_port.bytesToWrite() == 8,
			"failed clear cannot retain an endpoint block or wedge the following queued request");
		recovery_port.set_clear_fails(false);
		recovery_port.drain();
		check(recovery.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 0u, 0u, 1u}, capture()),
			"the recovered port accepts new work");
		pump(5);
		recovery_port.settle_without_signal();
		pump(20);   // no bytesWritten: the write deadline polls the observable port state
		check(states.size() == 2u && !recovery_link.tx_active(),
			"lost bytesWritten alone is not a write error when the port has actually drained");
		recovery_port.feed(make_adu(0x11u, 0x03u, std::vector<uint8_t>{2u, 0u, 8u}));
		check(states.size() == 3u && states.back() == RequestState::Completed && attempts.back() == 1u && recovery.idle(),
			"a fresh response window follows the polled write completion");
		pump(40);
		check(states.size() == 3u, "stale write/response timers cannot complete a request twice");
	}

	group("QtRtuClientAbandonedInput");
	{
		FakePort stale_port;
		Client stale_link;
		RtuClient stale_client{stale_port, stale_link};
		stale_client.set_response_timeout_ms(15);
		stale_client.set_retries(0);
		unsigned completed = 0u;
		Response seen;
		const auto capture = [&]() {
			return RtuClient::Handler{[&](const Response& response) { ++completed; seen = response; }};
		};
		const auto reply = make_adu(0x11u, 0x03u, std::vector<uint8_t>{2u, 0u, 8u});
		stale_link.consume(reply);   // a packet predating the client binding
		check(stale_link.has_packet() && stale_client.bind() && stale_client.send(0x11u, 0x03u,
			std::vector<uint8_t>{0u, 0u, 0u, 1u}, capture()), "new session starts with a pre-existing decoded packet");
		pump(5);
		stale_port.drain();
		check(!stale_link.has_packet() && completed == 0u, "a packet decoded before the request cannot answer it");
		stale_port.feed(std::span<const uint8_t>{reply}.first(4u));
		check(stale_link.assembling() && stale_client.adapter().deadline_armed(), "a partial response is now in flight");
		pump(20);
		check(completed == 1u && seen.state == RequestState::Timeout && !stale_link.assembling() &&
			!stale_client.adapter().deadline_armed() && stale_link.framing_stats().stale_frames == 0u,
			"request timeout immediately discards its partial response, without waiting for the 50 ms stale timer");
		check(stale_client.send(0x11u, 0x03u, std::vector<uint8_t>{0u, 0u, 0u, 1u}, capture()),
			"another request follows the partial-response timeout");
		pump(5);
		stale_port.drain();
		stale_port.feed(reply);
		check(completed == 2u && seen.state == RequestState::Completed && stale_client.idle(),
			"the next complete response is not appended to an abandoned prefix");
	}

	group("QtRtuClientMaximumRetryCount");
	{
		FakePort retry_port;
		Client retry_link;
		RtuClient retry_client{retry_port, retry_link};
		retry_client.set_response_timeout_ms(15);
		retry_client.set_retries(std::numeric_limits<int>::max());
		unsigned completed = 0u;
		Response seen;
		check(retry_client.bind() && retry_client.send(0x11u, 0x03u,
			std::vector<uint8_t>{0u, 0u, 0u, 1u}, RtuClient::Handler{
			[&](const Response& response) { ++completed; seen = response; }}), "INT_MAX retries can be queued without signed overflow");
		pump(5);
		retry_port.drain();
		pump(20);
		retry_port.drain();
		retry_port.feed(make_adu(0x11u, 0x03u, std::vector<uint8_t>{2u, 0u, 8u}));
		check(completed == 1u && seen.state == RequestState::Completed && seen.attempts == 2u,
			"the maximum retry setting retains its budget and completes on the second attempt");
	}

	group("QtSerialAdapterErrorDuringClear");
	{
		FakePort error_port;
		Client error_link;
		ClientAdapter error_adapter{error_port, error_link};
		unsigned services = 0u;
		check(error_adapter.bind(), "adapter for nested port errors binds");
		error_adapter.set_service_handler(ClientAdapter::ServiceHandler{[&]() { ++services; }});
		auto message = error_link.make_message(0x11u, 0x03u, 4u);
		check(message && message.append_be<uint32_t>(1u) && error_link.send(message) == modbus::SendResult::Sent,
			"a frame is borrowed before a device failure");
		error_port.set_clear_error(QSerialPort::ResourceError);
		const unsigned before = error_port.clear_calls();
		error_port.raise(QSerialPort::ResourceError);
		check(error_port.clear_calls() == before + 1u && services == 1u && error_link.stats().rx.stream_gaps == 1u &&
			!error_link.tx_active() && error_adapter.last_transport_error() == adapters::qt::TransportError::Resource,
			"a clear() error cannot recursively clear the port, multiply gaps or notify partial recovery");
		error_port.set_clear_error(QSerialPort::NoError);
		error_port.drain();
	}

	group("QtRtuClientErrorDuringTimeout");
	{
		FakePort error_port;
		Client error_link;
		RtuClient error_client{error_port, error_link};
		error_client.set_response_timeout_ms(15);
		error_client.set_retries(0);
		unsigned completed = 0u;
		Response seen;
		const auto capture = [&]() {
			return RtuClient::Handler{[&](const Response& response) { ++completed; seen = response; }};
		};
		check(error_client.bind() && error_client.send(0x11u, 0x03u,
			std::vector<uint8_t>{0u, 0u, 0u, 1u}, capture()), "a request is queued before the port disappears");
		pump(5);
		error_port.drain();
		error_port.set_clear_error(QSerialPort::ResourceError);
		pump(30);
		check(completed == 1u && seen.state == RequestState::WriteError && seen.attempts == 1u && error_client.idle(),
			"a resource error inside timeout cleanup completes once without reading a popped queue head");
		error_port.set_clear_error(QSerialPort::NoError);
		check(error_client.send(0x11u, 0x04u, std::vector<uint8_t>{0u, 0u, 0u, 1u}, capture()), "a request follows the cleanup error");
		pump(5);
		error_port.drain();
		error_port.feed(make_adu(0x11u, 0x04u, std::vector<uint8_t>{2u, 0u, 8u}));
		check(completed == 2u && seen.state == RequestState::Completed && error_client.idle(),
			"the queue recovers after an error nested in timeout cleanup");
	}

	group("CobsOverSerial");
	FakePort cobs_port;
	CobsLink cobs_link;
	CobsAdapter cobs_adapter{cobs_port, cobs_link};
	check(cobs_adapter.bind(), "a COBS endpoint binds to the port through the same adapter");
	const std::string body = "hello";
	{
		auto message = cobs_link.make_message(body.size());
		check(message.append_bytes(std::span<const uint8_t>{
				reinterpret_cast<const uint8_t*>(body.data()), body.size()}) &&
		      cobs_link.send(message) == cobs::SendResult::Sent, "a COBS message is sent");
	}
	const auto frame = cobs_port.take_written();
	check(!frame.empty() && frame.back() == 0u, "the port received one delimited COBS frame");
	cobs_port.drain();
	check(!cobs_link.tx_active(), "bytesWritten releases the COBS block");
	const std::span<const uint8_t> frame_bytes{frame};
	cobs_port.feed(frame_bytes.first(2u));
	cobs_port.feed(frame_bytes.subspan(2u));
	{
		auto packet = cobs_link.pop_packet();
		check(packet && std::string(packet.data().begin(), packet.data().end()) == body,
		      "the frame fed back in two cuts is one packet with the payload");
	}
	check(!cobs_adapter.deadline_armed(), "COBS never arms a silence timer");

	group("CobsDiscontinuities");
	const auto check_cobs_discontinuity = []<class Integrity>() {
		using Link = cobs::Endpoint<wire::Pool<3, 1>, cobs::Format<Integrity>>;
		using Adapter = adapters::qt::SerialAdapter<Link, FakePort>;
		for (unsigned action = 0; action < 4u; ++action) {
			FakePort gap_port;
			Link link;
			auto gap_adapter = std::make_unique<Adapter>(gap_port, link);
			check(gap_adapter->bind(), "COBS discontinuity fixture binds");
			const std::vector<uint8_t> payload{'A', 'B', 'C', 'D'};
			auto message = link.make_message(payload.size());
			check(message.append_bytes(payload) && link.send(message) == wire::SendResult::Sent,
			      "COBS discontinuity fixture builds a real frame");
			const auto wire = gap_port.take_written();
			gap_port.drain();
			const std::span<const uint8_t> bytes{wire};
			gap_port.feed(bytes); // a completed packet must survive every discontinuity
			gap_port.feed(bytes.first(bytes.size() - 2u)); // an allocated, incomplete packet
			check(link.storage().rx_available() == 1u, "one COBS packet is queued and one is building");
			if (action < 2u) {
				gap_port.set_clear_fails(action == 1u);
				gap_adapter->discard_incoming();
			} else if (action == 2u) {
				check(gap_adapter->unbind() && gap_adapter->bind(), "COBS adapter detaches and rebinds");
			} else {
				gap_adapter.reset();
				gap_adapter = std::make_unique<Adapter>(gap_port, link);
				check(gap_adapter->bind(), "a replacement COBS adapter binds");
			}
			check(link.storage().rx_available() == 2u && link.stats().rx.resyncs == 1u,
			      "dropping or detaching COBS input immediately releases the partial frame and announces a gap");
			{
				auto queued = link.pop_packet();
				check(queued && std::vector<uint8_t>(queued.data().begin(), queued.data().end()) == payload,
				      "a completed COBS packet survives the discontinuity");
			}
			gap_port.feed(bytes.last(2u));
			check(!link.has_packet() && link.storage().rx_available() == 3u,
			      "the old tail cannot finish a packet across a discarded stream interval");
			gap_port.feed(bytes);
			check(link.has_packet(), "COBS resumes after the discarded tail's delimiter");
		}
	};
	check_cobs_discontinuity.template operator()<crc::NoCrc>();
	check_cobs_discontinuity.template operator()<crc::Crc16Bitwise>();
	{
		FakePort empty_port;
		Client empty_link;
		ClientAdapter empty_adapter{empty_port, empty_link};
		check(empty_adapter.bind(), "empty-delivery timing fixture binds");
		empty_port.feed(response_bytes.first(4u));
		for (unsigned i = 0; i < 10u; ++i) {
			pump(10);
			empty_adapter.deliver({});
		}
		check(!empty_link.assembling() && !empty_adapter.deadline_armed() &&
		      empty_link.framing_stats().stale_frames == 1u,
		      "empty serial deliveries cannot keep an abandoned RTU frame alive");
	}

	return modbus_test::finish();
}

#include "serial_adapter_test.moc"
