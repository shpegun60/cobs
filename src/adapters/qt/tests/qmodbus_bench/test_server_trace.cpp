/* Author: shpegun60
 * SPDX-License-Identifier: MIT
 */
#include "ServerTrace.h"

#include <QCoreApplication>
#include <QDebug>

#include <cstdio>

Q_LOGGING_CATEGORY(trace_test_messages, "qt.modbus.lowlevel")

namespace {
int checks = 0;
int forwarded = 0;
void previous_handler(QtMsgType, const QMessageLogContext&, const QString&) { ++forwarded; }
}

#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); return 1; } } while (false)

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	const auto fragment = [](const char* hex) {
		return ServerTrace::read_fragment(QStringLiteral("(RTU server) Received ADU: \"") +
			QString::fromLatin1(hex) + QChar('"'));
	};
	CHECK(fragment("0a") == QByteArray::fromHex("0a"));
	CHECK(fragment("0a03") == QByteArray::fromHex("0a03"));
	CHECK(fragment("0a030000000ac4") == QByteArray::fromHex("0a030000000ac4"));
	CHECK(fragment("0a030000000ac4b6").isEmpty());
	CHECK(fragment("1103").isEmpty());
	CHECK(fragment("0a04").isEmpty());
	CHECK(fragment("0a64").isEmpty());
	CHECK(fragment("").isEmpty());
	CHECK(ServerTrace::read_fragment(QStringLiteral("unrelated warning")).isEmpty());
	{
		ServerTrace trace(false, 0);
		CHECK(!trace.document()["enabled"].toBool());
		CHECK(trace.document()["entries"].toArray().isEmpty());
	}
	const QtMessageHandler original = qInstallMessageHandler(previous_handler);
	{
		ServerTrace trace(true, 1);
		qWarning("unrelated message");
		CHECK(forwarded == 1);
		qCDebug(trace_test_messages) << "(RTU server) Received ADU:" << QByteArray::fromHex("0a03").toHex();
		qCDebug(trace_test_messages) << "(RTU server) Received ADU:" << QByteArray::fromHex("0a030000000ac4b6").toHex();
		const QJsonObject record = trace.document();
		CHECK(record["enabled"].toBool());
		CHECK(record["stall_injected"].toBool());
		CHECK(record["stall_fragment"].toString() == "0a03");
		CHECK(record["stall_elapsed_us"].toDouble() >= 1000.0);
		CHECK(record["entries"].toArray().size() == 2);
		CHECK(!record["overflow"].toBool());
	}
	qWarning("after capture");
	CHECK(forwarded == 2);
	{
		ServerTrace trace(true, 0);
		for (int i = 0; i < 4097; ++i) {
			qCDebug(trace_test_messages) << "test" << i;
		}
		CHECK(trace.document()["overflow"].toBool());
		CHECK(trace.document()["entries"].toArray().size() == 4096);
		CHECK(!trace.document()["stall_injected"].toBool());
	}
	qInstallMessageHandler(original);
	std::printf("PASS %d Qt server trace checks (no COM port)\n", checks);
	return 0;
}
