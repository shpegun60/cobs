/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * adapters::qt::SerialAdapter — QSerialPort bound to a protocol endpoint on the
 * desktop, for cobs::Endpoint and for a modbus::rtu::Endpoint with a framing
 * policy. The Qt counterpart of adapters/rtu/UartAdapter.h: it knows both a
 * transport and an endpoint, and neither of them knows it.
 *
 *     QSerialPort port;
 *     port.setPortName("COM6"); port.setBaudRate(115200);
 *     port.open(QIODevice::ReadWrite);
 *
 *     using Client = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<>,
 *                        modbus::rtu::framing::Standard<modbus::rtu::framing::Direction::Response>>;
 *     Client client;
 *     adapters::qt::SerialAdapter adapter{port, client};   // port and client outlive it
 *     adapter.bind();
 *
 *     auto request = client.make_message(0x11, 0x03);
 *     request.append_be<uint16_t>(0x006B); request.append_be<uint16_t>(3);
 *     client.send(request);                                 // QSerialPort::write copies the ADU
 *     // ... readyRead -> consume(); a complete response is client.pop_packet()
 *
 * Event-driven, on the thread that owns the port's event loop: readyRead
 * delivers whatever the OS handed over to consume() (a fragment, several
 * frames, or both), bytesWritten lets poll() return the transmitted block,
 * and the endpoint's Sender is QSerialPort::write. BusyQuery is
 * bytesToWrite() != 0: Qt copied the frame, so the endpoint's block could be
 * released at once, but "the line is still carrying the previous frame" is
 * the fact an RTU client needs before it starts the next one.
 *
 * Off the STM32 driver there are no IDLE-ended bursts: a QSerialPort delivers
 * arbitrary cuts, so an RTU endpoint over it needs the framing policy
 * (static_assert), and cobs::Endpoint needs nothing.
 *
 * Frames that stop arriving (RTU with a framing policy). The endpoint exposes
 * assembling() and expire_incomplete(); when to call the latter is this
 * layer's knowledge, and on the desktop that knowledge is not the wire but
 * the OS: bytes reach the process in bursts whose spacing is the serial
 * stack's, not the peer's (a USB-serial bridge hands over up to one latency
 * timer of data at a time, 16 ms by default on FTDI parts; Windows adds its
 * scheduling). stale_silence_ms is therefore a transport allowance above
 * typical delivery granularity, not a hard bound on OS scheduling: a
 * single-shot timer restarted on every delivery while a
 * frame is in flight, expiring the frame after 50 ms without bytes. A
 * request/response client rarely reaches it — its request timeout discards
 * the incomplete response together with the request — but a server, or a
 * client whose peer died mid-frame, must not glue the next frame onto an
 * orphan.
 *
 * QtSerialBus solves the same problem twice, and neither way is this one.
 * Its RTU server (QModbusRtuSerialServerPrivate) keeps no timer: on each
 * readyRead it drops the buffered fragment if the PREVIOUS delivery was more
 * than an inter-frame delay ago — 3.5 character times below 19200 baud, a
 * flat 2 ms above it (QModbusDevicePrivate::calculateInterFrameDelay, eleven
 * bits per character). That threshold is the wire's, while the spacing it
 * measures is the operating system's, so a frame that arrives in two
 * deliveries a few milliseconds apart — routine for a USB bridge on Windows
 * — is discarded even though it is intact; Qt's own comment concedes the
 * risk for "very slow baud rates". A stale fragment also survives until the
 * next byte arrives, since nothing expires it on its own. Its RTU client
 * (QModbusRtuSerialClientPrivate) instead starts every request clean:
 * processQueue() clears both its response buffer and the port's, which is
 * what discard_incoming() below offers. This adapter keeps the timer, which
 * tolerates delivery gaps below its deadline, and offers the clean start
 * explicitly rather than tying it to a request queue it does not own.
 *
 * Errors. QSerialPort reports no per-byte framing or parity errors on Qt 6.
 * A ReadError means bytes may have been lost on the way in: the frame in
 * flight is dropped through notify_gap(), counted as a stream gap, because a
 * lost byte inside a length-prefixed frame would otherwise consume the frame
 * behind it (QModbus reports the error to the application and leaves its
 * buffer standing; the discontinuity is this adapter's decision). A
 * WriteError means the frame being sent will not leave: what the port still
 * buffers is cleared and the endpoint releases the block it was lending, so
 * nothing waits for a bytesWritten that never comes. The release does not
 * depend on the clearing succeeding — a port whose device is gone refuses
 * clear() — because the port holds its own copy of the frame and the
 * endpoint's block is referenced by nobody else; a line whose state is
 * unknown after an error is not "busy" in any sense worth waiting for. A
 * ResourceError (the device went away) is both. The layer above learns which happened from
 * take_transport_error(), read inside its service handler, and decides what
 * it means for its transaction (adapters/qt/RtuClient.h finishes the request
 * as a write error). Everything else the port reports is its own business.
 *
 * Partial writes. QIODevice::write() may accept fewer bytes than offered, and
 * from Qt 6.10 QSerialPort has a configurable write-buffer limit that makes
 * it do so. Half a frame on the line followed by the endpoint's retry of the
 * whole frame would be a hybrid nobody can parse, so a short write is
 * treated as a failed transmission: the port's output is cleared at once,
 * the endpoint is told the transport refused (it keeps the message for a
 * deliberate retry), and TransportError::Write is recorded.
 *
 * Lifetime: the port and the endpoint outlive the adapter while it is bound.
 * bind() is transactional (the endpoint's transport binding first, the
 * signal connections only when it succeeded); unbind() is refused while the
 * transmitted frame has not left the port, and detaches otherwise. The
 * endpoint's Sender and BusyQuery point at THIS object — unlike the STM32
 * adapter's, which point at the driver — so the destructor of a bound
 * adapter unbinds the endpoint before it goes: the block a frame still on
 * its way out was lending is released (Qt holds its own copy of the bytes,
 * which keep leaving), and a later send() on the endpoint is refused as
 * Unbound instead of calling into a destroyed adapter. All connections use
 * the adapter's own timer as their context object. One adapter per port at
 * a time, and that is the application's duty: a Qt connection is added
 * rather than substituted, so binding a second adapter to a port whose first
 * adapter is still bound leaves both connected and both draining the same
 * readyRead, and the first one's destructor would then unbind the endpoint
 * from under the second. Unbind the first.
 */

#ifndef ADAPTERS_QT_SERIAL_ADAPTER_H_
#define ADAPTERS_QT_SERIAL_ADAPTER_H_

#include "tiny_delegate.hpp"

#include <QByteArray>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>
#include <QSerialPort>
#include <QTimer>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace adapters::qt {

// What the adapter needs from an endpoint: the shape both protocols share.
template<class E>
concept StreamEndpoint = requires(E& endpoint, std::span<const uint8_t> bytes, uint32_t now_ms) {
	endpoint.consume(bytes);
	endpoint.notify_gap();
	endpoint.poll(now_ms);
	{ endpoint.tx_active() } -> std::convertible_to<bool>;
	{ endpoint.unbind() } -> std::convertible_to<bool>;
};

// An RTU endpoint names whether it carries a framing policy; COBS has no such
// notion and needs none.
template<class E>
concept RtuEndpoint = requires {
	{ E::framed } -> std::convertible_to<bool>;
};

// The stale-frame vocabulary of an RTU endpoint with a framing policy.
template<class E>
concept FramedEndpoint = RtuEndpoint<E> && requires(E& endpoint) {
	{ endpoint.assembling() } -> std::convertible_to<bool>;
	endpoint.expire_incomplete();
	endpoint.discard_incomplete();
};

// What the adapter needs from the port: QSerialPort, or a QIODevice stand-in
// with the same three signals (the host test uses one).
template<class P>
concept SerialLike = requires(P& port, const char* data, qint64 size) {
	{ port.write(data, size) } -> std::convertible_to<qint64>;
	{ port.readAll() } -> std::convertible_to<QByteArray>;
	{ port.bytesToWrite() } -> std::convertible_to<qint64>;
	{ port.clear(QSerialPort::Input) } -> std::convertible_to<bool>;
	&P::readyRead;
	&P::bytesWritten;
	&P::errorOccurred;
};

// What went wrong with the port, as far as the layer above needs to know.
enum class TransportError : uint8_t {
	None = 0u,
	Read,       // bytes may have been lost on the way in; the frame in flight was dropped
	Write,      // the frame being sent did not (fully) leave; output cleared, its block released
	Resource,   // the device went away: both of the above
};

template<class EndpointT, class PortT = QSerialPort>
class SerialAdapter final {
	static_assert(StreamEndpoint<EndpointT>,
		"SerialAdapter serves a cobs::Endpoint or a modbus::rtu::Endpoint");
	// FramedEndpoint, not EndpointT::framed: the name must not be looked up in
	// an endpoint that has none (cobs::Endpoint), which a static_assert would.
	static_assert(!RtuEndpoint<EndpointT> || FramedEndpoint<EndpointT>,
		"an RTU endpoint over QSerialPort needs a framing policy: the port delivers arbitrary cuts, not bursts");
	static_assert(SerialLike<PortT>, "SerialAdapter drives a QSerialPort (or a stand-in with its signals)");

public:
	using Endpoint = EndpointT;
	using Port = PortT;
	// "the endpoint may have something for you": raised after a delivery, a
	// finished transmission, a transport error and a stale-frame expiry. A
	// layer above (adapters/qt/RtuClient.h) drains packets, reads
	// take_transport_error() and advances its transaction in it; an
	// application that polls the endpoint itself leaves it unset. It is
	// installed for one binding: unbind() and the destructor drop it, so a
	// rebind never raises it into an owner that may be gone by then.
	using ServiceHandler = tiny::delegate<void()>;

	// Whether the stale-frame rule is compiled in (RTU with a framing policy).
	static constexpr bool framed = FramedEndpoint<EndpointT>;
	// Silence after the last delivered byte that ends a frame in flight:
	// a USB/OS delivery allowance, independent of the request timeout.
	static constexpr int stale_silence_ms = 50;

	SerialAdapter(PortT& port, EndpointT& endpoint)
		: m_port(port), m_endpoint(endpoint)
	{
		m_clock.start();   // monotonic for the adapter's whole life, across bind/unbind cycles
		m_stale.setSingleShot(true);
		m_stale.setTimerType(Qt::PreciseTimer);
		QObject::connect(&m_stale, &QTimer::timeout, &m_stale, [this]() { on_stale(); });
	}

	SerialAdapter(const SerialAdapter&) = delete;
	SerialAdapter& operator=(const SerialAdapter&) = delete;

	// A bound adapter leaves the endpoint unbound: its Sender and BusyQuery
	// point at this object. A frame still leaving the port keeps leaving (Qt
	// owns the bytes); the block it was lending is released now, since a
	// destroyed adapter can no longer answer whether the line is busy.
	~SerialAdapter()
	{
		if (m_bound) {
			m_releasing = true;
			release_borrowed_block();
			(void)m_endpoint.unbind();
			detach();
		}
	}

	/*
	 * Wires the port to the endpoint. Transactional: false — the endpoint
	 * refuses the transport binding because a transmission is still active —
	 * leaves both untouched. The port need not be open yet: bytes flow from
	 * the moment it is.
	 */
	[[nodiscard]] bool bind()
	{
		if (m_bound) {
			return true;
		}
		if (!m_endpoint.bind(
				typename EndpointT::Sender{tiny::bind<&SerialAdapter::send>(*this)},
				typename EndpointT::BusyQuery{tiny::bind<&SerialAdapter::busy>(*this)})) {
			return false;
		}
		m_rx = QObject::connect(&m_port, &PortT::readyRead, &m_stale,
			[this]() { on_ready_read(); });
		m_tx = QObject::connect(&m_port, &PortT::bytesWritten, &m_stale,
			[this](qint64) { on_bytes_written(); });
		m_error = QObject::connect(&m_port, &PortT::errorOccurred, &m_stale,
			[this](QSerialPort::SerialPortError error) { on_error(error); });
		m_transport_error = TransportError::None;
		m_bound = true;
		return true;
	}

	// The reverse: false while the transmitted frame has not left the port
	// (the endpoint still holds its block), and then nothing has changed.
	[[nodiscard]] bool unbind()
	{
		if (!m_bound) {
			return true;
		}
		m_endpoint.poll(now_ms());
		if (!m_endpoint.unbind()) {
			return false;
		}
		detach();
		return true;
	}

	[[nodiscard]] bool bound() const noexcept { return m_bound; }

	void set_service_handler(ServiceHandler handler) { m_service = static_cast<ServiceHandler&&>(handler); }

	// Milliseconds since the adapter was created: the monotonic tick the
	// endpoint's poll() takes, and the clock the layer above dates its
	// deadlines in. It does not restart on a rebind.
	[[nodiscard]] uint32_t now_ms() const noexcept
	{
		return static_cast<uint32_t>(m_clock.elapsed());
	}

	// Whether a frame is in flight and its silence timer running (framed RTU).
	[[nodiscard]] bool deadline_armed() const noexcept { return m_stale.isActive(); }

	// The last transport error since it was last taken: what a failed
	// transmission or a dropped frame in flight was caused by.
	[[nodiscard]] TransportError last_transport_error() const noexcept { return m_transport_error; }

	[[nodiscard]] TransportError take_transport_error() noexcept
	{
		const TransportError error = m_transport_error;
		m_transport_error = TransportError::None;
		return error;
	}

	// Transport entry points. bind() routes the port's signals here; a test or
	// another byte source may call deliver() directly.
	void on_ready_read()
	{
		const QByteArray bytes = m_port.readAll();
		deliver(std::span<const uint8_t>{
			reinterpret_cast<const uint8_t*>(bytes.constData()),
			static_cast<std::size_t>(bytes.size())});
	}

	void deliver(const std::span<const uint8_t> bytes)
	{
		m_endpoint.consume(bytes);
		if constexpr (framed) {
			if (m_endpoint.assembling()) {
				m_stale.start(stale_silence_ms);   // restarted on every delivery
			} else {
				m_stale.stop();
			}
		}
		m_endpoint.poll(now_ms());
		notify();
	}

	void on_bytes_written()
	{
		m_endpoint.poll(now_ms());
		notify();
	}

	/*
	 * Starts clean: the bytes the OS has buffered for us are dropped and the
	 * frame in flight is discarded (uncounted — it is a discontinuity, not a
	 * fault). This drops buffered input, not bytes that may arrive later, and
	 * does not drain already published endpoint packets. A transaction layer
	 * owns those and decides when to discard them. It is deliberately not
	 * automatic: this layer owns no request queue.
	 */
	void discard_incoming()
	{
		if (!m_port.clear(QSerialPort::Input)) {
			(void)m_port.readAll();   // a port that cannot clear can still be read empty
		}
		m_stale.stop();
		if constexpr (framed) {
			m_endpoint.discard_incomplete();
		}
	}

	void on_error(const QSerialPort::SerialPortError error)
	{
		if (m_releasing) {
			return;   // clear() can itself emit an error: finish the outer recovery first
		}
		switch (error) {
		case QSerialPort::ReadError:
			drop_incoming(TransportError::Read);
			break;
		case QSerialPort::WriteError:
			abort_outgoing(TransportError::Write);
			break;
		case QSerialPort::ResourceError:
		case QSerialPort::UnknownError:
			abort_outgoing(TransportError::Resource);
			drop_incoming(TransportError::Resource);
			break;
		default:
			return;   // the port's own business
		}
		notify();
	}

	// A transaction's write deadline expired without a port error signal.
	// Qt owns its copy, so releasing the endpoint's block is safe even when
	// clear() fails. Already transmitted bytes cannot be retracted.
	void abort_outgoing()
	{
		if (!m_bound) {
			return;
		}
		abort_outgoing(TransportError::Write);
		notify();
	}

private:
	void on_stale()
	{
		if constexpr (framed) {
			if (m_endpoint.assembling()) {
				m_endpoint.expire_incomplete();
			}
		}
		notify();
	}

	void notify()
	{
		if (m_service) {
			m_service();
		}
	}

	// Bytes may have been lost on the way in: the frame in flight cannot be
	// completed, and the next byte starts a new one.
	void drop_incoming(const TransportError error)
	{
		m_stale.stop();
		m_endpoint.notify_gap();
		m_transport_error = error;
	}

	// The frame being sent will not leave: what the port still buffers is
	// dropped — best effort, a dead device refuses — and the endpoint takes
	// back the block it was lending whether or not the port could clear,
	// since nothing but the endpoint references that block.
	void abort_outgoing(const TransportError error)
	{
		const bool releasing = m_releasing;
		m_releasing = true;
		(void)m_port.clear(QSerialPort::Output);
		release_borrowed_block();
		m_releasing = releasing;
		m_transport_error = error;
	}

	// poll() with busy() forced false: the endpoint releases the block of the
	// frame on its way out, whatever the port says about its output.
	void release_borrowed_block()
	{
		const bool releasing = m_releasing;
		m_releasing = true;
		m_endpoint.poll(now_ms());
		m_releasing = releasing;
	}

	[[nodiscard]] bool send(const std::span<const uint8_t> frame)
	{
		const qint64 size = static_cast<qint64>(frame.size());
		const qint64 written = m_port.write(reinterpret_cast<const char*>(frame.data()), size);
		if (written == size) {
			return true;
		}
		// A short write would put part of a frame on the line and let a retry
		// follow it with the whole one: it is a failed transmission instead.
		(void)m_port.clear(QSerialPort::Output);
		m_transport_error = TransportError::Write;
		return false;
	}

	[[nodiscard]] bool busy() const
	{
		return !m_releasing && m_port.bytesToWrite() != 0;
	}

	void detach()
	{
		QObject::disconnect(m_rx);
		QObject::disconnect(m_tx);
		QObject::disconnect(m_error);
		m_stale.stop();
		if constexpr (framed) {
			m_endpoint.discard_incomplete();   // a discontinuity, not a fault
		}
		m_service = ServiceHandler{};   // the handler belongs to the binding, not to the adapter
		m_bound = false;
	}

	PortT& m_port;
	EndpointT& m_endpoint;
	QTimer m_stale;
	QElapsedTimer m_clock;
	QMetaObject::Connection m_rx;
	QMetaObject::Connection m_tx;
	QMetaObject::Connection m_error;
	ServiceHandler m_service;
	TransportError m_transport_error = TransportError::None;
	bool m_bound = false;
	bool m_releasing = false;
};

} // namespace adapters::qt

#endif /* ADAPTERS_QT_SERIAL_ADAPTER_H_ */
