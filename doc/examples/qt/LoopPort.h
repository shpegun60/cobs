/* Author: shpegun60; SPDX-License-Identifier: MIT */
#pragma once
#include <QIODevice>
#include <QPointer>
#include <QSerialPort>
#include <QTimer>
#include <cstring>

// TEST SCAFFOLDING ONLY: copies bytes asynchronously through the real Qt event
// loop. The application examples instantiate QSerialPort in --port mode.
class LoopPort final : public QIODevice {
    Q_OBJECT
public:
    QPointer<LoopPort> peer;
    LoopPort() { open(QIODevice::ReadWrite); }
    qint64 bytesToWrite() const override { return queued_; }
    bool clear(QSerialPort::Directions directions = QSerialPort::AllDirections) {
        if (directions.testFlag(QSerialPort::Input)) { input_.clear(); }
        if (directions.testFlag(QSerialPort::Output)) { queued_ = 0; ++generation_; }
        return true;
    }
    qint32 baudRate() const { return 115200; }
signals:
    void errorOccurred(QSerialPort::SerialPortError error);
protected:
    qint64 readData(char* bytes, qint64 maximum) override {
        const qint64 count = qMin(maximum, static_cast<qint64>(input_.size()));
        if (count > 0) { std::memcpy(bytes, input_.constData(), static_cast<std::size_t>(count)); }
        input_.remove(0, static_cast<qsizetype>(count));
        return count;
    }
    qint64 writeData(const char* bytes, qint64 count) override {
        if (!peer) { return -1; }
        const QByteArray copy(bytes, static_cast<qsizetype>(count));
        queued_ += count;
        const unsigned generation = generation_;
        QTimer::singleShot(0, this, [this, copy, generation] {
            if (generation != generation_) { return; }
            queued_ -= copy.size();
            if (peer) {
                // Two real callbacks: no promise that readAll() returns an ADU.
                const qsizetype cut = copy.size() / 2;
                peer->input_.append(copy.first(cut));
                emit peer->readyRead();
                peer->input_.append(copy.sliced(cut));
                emit peer->readyRead();
            }
            emit bytesWritten(copy.size());
        });
        return count;
    }
private:
    QByteArray input_;
    qint64 queued_ = 0;
    unsigned generation_ = 0;
};
