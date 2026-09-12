<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Qt: serial COBS, Modbus RTU and TCP

<!-- toc -->

Contents

- [Choose the Qt composition](#choose-the-qt-composition)
- [Build and run](#build-and-run)
- [Port setup and object lifetime](#port-setup-and-object-lifetime)
- [COBS over QSerialPort](#cobs-over-qserialport)
- [RTU client and server](#rtu-client-and-server)
  - [A queued client request](#a-queued-client-request)
  - [A small server, with the same pending-send discipline](#a-small-server-with-the-same-pending-send-discipline)
- [Serial adapter errors and teardown](#serial-adapter-errors-and-teardown)
- [TCP over QTcpSocket](#tcp-over-qtcpsocket)
- [Integrating into an existing Qt project](#integrating-into-an-existing-qt-project)
- [Verification scope](#verification-scope)

<!-- /toc -->

[Documentation](README.md) · [Почни звідси](START_HERE_UK.md) · [Examples](EXAMPLES.md) · [FreeRTOS](FREERTOS.md)

You need a Qt event loop and one owning thread for each port/endpoint/adapter
group. This page shows setup, types, RX, TX, backpressure and teardown. The
examples do not require reading library internals. Source links provide the
complete buildable programs, not missing parts of the API explanation.

## Choose the Qt composition

| Need | Composition | What remains application-owned |
|---|---|---|
| COBS over QSerialPort | `cobs::Endpoint` + `adapters::qt::SerialAdapter` | payload schema and pending-send policy |
| RTU request scheduler | framed Response endpoint + `adapters::qt::RtuClient` | function data, callback handling, timeout/retry choices |
| RTU server or custom transaction manager | framed Request/Response endpoint + `SerialAdapter` | register semantics or request scheduling |
| TCP | `modbus::tcp::Endpoint` + application QTcpSocket glue | connection, partial-write handling, transaction matching |

Do not bind another SerialAdapter when using RtuClient: it owns one already.
SerialAdapter supports COBS and **framed RTU**, not bare RTU and not TCP.
The TCP example below is explicitly application glue, not a new public
adapter or a custom replacement for MBAP framing.

## Build and run

Qt Core + SerialPort are needed for serial examples; Core + Network for TCP.
On this workstation Qt 6.4.3/MinGW 11.2 has all three modules. Use a matching
compiler from the selected Qt kit; the GUI application's Qt 6.10.1 kit is
not a substitute when SerialPort is absent.

```sh
# Git Bash on the recorded Windows setup; safe: no serial port opened.
sh doc/examples/qt/build.sh

# Another installed Qt kit, for example on Linux:
QMAKE=/path/to/qmake6 MAKE=make sh doc/examples/qt/build.sh
```

The runner creates `doc/examples/qt/out/{cobs,rtu,tcp}/bin/qt_{cobs,rtu,tcp}`
(with `.exe` on Windows), builds warning-free, then runs `--help` and
`--self-test` for each. Serial self-tests use a paired asynchronous test port
and split every write into two receive callbacks; TCP uses real localhost
sockets on an ephemeral port. This never contacts a remote server or COM port.

After building, deliberate real-port use is explicit:

```sh
doc/examples/qt/out/cobs/bin/qt_cobs.exe --port COM6 115200
doc/examples/qt/out/cobs/bin/qt_cobs.exe --server COM7 115200
doc/examples/qt/out/rtu/bin/qt_rtu.exe --port COM6 115200
doc/examples/qt/out/rtu/bin/qt_rtu.exe --server COM7 115200
doc/examples/qt/out/tcp/bin/qt_tcp.exe --self-test
```

COM6/COM7 are placeholders: choose your actual connected peers; do not open
the same physical serial port in two programs. `--port` sends one demo request
and waits at most two seconds. `--server` runs until the process is stopped.
COBS's peer must echo its five-byte payload. RTU's peer must expose unit 1,
FC03 registers 0..1 with values 100 and 200 for this specific demo to pass.

## Port setup and object lifetime

Port setup is ordinary Qt. This fragment belongs before constructing/binding
the chosen endpoint adapter; check failures rather than proceeding unopened:

```cpp
#include <QCoreApplication>
#include <QSerialPort>

QSerialPort port;
port.setPortName("COM6"); // or /dev/ttyUSB0
if (!port.setBaudRate(115200) ||
    !port.setDataBits(QSerialPort::Data8) ||
    !port.setParity(QSerialPort::NoParity) ||
    !port.setStopBits(QSerialPort::OneStop) ||
    !port.setFlowControl(QSerialPort::NoFlowControl) ||
    !port.open(QIODevice::ReadWrite)) {
    // Report port.errorString() and stop this attempt.
}
```

These are demo 8N1 settings, not an instruction to change an existing Modbus
device's agreed serial settings. A standard serial-line deployment may require
different parity/stop bits; match the peer. The adapters do not configure the
UART format for you. Qt exposes port configuration and asynchronous signals
through [QSerialPort](https://doc.qt.io/qt-6/qserialport.html).

Use member declaration order `port`, `endpoint`, `adapter/client`, `pending`
so destruction unwinds owners safely. They must remain in the same thread
while bound. Construct/bind them in a worker thread if that is where its event
loop runs; do not move only the port and leave the endpoint/Qt timers behind.
For GUI updates emit copied values or an owning QByteArray, not a Packet/span
whose non-atomic ownership is still managed by the communication thread.

## COBS over QSerialPort

Includes: `cobs/Cobs.h`, `adapters/qt/SerialAdapter.h`, Qt Core/SerialPort.
No UART driver, STM32 HAL, FreeRTOS wake or manual clock is involved.

The example's types (both serial protocols shown once):

```cpp
namespace framing = modbus::rtu::framing;
```

<!-- example: examples/qt/serial.cpp#qt-types -->
```cpp
using ClientLink = modbus::rtu::Endpoint<wire::Pool<4, 2>, modbus::rtu::Format<>,
    framing::Standard<framing::Direction::Response>>;
using ServerLink = modbus::rtu::Endpoint<wire::Pool<4, 2>, modbus::rtu::Format<>,
    framing::Standard<framing::Direction::Request>>;
using CobsLink = cobs::Endpoint<wire::Pool<4, 2>>;
```
<!-- /example -->

In an ordinary application `Port` below is `QSerialPort`. The complete
[serial.cpp](examples/qt/serial.cpp) wraps this in `exchange(port)` with a
QEventLoop, a two-second outer timeout and checked port setup:

<!-- example: examples/qt/serial.cpp#qt-cobs-client -->
```cpp
CobsLink link;
adapters::qt::SerialAdapter<CobsLink, Port> adapter{port, link};
if (!adapter.bind()) { return 1; }
auto pending = link.make_message();
const std::array<uint8_t, 5> payload{'h', 'e', 0, 'l', 'o'};
if (!pending || !pending.append_bytes(payload)) { return 1; }
const auto service = [&] {
    if (pending) {
        const auto result = link.send(pending);
        if (result != wire::SendResult::Sent && result != wire::SendResult::Busy) {
            pending = {};
            loop.exit(1);
        }
    }
    while (auto packet = link.pop_packet()) {
        const bool ok = std::ranges::equal(packet.data(), payload);
        qInfo() << "COBS echo:" << packet.size() << "payload bytes";
        loop.exit(ok ? 0 : 1);
    }
};
adapter.set_service_handler(service); // invoked by readyRead/bytesWritten/errors
QTimer::singleShot(0, &loop, service); // start after the event loop begins
```
<!-- /example -->

`pending` survives a Busy response. The installed service callback runs on
receive, TX completion, transport error and applicable stale expiry. Install
it **after bind**, since binding teardown clears the previous callback.
It must not block the event loop. The demo checks the echoed bytes and exits
the local event loop; a continuous application can instead dispatch packets
and retain another bounded pending Message.

COBS does not start an incomplete-frame timer. It keeps a partial frame until
completion, a gap or explicit discard/detach. A separate application request
timeout is your decision, not hidden COBS protocol state.

## RTU client and server

### A queued client request

Includes: `modbus/rtu/Rtu.h`, `adapters/qt/RtuClient.h`, Qt Core/SerialPort.
Use the Response-framed ClientLink above. `RtuClient` owns both the serial
adapter and the transaction queue; the endpoint continues to own wire buffers.

<!-- example: examples/qt/serial.cpp#qt-rtu-client -->
```cpp
ClientLink link;
adapters::qt::RtuClient<ClientLink, Port> client{port, link};
if (!client.bind()) { return 1; }
client.set_response_timeout_ms(500);
client.set_retries(0); // explicit for the demo; production policy is yours
client.update_timing_from_port();
const std::array<uint8_t, 4> request{0, 0, 0, 2}; // first register, count; BE16
const bool queued = client.send(1u, 3u, request, [&](const auto& response) {
    using State = adapters::qt::RequestState;
    std::size_t offset = 0;
    uint8_t bytes = 0;
    uint16_t first = 0, second = 0;
    const bool ok = response.state == State::Completed && !response.exception &&
        wire::read_be(response.data, offset, bytes) && bytes == 4u &&
        wire::read_be(response.data, offset, first) &&
        wire::read_be(response.data, offset, second) && offset == response.data.size();
    if (ok) { qInfo() << "registers:" << first << second; }
    // response.data belongs only to this callback; retain values, not its span.
    loop.exit(ok && first == 100u && second == 200u ? 0 : 1);
});
if (!queued) { return 1; }
```
<!-- /example -->

The callback reads a standard FC03 byte count and two BE16 registers. It
checks completion/exception before reading data and verifies consumption of
the entire function-data span. For an exception, inspect `exception_code`;
for a timeout/write error/cancellation there may be no data to parse.

| Client API | Meaning |
|---|---|
| `bind()`, `bound()`, `unbind()` | start/inspect/stop binding; unbind can refuse an active TX borrow |
| `send(address, function, data, handler)` | copy/enqueue request; false means not queued; handler is not called before send returns |
| `set_response_timeout_ms(ms)` | response budget; default 1000 ms, minimum 1 |
| `set_retries(n)` | retry count after the first accepted attempt; default 3, minimum 0 |
| `set_turnaround_delay_ms(ms)` | broadcast turnaround budget; default 100 ms |
| `set_inter_frame_delay_ms(ms)` | requested spacing, bounded by the transport timing floor |
| `update_timing_from_port()` | refresh timing after configuring/changing the port baud |
| `pending()`, `idle()` | queued/outstanding request state, not UART chunk count |
| `service()` | manual service hook; ordinary event-loop use is automatic |
| `adapter()` | access transport error/abort/discard operations when explicitly needed |

Response fields: `state`, `address`, `function`, `exception`, `exception_code`,
`data`, `attempts`. States are Completed, Broadcast, Timeout, WriteError and
Cancelled. `attempts` counts accepted sends, not Busy polls. `data` is valid
only while the handler runs. Copy it if needed afterward:

```cpp
QByteArray saved(reinterpret_cast<const char*>(response.data.data()),
                 static_cast<qsizetype>(response.data.size()));
```

The client serializes RTU requests because the wire has no transaction ID.
Late responses with the same address/function can remain indistinguishable
from a newer transaction; retries of writes can repeat an action. The library
does not invent at-most-once execution. Busy is not a consumed attempt;
write failure is handled differently from waiting for a response. See
[client recovery contract](QT_CLIENT_RECOVERY.md) for exact edge cases.

### A small server, with the same pending-send discipline

The following checked helper is used for both COBS echo and RTU register
demo. Define `EXAMPLE_RTU=0` for COBS or `1` for RTU at compile time. Instantiate
`DemoServer<CobsLink, QSerialPort>` or `DemoServer<ServerLink, QSerialPort>`
with your open port, check `server.start()`, then run the application's Qt
event loop. No extra RtuClient is involved on the server side.

<!-- example: examples/qt/serial.cpp#qt-server -->
```cpp
template<class Link, class Port>
class DemoServer {
    Link link_;
    adapters::qt::SerialAdapter<Link, Port> adapter_;
    typename Link::Message pending_;
public:
    explicit DemoServer(Port& port) : adapter_(port, link_) {}
    bool start() {
        if (!adapter_.bind()) { return false; }
        adapter_.set_service_handler([this] { service(); }); // AFTER bind
        return true;
    }
    void service() {
        if (!pending_) {
            if (auto packet = link_.pop_packet()) {
                if constexpr (EXAMPLE_RTU != 0) {
                    std::size_t offset = 0;
                    uint16_t start = 0, count = 0;
                    if (packet.address() != 1u) { return; } // no broadcast response
                    const bool valid = packet.function() == 3u &&
                        wire::read_be(packet.data(), offset, start) &&
                        wire::read_be(packet.data(), offset, count) &&
                        offset == packet.size() && start == 0u && count == 2u;
                    const uint8_t function = valid ? uint8_t{3}
                        : static_cast<uint8_t>(packet.function() | 0x80u);
                    pending_ = link_.make_message(1u, function);
                    if (!pending_) { return; } // explicit demo drop on allocation failure
                    const bool built = valid
                        ? pending_.append_be(uint8_t{4}) && pending_.append_be(uint16_t{100}) && pending_.append_be(uint16_t{200})
                        : pending_.append_be(packet.function() == 3u ? uint8_t{2} : uint8_t{1});
                    if (!built) { pending_ = {}; }
                } else {
                    pending_ = link_.make_message(packet.size());
                    if (!pending_ || !pending_.append_bytes(packet.data())) { pending_ = {}; }
                }
            }
        }
        if (pending_) {
            const auto result = link_.send(pending_);
            // A serial start failure can leave bytes on the wire: this demo
            // drops Failed explicitly. It never retries a partial transmission.
            if (result != wire::SendResult::Sent && result != wire::SendResult::Busy) {
                pending_ = {};
            }
        }
    }
};
```
<!-- /example -->

The RTU demo serves unit 1, FC03, start 0/count 2. Other FC03 ranges receive
exception 2; other functions the RX framer recognizes receive exception 1
with their own function number's high bit set. Unknown RX layouts cannot be
assembled and do not reach this handler. This is not a full Modbus application
server. Unit 0 does not receive a response. A production server must implement
its own register map, validation, broadcast semantics and scheduling policy.

## Serial adapter errors and teardown

| Adapter operation | Use |
|---|---|
| `deliver(bytes)` | explicitly deliver a byte span, useful for a custom Qt read path/test; normal readyRead wiring does this |
| `set_service_handler(...)` | packet/TX/error service notification in the owning Qt thread |
| `last_transport_error()` | inspect None/Read/Write/Resource |
| `take_transport_error()` | inspect and clear the remembered error |
| `discard_incoming()` | clear input and partial parser state; already queued Packets survive |
| `abort_outgoing()` | cancel buffered serial output and release the endpoint borrow |
| `now_ms()` | adapter clock for an advanced caller; ordinary use needs none |
| `deadline_armed()` | RTU stale timer status; false for COBS |

`SerialAdapter` uses a 50-ms desktop stale-silence rule for framed RTU, not
strict physical t1.5/t3.5 measurement. Empty deliveries do not extend it.
COBS input discard becomes a gap/delimiter recovery, not an RTU timer.

A serial write accepting fewer than the full frame is a transport failure.
Clearing buffered output cannot retract bytes already transmitted; do not
claim a retry is safe simply because the Message survived as Failed. Establish
the application's recovery boundary before replaying a command.

For intentional shutdown, stop new requests, close/cancel the port, call
`adapter.abort_outgoing()` to complete the library borrow, then unbind and
release any remaining Packet/Message. For RtuClient use `client.adapter()`
and `client.unbind()`; unbind reports cancellation to queued handlers, while
destruction deliberately does not invoke user handlers. The complete program
executes this shutdown path before its endpoint/port owners disappear.

## TCP over QTcpSocket

The endpoint already frames by MBAP; do not add an RTU table or another
application length prefix. Include `modbus/tcp/Tcp.h`, use Qt Core/Network,
and create one endpoint per independent connection. Default TCP has no CRC.

The checked [tcp.cpp](examples/qt/tcp.cpp) contains the complete application
glue below. It accepts a whole outbound ADU into a bounded staging array,
then continues any short socket writes internally. It has one outstanding
frame and a one-second TX deadline. Its 1-ms timer is **example transport
pacing**, not a timer required by the TCP protocol core.

```cpp
using Link = modbus::tcp::Endpoint<wire::Pool<4, 2>>;
```

<!-- example: examples/qt/tcp.cpp#qt-tcp-connection -->
```cpp
class Connection final : public QObject {
    QTcpSocket& socket_;
    QTimer service_;
    QElapsedTimer clock_, tx_clock_;
    std::array<uint8_t, Link::max_frame_size> output_{};
    std::size_t count_ = 0, offset_ = 0;
    bool failed_ = false;
public:
    Link endpoint;
    explicit Connection(QTcpSocket& socket) : socket_(socket) {
        clock_.start();
        QObject::connect(&socket_, &QTcpSocket::readyRead, this, [this] {
            const auto bytes = socket_.readAll();
            endpoint.consume({reinterpret_cast<const uint8_t*>(bytes.constData()),
                              static_cast<std::size_t>(bytes.size())});
            if (endpoint.rx_failed()) { fail(); }
        });
        QObject::connect(&socket_, &QTcpSocket::disconnected, this, [this] { fail(); });
        QObject::connect(&socket_, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) { fail(); });
        QObject::connect(&service_, &QTimer::timeout, this, [this] { service(); });
        service_.start(1); // bounded demo pacing; includes a one-second TX watchdog
        if (!endpoint.bind(Link::Sender{[this](auto frame) { return send(frame); }},
                           Link::BusyQuery{[this] { return count_ != 0; }})) { fail(); }
    }
    ~Connection() override {
        service_.stop();
        QObject::disconnect(&socket_, nullptr, this, nullptr);
        socket_.abort();
        count_ = 0;
        endpoint.poll(now());
        (void)endpoint.unbind();
        // endpoint is destroyed before its socket reference; no pending borrow.
    }
    bool failed() const { return failed_; }
private:
    uint32_t now() const { return static_cast<uint32_t>(clock_.elapsed()); }
    bool send(std::span<const uint8_t> frame) {
        if (failed_ || count_ != 0 || frame.size() > output_.size() ||
            socket_.state() != QAbstractSocket::ConnectedState) { return false; }
        std::copy(frame.begin(), frame.end(), output_.begin());
        count_ = frame.size();
        offset_ = 0;
        tx_clock_.start();
        return true; // accepts ALL bytes locally; no socket call/re-entry here
    }
    void service() {
        if (failed_) { return; }
        if (count_ != 0 && tx_clock_.elapsed() >= 1000) { fail(); return; }
        if (offset_ < count_) {
            const auto written = socket_.write(
                reinterpret_cast<const char*>(output_.data() + offset_),
                static_cast<qint64>(count_ - offset_));
            if (failed_) { return; } // errorOccurred may run inside write()
            if (written < 0) { fail(); return; }
            offset_ += static_cast<std::size_t>(written); // zero: retry next tick
        }
        if (offset_ == count_ && socket_.bytesToWrite() == 0) {
            count_ = offset_ = 0;
        }
        endpoint.poll(now());
    }
    void fail() {
        if (failed_) { return; }
        failed_ = true;
        service_.stop();
        endpoint.notify_gap();
        socket_.abort();
        count_ = offset_ = 0;
        endpoint.poll(now());
        // Never reset and resume the corrupt connection. Make a fresh Connection
        // for a newly established stream; handle old queued packets explicitly.
    }
};
```
<!-- /example -->

Use it with an already connected QTcpSocket that outlives Connection. The
application creates/sends messages in the same thread:

<!-- example: examples/qt/tcp.cpp#qt-tcp-message -->
```cpp
auto request = client.endpoint.make_message(7u, 1u, 0x41u);
if (!request || !request.append_be(uint16_t{1234}) ||
    client.endpoint.send(request) != wire::SendResult::Sent) { return 1; }
// MBAP handles the custom function; no additional payload-length field.
```
<!-- /example -->

The localhost program constructs a listener and client, echoes function
0x41 while preserving transaction/unit/function metadata, then injects a
nonzero Protocol ID and checks fail-closed disconnect. It does not claim a
complete network server, TLS, automatic reconnect or industrial device interop.
QAbstractSocket's buffering and completion/error APIs are described in the
[official socket reference](https://doc.qt.io/qt-6/qabstractsocket.html).

After disconnect or invalid MBAP, create a fresh Connection for a newly
established stream. If reusing an endpoint manually, discard old-session
queued Packets deliberately and call `reset_rx()` only at that known new
boundary. A plausible corrupted Length is not safely repaired by scanning
random bytes for something that resembles MBAP.

## Integrating into an existing Qt project

qmake, substituting the repository path:

```qmake
CONFIG += c++20
QT += core serialport
include(path/to/repository/src/cobs/cobs.pri)
include(path/to/repository/src/modbus/rtu/rtu.pri)
include(path/to/repository/src/adapters/qt/qt.pri)
# TCP instead/additionally:
QT += network
include(path/to/repository/src/modbus/tcp/tcp.pri)
```

Include only the protocols you use. For CMake see [Build](BUILD.md): add
the `src` and delegate include roots, two COBS `.cpp` files if using COBS,
and link Qt6::Core plus Qt6::SerialPort or Qt6::Network. Endpoint/storage
remain independent of Qt; no QObject belongs in your custom storage contract.

## Verification scope

`sh doc/examples/qt/build.sh` checks the three cookbook programs. The larger
`sh src/adapters/qt/tests/run.sh` checks adapter ordering, retries, cancellation,
partial writes and failure paths with real Qt and fake serial I/O. Live
QModbus/H7S comparisons are separately indexed in [Testing](TESTING.md).
Neither host serial fake nor localhost socket results replace a physical
serial/Ethernet deployment test.
