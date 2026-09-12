/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "adapters/qt/SerialAdapter.h"
#include "adapters/qt/RtuClient.h"
#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QSerialPort>
#include <QTimer>
#include <cstring>
#include "LoopPort.h"

#ifndef EXAMPLE_RTU
#define EXAMPLE_RTU 0
#endif

namespace framing = modbus::rtu::framing;
// example-begin: qt-types
using ClientLink = modbus::rtu::Endpoint<wire::Pool<4, 2>, modbus::rtu::Format<>,
    framing::Standard<framing::Direction::Response>>;
using ServerLink = modbus::rtu::Endpoint<wire::Pool<4, 2>, modbus::rtu::Format<>,
    framing::Standard<framing::Direction::Request>>;
using CobsLink = cobs::Endpoint<wire::Pool<4, 2>>;
// example-end: qt-types

// Own the pending Message until send succeeds. The endpoint and port outlive
// this helper; the service delegate captures a borrowed this pointer.
// example-begin: qt-server
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
// example-end: qt-server

template<class Port>
int exchange(Port& port)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { loop.exit(2); });
    timeout.start(2000);
#if EXAMPLE_RTU
    // example-begin: qt-rtu-client
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
    // example-end: qt-rtu-client
    const int result = loop.exec();
    port.close(); // Qt owns copied output; stop this session before teardown
    client.adapter().abort_outgoing();
    (void)client.unbind();
    return result;
#else
    // example-begin: qt-cobs-client
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
    // example-end: qt-cobs-client
    const int result = loop.exec();
    port.close();
    adapter.abort_outgoing();
    (void)adapter.unbind();
    return result;
#endif
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 2 && args[1] == "--self-test") {
        LoopPort client_port, server_port;
        client_port.peer = &server_port;
        server_port.peer = &client_port;
#if EXAMPLE_RTU
        DemoServer<ServerLink, LoopPort> server{server_port};
#else
        DemoServer<CobsLink, LoopPort> server{server_port};
#endif
        if (!server.start()) { return 1; }
        const int result = exchange(client_port);
        qInfo() << "split-input Qt serial example:" << (result == 0 ? "PASS" : "FAIL");
        return result;
    }
    if (args.size() >= 3 && (args[1] == "--port" || args[1] == "--server")) {
        bool baud_ok = true;
        const int baud = args.size() > 3 ? args[3].toInt(&baud_ok) : 115200;
        QSerialPort port;
        port.setPortName(args[2]);
        if (!baud_ok || baud <= 0 || !port.setBaudRate(baud) ||
            !port.setDataBits(QSerialPort::Data8) || !port.setParity(QSerialPort::NoParity) ||
            !port.setStopBits(QSerialPort::OneStop) || !port.setFlowControl(QSerialPort::NoFlowControl) ||
            !port.open(QIODevice::ReadWrite)) { qCritical() << port.errorString(); return 1; }
        if (args[1] == "--port") { return exchange(port); }
#if EXAMPLE_RTU
        DemoServer<ServerLink, QSerialPort> server{port};
#else
        DemoServer<CobsLink, QSerialPort> server{port};
#endif
        if (!server.start()) { return 1; }
        qInfo() << "demo server; stop the process to exit";
        return app.exec();
    }
    qInfo() << "--self-test | --port NAME [BAUD] | --server NAME [BAUD]";
    qInfo() << "No port is opened unless --port or --server is explicit. Both peers must agree on format.";
    return args.size() == 1 || (args.size() == 2 && args[1] == "--help") ? 0 : 1;
}
