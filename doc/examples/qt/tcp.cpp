/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "modbus/tcp/Tcp.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QDebug>
#include <algorithm>
#include <array>

using Link = modbus::tcp::Endpoint<wire::Pool<4, 2>>;

// Example glue, NOT a production adapter exported by the library. One object
// owns one established connection. It accepts a complete frame into a bounded
// staging buffer before writing any prefix to the socket; partial writes are
// continued internally. Socket acceptance is not a peer acknowledgement.
// example-begin: qt-tcp-connection
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
// example-end: qt-tcp-connection

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2 || app.arguments()[1] != "--self-test") {
        qInfo() << "--self-test: real QTcpSocket/QTcpServer on loopback only; no board or remote server";
        const auto args = app.arguments();
        return args.size() == 1 || (args.size() == 2 && args[1] == "--help") ? 0 : 1;
    }
    QTcpServer listener;
    if (!listener.listen(QHostAddress::LocalHost, 0)) { return 1; }
    QTcpSocket client_socket;
    client_socket.connectToHost(QHostAddress::LocalHost, listener.serverPort());
    if (!client_socket.waitForConnected(1000) ||
        (!listener.hasPendingConnections() && !listener.waitForNewConnection(1000))) { return 1; }
    QTcpSocket* server_socket = listener.nextPendingConnection();
    if (!server_socket) { return 1; }
    Connection client{client_socket}, server{*server_socket};
    QEventLoop loop;
    QTimer deadline, application;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { loop.exit(2); });
    deadline.start(3000);

    // example-begin: qt-tcp-message
    auto request = client.endpoint.make_message(7u, 1u, 0x41u);
    if (!request || !request.append_be(uint16_t{1234}) ||
        client.endpoint.send(request) != wire::SendResult::Sent) { return 1; }
    // MBAP handles the custom function; no additional payload-length field.
    // example-end: qt-tcp-message
    Link::Message pending;
    bool echoed = false;
    QObject::connect(&application, &QTimer::timeout, &loop, [&] {
        if (!pending) {
            if (auto packet = server.endpoint.pop_packet()) {
                pending = server.endpoint.make_message(packet.transaction_id(), packet.unit_id(), packet.function());
                if (!pending || !pending.append_bytes(packet.data())) { loop.exit(1); return; }
            }
        }
        if (pending) {
            const auto result = server.endpoint.send(pending);
            if (result != wire::SendResult::Sent && result != wire::SendResult::Busy) { loop.exit(1); return; }
        }
        if (auto packet = client.endpoint.pop_packet()) {
            std::size_t offset = 0;
            uint16_t value = 0;
            if (packet.transaction_id() != 7u || packet.unit_id() != 1u || packet.function() != 0x41u ||
                !wire::read_be(packet.data(), offset, value) || value != 1234u || offset != packet.size()) {
                loop.exit(1); return;
            }
            echoed = true;
            // Deliberate invalid Protocol ID to test fail-closed socket teardown.
            const char invalid[]{0, 8, 0, 1, 0, 2, 1, 3};
            if (client_socket.write(invalid, sizeof(invalid)) != sizeof(invalid)) { loop.exit(1); }
        }
        if (server.failed()) { loop.exit(echoed && server.endpoint.rx_failed() ? 0 : 1); }
    });
    application.start(1);
    const int result = loop.exec();
    qInfo() << "Qt TCP localhost echo + invalid-MBAP disconnect:" << (result == 0 ? "PASS" : "FAIL");
    return result;
}
