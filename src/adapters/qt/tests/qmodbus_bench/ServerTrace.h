/* Author: shpegun60
 * SPDX-License-Identifier: MIT
 */
#ifndef QMODBUS_BENCH_SERVER_TRACE_H_
#define QMODBUS_BENCH_SERVER_TRACE_H_

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <vector>
#include <cstdio>

// Test-process instrumentation only, never included by a production adapter.
// Captures Qt's own decisions in memory, without synchronous stderr/file I/O
// in readyRead. The optional one-shot stall models delayed HOST processing;
// it neither delays the board's UART TX nor changes a byte of its request.
// One capture per isolated, single-Modbus-server process.
class ServerTrace final {
public:
	ServerTrace(const bool enabled, const int stall_first_read_fragment_ms)
		: m_enabled(enabled), m_stall_ms(stall_first_read_fragment_ms)
	{
		if (m_enabled) {
			m_entries.reserve(max_entries);
			m_clock.start();
			s_current = this;
			m_previous = qInstallMessageHandler(&capture);
			QLoggingCategory::setFilterRules(QStringLiteral("qt.modbus.debug=true\nqt.modbus.lowlevel.debug=true"));
		}
	}

	~ServerTrace()
	{
		if (m_enabled) {
			qInstallMessageHandler(m_previous);
			s_current = nullptr;
		}
	}
	ServerTrace(const ServerTrace&) = delete;
	ServerTrace& operator=(const ServerTrace&) = delete;

	QJsonObject document() const
	{
		QMutexLocker lock(&m_mutex);
		QJsonArray entries;
		for (const auto& entry : m_entries) {
			entries.append(QJsonObject{{"at_us", static_cast<double>(entry.at_us)},
				{"category", QString::fromLatin1(entry.category)}, {"message", entry.message}});
		}
		return {{"enabled", m_enabled}, {"overflow", m_overflow}, {"entries", entries},
			{"stall_requested_ms", m_stall_ms}, {"stall_injected", m_stalled},
			{"stall_fragment", QString::fromLatin1(m_fragment.toHex())},
			{"stall_elapsed_us", static_cast<double>(m_stall_elapsed_us)}};
	}

	// FC03 requests are always eight bytes including address and CRC. Do not
	// stall an already complete request or the unknown-function control case.
	static QByteArray read_fragment(const QString& message)
	{
		if (!message.startsWith(QLatin1String("(RTU server) Received ADU: "))) {
			return {};
		}
		const auto first = message.indexOf(QChar('"'));
		const auto last = message.lastIndexOf(QChar('"'));
		if (first < 0 || last <= first) {
			return {};
		}
		const QByteArray raw = QByteArray::fromHex(message.mid(first + 1, last - first - 1).toLatin1());
		if (raw.isEmpty() || raw.size() >= 8 || raw[0] != '\x0a' ||
			(raw.size() > 1 && raw[1] != '\x03')) {
			return {};
		}
		return raw;
	}

private:
	struct Entry final { qint64 at_us; QByteArray category; QString message; };
	static constexpr std::size_t max_entries = 4096;
	inline static ServerTrace* s_current = nullptr;

	static void capture(QtMsgType type, const QMessageLogContext& context, const QString& message)
	{
		auto* const self = s_current;
		if (self == nullptr) {
			return;
		}
		const QByteArray category(context.category);
		if (category != "qt.modbus" && category != "qt.modbus.lowlevel") {
			if (self->m_previous != nullptr) {
				self->m_previous(type, context, message);
			} else {
				const QByteArray line = qFormatLogMessage(type, context, message).toLocal8Bit();
				std::fprintf(stderr, "%s\n", line.constData());
			}
			return;
		}
		bool stall = false;
		{
			QMutexLocker lock(&self->m_mutex);
			if (self->m_entries.size() == max_entries) {
				self->m_overflow = true;
				return;
			}
			self->m_entries.push_back({self->m_clock.nsecsElapsed() / 1000, category, message});
			if (self->m_stall_ms > 0 && !self->m_stalled) {
				self->m_fragment = read_fragment(message);
				stall = !self->m_fragment.isEmpty();
				self->m_stalled = stall;
			}
		}
		if (stall) {
			QElapsedTimer elapsed;
			elapsed.start();
			QThread::msleep(static_cast<unsigned long>(self->m_stall_ms));
			QMutexLocker lock(&self->m_mutex);
			self->m_stall_elapsed_us = elapsed.nsecsElapsed() / 1000;
		}
	}

	bool m_enabled;
	int m_stall_ms;
	QtMessageHandler m_previous = nullptr;
	QElapsedTimer m_clock;
	mutable QMutex m_mutex;
	std::vector<Entry> m_entries;
	bool m_overflow = false;
	bool m_stalled = false;
	QByteArray m_fragment;
	qint64 m_stall_elapsed_us = 0;
};

#endif
