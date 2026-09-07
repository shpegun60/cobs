/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * qmodbus_bench — the PC side of the comparison between this repository's
 * Modbus RTU stack and Qt's QtSerialBus, over one real serial link to the
 * NUCLEO-H7S3L8 running modbus_bench.cpp in a role:
 *
 *   --role qtclient   Qt's QModbusRtuSerialClient runs the shared script
 *                     against the board's reference server (unit 0x11);
 *   --role ourclient  adapters/qt/RtuClient runs the same script against the
 *                     same server, so the two clients can be compared
 *                     scenario by scenario;
 *   --role qtserver   Qt's QModbusRtuSerialServer serves the reference model
 *                     at unit 0x0A while the board's client runs the script
 *                     against it; the final register map and every write Qt
 *                     saw are dumped when the time is up.
 *
 * Expectations come from modbus/rtu/tests/reference_model.h, the same header
 * the firmware serves and shadows, so a verdict of "ok" means the peer
 * answered exactly what the reference model predicts (or stayed silent
 * where the model says nobody answers). Results are one JSON document per
 * run; run_qmodbus.py drives the roles and binds the documents to the
 * flashed images.
 */

#include "adapters/qt/RtuClient.h"
#include "modbus/rtu/Rtu.h"
#include "reference_model.h"
#include "ServerTrace.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QModbusDataUnit>
#include <QModbusReply>
#include <QModbusRtuSerialClient>
#include <QModbusRtuSerialServer>
#include <QSerialPort>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace {

namespace ref = modbus_reference;
namespace framing = modbus::rtu::framing;
using Link = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<>,
	framing::Standard<framing::Direction::Response>>;
using OurClient = adapters::qt::RtuClient<Link, QSerialPort>;

struct Options final {
	QString role;
	QString port;
	int baud = 115200;
	int timeout_ms = 1000;
	int retries = 0;
	int seconds = 12;
	int server_inter_frame_us = -1;
	bool server_trace = false;
	int stall_first_read_fragment_ms = 0;
	QString out;
};

QString hex(const std::span<const uint8_t> bytes)
{
	return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(bytes.data()),
		static_cast<qsizetype>(bytes.size())).toHex());
}

// One scenario's outcome, the same shape for both clients.
struct Outcome final {
	QString status;        // ok | mismatch | timeout | unexpected | error
	QString detail;
	qint64 elapsed_us = 0;
	QString received;      // function byte + data, hex
	bool exception = false;
	int exception_code = 0;
};

QJsonObject scenario_json(const std::size_t index, const ref::script::Request& request,
                          const ref::Reply& expected, const Outcome& outcome)
{
	QJsonObject o;
	o["index"] = static_cast<int>(index);
	o["name"] = ref::script::name(index);
	o["address"] = request.address;
	o["function"] = request.function;
	o["request"] = hex(request.span());
	o["expects_response"] = expected.respond;
	if (expected.respond) {
		o["expected_function"] = expected.function;
		o["expected"] = hex(expected.span());
	}
	o["status"] = outcome.status;
	o["detail"] = outcome.detail;
	o["elapsed_us"] = static_cast<double>(outcome.elapsed_us);
	o["received"] = outcome.received;
	if (outcome.exception) {
		o["exception_code"] = outcome.exception_code;
	}
	return o;
}

// Compares what came back (function byte with its exception bit, data) to the
// reference reply.
Outcome judge(const ref::Reply& expected, const bool responded, const uint8_t function,
              const std::span<const uint8_t> data, const qint64 elapsed_us)
{
	Outcome outcome;
	outcome.elapsed_us = elapsed_us;
	if (!responded) {
		outcome.status = expected.respond ? "timeout" : "ok";
		return outcome;
	}
	outcome.received = QString("%1 %2").arg(function, 2, 16, QChar('0')).arg(hex(data));
	outcome.exception = (function & 0x80u) != 0u;
	outcome.exception_code = outcome.exception && !data.empty() ? data[0] : 0;
	if (!expected.respond) {
		outcome.status = "unexpected";
		return outcome;
	}
	const std::span<const uint8_t> want = expected.span();
	if (function == expected.function && data.size() == want.size() &&
	    std::equal(data.begin(), data.end(), want.begin())) {
		outcome.status = "ok";
	} else {
		outcome.status = "mismatch";
		outcome.detail = QString("expected %1 %2").arg(expected.function, 2, 16, QChar('0')).arg(hex(want));
	}
	return outcome;
}

QJsonObject summary(const QJsonArray& scenarios)
{
	QJsonObject s;
	int ok = 0, mismatch = 0, timeout = 0, unexpected = 0, error = 0;
	double total_us = 0.0;
	std::vector<double> burst;
	for (const auto& value : scenarios) {
		const QJsonObject o = value.toObject();
		const QString status = o["status"].toString();
		ok += status == "ok";
		mismatch += status == "mismatch";
		timeout += status == "timeout";
		unexpected += status == "unexpected";
		error += status == "error";
		total_us += o["elapsed_us"].toDouble();
		if (o["index"].toInt() >= static_cast<int>(ref::script::kScriptedSteps)) {
			burst.push_back(o["elapsed_us"].toDouble());
		}
	}
	s["ok"] = ok;
	s["mismatch"] = mismatch;
	s["timeout"] = timeout;
	s["unexpected"] = unexpected;
	s["error"] = error;
	s["total_us"] = total_us;
	if (!burst.empty()) {
		std::sort(burst.begin(), burst.end());
		s["burst_median_us"] = burst[burst.size() / 2];
		s["burst_min_us"] = burst.front();
		s["burst_max_us"] = burst.back();
	}
	return s;
}

bool write_json(const QString& path, const QJsonObject& document)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
		return false;
	}
	file.write(QJsonDocument(document).toJson(QJsonDocument::Indented));
	return true;
}

// ------------------------------------------------------------------ Qt's client
int run_qt_client(const Options& options)
{
	QModbusRtuSerialClient client;
	client.setConnectionParameter(QModbusDevice::SerialPortNameParameter, options.port);
	client.setConnectionParameter(QModbusDevice::SerialBaudRateParameter, options.baud);
	client.setConnectionParameter(QModbusDevice::SerialDataBitsParameter, QSerialPort::Data8);
	client.setConnectionParameter(QModbusDevice::SerialParityParameter, QSerialPort::NoParity);
	client.setConnectionParameter(QModbusDevice::SerialStopBitsParameter, QSerialPort::OneStop);
	client.setTimeout(options.timeout_ms);
	client.setNumberOfRetries(options.retries);
	if (!client.connectDevice()) {
		std::fprintf(stderr, "QModbusRtuSerialClient: %s\n", qPrintable(client.errorString()));
		return 2;
	}

	ref::Model model;
	model.reset();
	QJsonArray scenarios;
	for (std::size_t index = 0; index < ref::script::kSteps; ++index) {
		ref::script::Request request;
		ref::script::build(index, ref::kBoardUnit, request);
		const ref::Reply expected = ref::serve(model, ref::kBoardUnit, request.address, request.function, request.span());

		const QByteArray data(reinterpret_cast<const char*>(request.data.data()), static_cast<qsizetype>(request.size));
		QElapsedTimer clock;
		clock.start();
		QModbusReply* reply = client.sendRawRequest(
			QModbusRequest(static_cast<QModbusPdu::FunctionCode>(request.function), data), request.address);
		Outcome outcome;
		if (reply == nullptr) {
			outcome.status = "error";
			outcome.detail = client.errorString();
		} else {
			if (!reply->isFinished()) {
				QEventLoop loop;
				QObject::connect(reply, &QModbusReply::finished, &loop, &QEventLoop::quit);
				QTimer::singleShot((options.timeout_ms + 500) * (options.retries + 1) + 2000, &loop, &QEventLoop::quit);
				loop.exec();
			}
			const qint64 elapsed_us = clock.nsecsElapsed() / 1000;
			if (!reply->isFinished()) {
				outcome.status = "error";
				outcome.detail = "reply never finished";
				outcome.elapsed_us = elapsed_us;
			} else if (reply->error() == QModbusDevice::NoError) {
				const QModbusResponse raw = reply->rawResult();
				if (request.address == 0) {
					outcome = judge(expected, false, 0u, {}, elapsed_us);   // a broadcast: nothing comes back
				} else {
					const QByteArray body = raw.data();
					outcome = judge(expected, true, static_cast<uint8_t>(raw.functionCode()),
						std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(body.constData()),
						                         static_cast<std::size_t>(body.size())}, elapsed_us);
				}
			} else if (reply->error() == QModbusDevice::ProtocolError) {
				const QModbusResponse raw = reply->rawResult();
				const QByteArray body = raw.data();
				outcome = judge(expected, true, static_cast<uint8_t>(raw.functionCode() | 0x80u),
					std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(body.constData()),
					                         static_cast<std::size_t>(body.size())}, elapsed_us);
			} else if (reply->error() == QModbusDevice::TimeoutError) {
				outcome = judge(expected, false, 0u, {}, elapsed_us);
			} else if (reply->error() == QModbusDevice::InvalidResponseError &&
			           reply->rawResult().isValid()) {
				// Qt's client models no data unit for some functions (Diagnostics
				// among them) and reports the response as invalid at its API even
				// when the exchange on the wire was exactly right. The wire is
				// what is being compared, so the raw response is judged and Qt's
				// verdict is kept beside it.
				const QModbusResponse raw = reply->rawResult();
				const QByteArray body = raw.data();
				outcome = judge(expected, true, static_cast<uint8_t>(raw.functionCode()),
					std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(body.constData()),
					                         static_cast<std::size_t>(body.size())}, elapsed_us);
				outcome.detail = QString("Qt reports \"%1\"; the raw response is what is judged").arg(reply->errorString());
			} else {
				outcome.status = "error";
				outcome.detail = reply->errorString();
				outcome.elapsed_us = elapsed_us;
			}
			reply->deleteLater();
		}
		scenarios.append(scenario_json(index, request, expected, outcome));
		std::printf("%2zu %-34s %-10s %8lld us %s\n", index, ref::script::name(index),
			qPrintable(outcome.status), static_cast<long long>(outcome.elapsed_us), qPrintable(outcome.detail));
	}
	client.disconnectDevice();

	QJsonObject document;
	document["role"] = "qtclient";
	document["implementation"] = QString("QtSerialBus %1 QModbusRtuSerialClient").arg(QT_VERSION_STR);
	document["port"] = options.port;
	document["baud"] = options.baud;
	document["unit"] = ref::kBoardUnit;
	document["timeout_ms"] = options.timeout_ms;
	document["retries"] = options.retries;
	document["inter_frame_delay_us"] = client.interFrameDelay();
	document["scenarios"] = scenarios;
	document["summary"] = summary(scenarios);
	return write_json(options.out, document) ? 0 : 3;
}

// ------------------------------------------------------------------ our client
int run_our_client(const Options& options)
{
	QSerialPort port;
	port.setPortName(options.port);
	port.setBaudRate(options.baud);
	port.setDataBits(QSerialPort::Data8);
	port.setParity(QSerialPort::NoParity);
	port.setStopBits(QSerialPort::OneStop);
	if (!port.open(QIODevice::ReadWrite)) {
		std::fprintf(stderr, "QSerialPort: %s\n", qPrintable(port.errorString()));
		return 2;
	}
	Link link;
	OurClient client{port, link};
	client.set_response_timeout_ms(options.timeout_ms);
	client.set_retries(options.retries);
	client.update_timing_from_port();
	if (!client.bind()) {
		std::fprintf(stderr, "RtuClient: bind failed\n");
		return 2;
	}

	ref::Model model;
	model.reset();
	QJsonArray scenarios;
	for (std::size_t index = 0; index < ref::script::kSteps; ++index) {
		ref::script::Request request;
		ref::script::build(index, ref::kBoardUnit, request);
		const ref::Reply expected = ref::serve(model, ref::kBoardUnit, request.address, request.function, request.span());

		struct Captured final {
			bool done = false;
			adapters::qt::RequestState state = adapters::qt::RequestState::Timeout;
			uint8_t function = 0u;
			bool exception = false;
			std::vector<uint8_t> data;
		} captured;
		QEventLoop loop;
		QElapsedTimer clock;
		clock.start();
		const bool queued = client.send(request.address, request.function, request.span(),
			OurClient::Handler{[&captured, &loop](const adapters::qt::Response& response) {
				captured.done = true;
				captured.state = response.state;
				captured.function = response.function;
				captured.exception = response.exception;
				captured.data.assign(response.data.begin(), response.data.end());
				loop.quit();
			}});
		Outcome outcome;
		if (!queued) {
			outcome.status = "error";
			outcome.detail = "send refused";
		} else {
			QTimer::singleShot((options.timeout_ms + 500) * (options.retries + 1) + 2000, &loop, &QEventLoop::quit);
			loop.exec();
			const qint64 elapsed_us = clock.nsecsElapsed() / 1000;
			if (!captured.done) {
				outcome.status = "error";
				outcome.detail = "handler never ran";
				outcome.elapsed_us = elapsed_us;
			} else {
				switch (captured.state) {
				case adapters::qt::RequestState::Completed:
					outcome = judge(expected, true,
						static_cast<uint8_t>(captured.function | (captured.exception ? 0x80u : 0u)),
						std::span<const uint8_t>{captured.data}, elapsed_us);
					break;
				case adapters::qt::RequestState::Broadcast:
				case adapters::qt::RequestState::Timeout:
					outcome = judge(expected, false, 0u, {}, elapsed_us);
					break;
				case adapters::qt::RequestState::WriteError:
					outcome.status = "error";
					outcome.detail = "write error";
					outcome.elapsed_us = elapsed_us;
					break;
				case adapters::qt::RequestState::Cancelled:
					outcome.status = "error";
					outcome.detail = "cancelled";
					outcome.elapsed_us = elapsed_us;
					break;
				}
			}
		}
		scenarios.append(scenario_json(index, request, expected, outcome));
		std::printf("%2zu %-34s %-10s %8lld us %s\n", index, ref::script::name(index),
			qPrintable(outcome.status), static_cast<long long>(outcome.elapsed_us), qPrintable(outcome.detail));
	}
	(void)client.unbind();
	port.close();

	QJsonObject document;
	document["role"] = "ourclient";
	document["implementation"] = "adapters/qt/RtuClient over adapters/qt/SerialAdapter";
	document["port"] = options.port;
	document["baud"] = options.baud;
	document["unit"] = ref::kBoardUnit;
	document["timeout_ms"] = options.timeout_ms;
	document["retries"] = options.retries;
	document["inter_frame_delay_us"] = client.inter_frame_delay_ms() * 1000;
	document["scenarios"] = scenarios;
	document["summary"] = summary(scenarios);
	QJsonObject stats;
	stats["rx_candidates"] = static_cast<double>(link.stats().rx.candidates);
	stats["rx_crc_errors"] = static_cast<double>(link.stats().rx.crc_errors);
	stats["rx_stream_gaps"] = static_cast<double>(link.stats().rx.stream_gaps);
	stats["stale_frames"] = static_cast<double>(link.framing_stats().stale_frames);
	stats["resyncs"] = static_cast<double>(link.framing_stats().resyncs);
	stats["unsupported_function"] = static_cast<double>(link.framing_stats().unsupported_function);
	stats["tx_layout_rejected"] = static_cast<double>(link.framing_stats().tx_layout_rejected);
	document["endpoint_stats"] = stats;
	return write_json(options.out, document) ? 0 : 3;
}

// ------------------------------------------------------------------ Qt's server
int run_qt_server(const Options& options)
{
	ServerTrace trace(options.server_trace, options.stall_first_read_fragment_ms);
	QModbusRtuSerialServer server;
	server.setConnectionParameter(QModbusDevice::SerialPortNameParameter, options.port);
	server.setConnectionParameter(QModbusDevice::SerialBaudRateParameter, options.baud);
	server.setConnectionParameter(QModbusDevice::SerialDataBitsParameter, QSerialPort::Data8);
	server.setConnectionParameter(QModbusDevice::SerialParityParameter, QSerialPort::NoParity);
	server.setConnectionParameter(QModbusDevice::SerialStopBitsParameter, QSerialPort::OneStop);
	server.setServerAddress(ref::kPcUnit);
	if (options.server_inter_frame_us >= 0) {
		server.setInterFrameDelay(options.server_inter_frame_us);
	}

	QModbusDataUnitMap map;
	map.insert(QModbusDataUnit::HoldingRegisters, {QModbusDataUnit::HoldingRegisters, 0, static_cast<quint16>(ref::kHoldingCount)});
	map.insert(QModbusDataUnit::InputRegisters, {QModbusDataUnit::InputRegisters, 0, static_cast<quint16>(ref::kInputCount)});
	map.insert(QModbusDataUnit::Coils, {QModbusDataUnit::Coils, 0, static_cast<quint16>(ref::kCoilCount)});
	map.insert(QModbusDataUnit::DiscreteInputs, {QModbusDataUnit::DiscreteInputs, 0, static_cast<quint16>(ref::kDiscreteCount)});
	if (!server.setMap(map)) {
		std::fprintf(stderr, "QModbusRtuSerialServer: setMap failed\n");
		return 2;
	}
	ref::Model model;
	model.reset();
	for (std::size_t i = 0; i < ref::kHoldingCount; ++i) {
		server.setData(QModbusDataUnit::HoldingRegisters, static_cast<quint16>(i), model.holding[i]);
	}
	for (std::size_t i = 0; i < ref::kInputCount; ++i) {
		server.setData(QModbusDataUnit::InputRegisters, static_cast<quint16>(i), model.input[i]);
	}
	for (std::size_t i = 0; i < ref::kCoilCount; ++i) {
		server.setData(QModbusDataUnit::Coils, static_cast<quint16>(i), ref::Model::bit(model.coils, i) ? 1 : 0);
	}
	for (std::size_t i = 0; i < ref::kDiscreteCount; ++i) {
		server.setData(QModbusDataUnit::DiscreteInputs, static_cast<quint16>(i), ref::Model::bit(model.discrete, i) ? 1 : 0);
	}

	QJsonArray writes;
	QObject::connect(&server, &QModbusServer::dataWritten, &server,
		[&writes](const QModbusDataUnit::RegisterType table, const int address, const int size) {
			QJsonObject w;
			w["table"] = static_cast<int>(table);
			w["address"] = address;
			w["size"] = size;
			writes.append(w);
		});
	QJsonArray errors;
	QObject::connect(&server, &QModbusDevice::errorOccurred, &server,
		[&errors, &server](const QModbusDevice::Error error) {
			QJsonObject e;
			e["error"] = static_cast<int>(error);
			e["text"] = server.errorString();
			errors.append(e);
		});
	if (!server.connectDevice()) {
		std::fprintf(stderr, "QModbusRtuSerialServer: %s\n", qPrintable(server.errorString()));
		return 2;
	}
	std::printf("QModbusRtuSerialServer at unit 0x%02X on %s, %d baud, serving for %d s\n",
		ref::kPcUnit, qPrintable(options.port), options.baud, options.seconds);
	{
		QEventLoop loop;
		QTimer::singleShot(options.seconds * 1000, &loop, &QEventLoop::quit);
		loop.exec();
	}
	server.disconnectDevice();

	QJsonObject document;
	document["role"] = "qtserver";
	document["implementation"] = QString("QtSerialBus %1 QModbusRtuSerialServer").arg(QT_VERSION_STR);
	document["port"] = options.port;
	document["baud"] = options.baud;
	document["unit"] = ref::kPcUnit;
	document["seconds"] = options.seconds;
	document["inter_frame_delay_us"] = server.interFrameDelay();
	document["trace"] = trace.document();
	document["writes"] = writes;
	document["errors"] = errors;
	QJsonArray holding, coils;
	for (std::size_t i = 0; i < ref::kHoldingCount; ++i) {
		quint16 value = 0;
		server.data(QModbusDataUnit::HoldingRegisters, static_cast<quint16>(i), &value);
		holding.append(value);
	}
	for (std::size_t i = 0; i < ref::kCoilCount; ++i) {
		quint16 value = 0;
		server.data(QModbusDataUnit::Coils, static_cast<quint16>(i), &value);
		coils.append(value != 0 ? 1 : 0);
	}
	document["final_holding"] = holding;
	document["final_coils"] = coils;
	std::printf("%lld writes recorded, %lld errors\n", static_cast<long long>(writes.size()),
		static_cast<long long>(errors.size()));
	return write_json(options.out, document) ? 0 : 3;
}

} // namespace

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	QCommandLineParser parser;
	parser.setApplicationDescription("PC side of the QModbus comparison on the H7S");
	parser.addHelpOption();
	parser.addOption({"role", "qtclient | ourclient | qtserver", "role"});
	parser.addOption({"port", "serial port name", "port", "COM6"});
	parser.addOption({"baud", "baud rate", "baud", "115200"});
	parser.addOption({"timeout", "response timeout in ms (clients)", "ms", "1000"});
	parser.addOption({"retries", "retries per request (clients)", "n", "0"});
	parser.addOption({"seconds", "how long the server serves", "s", "12"});
	parser.addOption({"server-inter-frame-us", "Qt server RX fragment deadline; -1 keeps Qt's default", "us", "-1"});
	parser.addOption({"server-trace", "capture Qt server decisions in memory"});
	parser.addOption({"stall-first-read-fragment-ms", "test only: stall host processing once after an incomplete FC03 request", "ms", "0"});
	parser.addOption({"out", "JSON output file", "path"});
	parser.process(app);

	Options options;
	options.role = parser.value("role");
	options.port = parser.value("port");
	options.baud = parser.value("baud").toInt();
	options.timeout_ms = parser.value("timeout").toInt();
	options.retries = parser.value("retries").toInt();
	options.seconds = parser.value("seconds").toInt();
	options.server_inter_frame_us = parser.value("server-inter-frame-us").toInt();
	options.server_trace = parser.isSet("server-trace");
	options.stall_first_read_fragment_ms = parser.value("stall-first-read-fragment-ms").toInt();
	options.out = parser.value("out");
	if (options.out.isEmpty() || options.baud <= 0 || options.server_inter_frame_us < -1 ||
		options.server_inter_frame_us > 1000000 || options.stall_first_read_fragment_ms < 0 ||
		options.stall_first_read_fragment_ms > 200 ||
		(options.stall_first_read_fragment_ms > 0 && !options.server_trace)) {
		parser.showHelp(1);
	}
	if (options.role == "qtclient") {
		return run_qt_client(options);
	}
	if (options.role == "ourclient") {
		return run_our_client(options);
	}
	if (options.role == "qtserver") {
		return run_qt_server(options);
	}
	parser.showHelp(1);
}
