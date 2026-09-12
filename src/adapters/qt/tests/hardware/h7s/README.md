# The Modbus RTU stack against QtSerialBus, on the NUCLEO-H7S3L8

Latest extension-contract repeat: [raw record](results_qmodbus_extensions_2026-09-12.json)
and [receipt](results_qmodbus_extensions_2026-09-12.json.session.json). All eight
role/baud/framer runs passed: 652 ok and eight expected step-19 timeouts among
660 verdicts, zero retries and no unexpected outcomes. All images programmed
on attempt one and original flash was restored/read back. See the
[full H7S repeat](../../../../../../doc/HARDWARE_EXTENSIONS_2026-09-12.md).

Earlier repeat after the UART/Qt cross-stack fixes: [12 September audit record](results_qmodbus_audit_2026-09-12.json)
and [image/restore receipt](results_qmodbus_audit_2026-09-12.json.session.json).
All **8 role/baud/framer runs**, **660 reference-model verdicts** matched
the expected outcomes, with zero retries and no unexpected timeouts. The
unknown-function timeouts remain expected for table-framed servers, as
explained below. Original boot flash was restored and the actual full
read-back rehashed; all eight retained ELF/flash identities were checked.
The [cross-stack audit](../../../../../../doc/PARANOID_AUDIT_2026-09-12.md)
keeps the Qt COBS gap fix separate from the RTU empty-delivery deadline fix:
COBS has no silence timer. Historical tables below keep their original date.

The [client recovery follow-up](../../../../../../doc/QT_CLIENT_RECOVERY.md)
fixes production desktop RX/TX ordering, write deadlines, retry/cancellation
accounting and nested port-error cleanup. Fault orderings are tested on the
host; new live interop records separately exercise the updated client.
Both full rounds passed: [round 1](results_qmodbus_2026-09-07_recovery1.json)
and [round 2](results_qmodbus_2026-09-07_recovery2.json), 16 role runs and
1,320 reference-model verdicts, zero unexpected outcomes, original boot
restored/read back in both sessions. Offline guard:
`python -B src/adapters/qt/tests/hardware/h7s/test_client_recovery.py`.

The [Qt/USB timeout follow-up](../../../../../../doc/QT_USB_TIMEOUT_DIAGNOSIS.md)
reproduces the native Qt server's 2-ms fragment-discard failure and verifies
the USB-aware 50-ms RX configuration. The runner now uses that setting,
keeps an in-memory trace by default, and returns the verifier's exit code
after restoring the board. Response timeout/retries/wire bytes are unchanged.

The [7 September repeat](../../../../../../doc/HARDWARE_REGRESSION_2026-09-07.md)
retains two unexpected Qt-server timeouts, a diagnostic fragment-discard
trace and the passing targeted/full control repeats. The original failing
records still fail `verify_qmodbus.py`; see `test_regression_evidence.py`.
The tables and findings below describe the **6 September** session and are
not an assertion that every subsequent exchange was successful.

Qt's `QtSerialBus` is the reference this repository's Modbus RTU stack is
measured against: the stack has to be a usable replacement, "не гірше".
This directory holds the measurement, both ways round, over the board's real
UART and the ST-Link virtual COM port.

## What is compared

One request script, `modbus_reference::script` in
[`src/modbus/rtu/tests/reference_model.h`](../../../../../modbus/rtu/tests/reference_model.h):
55 steps — reads of every table, writes of every kind each verified by a
read-back, the largest legal request and response (123 registers written,
125 read), the exceptions the specification defines (address out of range,
quantity 0, an unknown function, an illegal coil value), a diagnostics echo,
a request to a unit nobody has, a broadcast write verified by a read-back,
and thirty identical reads back to back for timing. One reference data model,
`modbus_reference::Model`, with `serve()` answering a request the way a
server holding that model does. Every party in the comparison uses the same
header, so a verdict of **ok** means "the peer answered exactly what the
reference model predicts, or stayed silent where the model says nobody
answers".

**The board as a server.** The harness firmware built with `MODBUS_HW_ROLE=1`
serves the model at unit 0x11 through the production stack (`Uart<256,4>`,
`UartAdapter`, `modbus::rtu::Endpoint`). Two clients on the PC run the
script against it in turn, the model reset between them:

- `QModbusRtuSerialClient` from QtSerialBus 6.4.3, driven through
  `sendRawRequest()` so that every request goes out byte for byte as
  scripted;
- `adapters::qt::RtuClient`, this repository's master shaped like Qt's, over
  `adapters::qt::SerialAdapter` and a `QSerialPort`.

Both run with a 1000 ms response timeout and no retries, so nothing masks a
failure, and both record the round trip of every scenario.

**The board as a client.** The firmware built with `MODBUS_HW_ROLE=2` runs
the same script against `QModbusRtuSerialServer` on the PC, which serves the
same model at unit 0x0A. The board keeps the model as a shadow, predicts
every response from it, records each step's verdict and round trip, and the
PC collects the report through the harness control protocol afterwards;
Qt's final register map and the writes Qt saw are recorded as well.

Each direction runs at 115200 and 1 M baud, with the board's endpoint in
both framing modes: the default burst endpoint (one IDLE-ended DMA burst is
one candidate) and the framing policy (`framing::Standard`, frame ends from
the bytes, the harness control function as a private length-prefixed one).

## How to run and recheck

```powershell
python -B src/adapters/qt/tests/hardware/h7s/run_qmodbus.py --port COM6 `
  --serial 002A001F3033510135393935 `
  --output src/adapters/qt/tests/hardware/h7s/results_qmodbus_NEW.json

# Recheck the historical 6 September tables below, without flashing:
python -B src/adapters/qt/tests/hardware/h7s/verify_qmodbus.py `
  src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-06.json `
  --check-doc src/adapters/qt/tests/hardware/h7s/README.md
```

`run_qmodbus.py` builds the PC runner
([`qmodbus_bench`](../../qmodbus_bench/main.cpp)) with the Qt 6.4.3 kit, the
only local kit with QtSerialBus, backs up the board's flash, then for every
baud and framing mode builds, inspects and flashes the two role images and
drives the runs; the flash is restored, verified and read back last. The
record carries the SHA-256 of every source it was built from and the flashed
images; `verify_qmodbus.py` rechecks every verdict against the expectations
below, the writes in Qt's final map, the flash restore and the provenance,
and prints the tables that follow.

Normal runs use `--server-inter-frame-us 50000` and `--server-trace` by
default; `--server-inter-frame-us -1` retains Qt's native RX timer, and
`--no-server-trace` disables the journal explicitly. The optional
`--stall-first-read-fragment-ms 10` is a host-side fault-injection test, not
part of normal traffic; see the follow-up report before using it.

## What the run showed

The only verdicts other than **ok**, in either direction, follow from two
facts about length-driven framing and QtSerialBus, and are written into the
verifier as the only acceptable exceptions:

- **An unknown function code cannot be framed.** Script step 19 sends
  function 0x64. A server that finds frame ends from a length table has no
  entry for it and never sees a complete request: the board's framed server
  and Qt's server (which frames the same way) stay silent where the
  specification asks for exception 01, so the client times out. Only the
  board's burst server, which takes a whole IDLE-ended burst as one
  candidate, answers 01 — and the burst server is what the default endpoint
  is. A client sends the unknown function as it is: the builder has no
  opinion about a function its table does not know, the peer decides.
- **Qt's client does not model Diagnostics.** Step 20 is function 0x08,
  sub-function 0 (return query data). The board's server echoes it exactly
  as the model predicts, `QModbusRtuSerialClient` receives the echo intact
  (its private code even special-cases this response), and then reports
  `InvalidResponseError` because it has no data unit to map function 0x08
  into. The runner judges the raw response, which is what is being
  compared, and keeps Qt's verdict in the scenario's detail.

Everything else matched, in both directions, at both bauds, in both framing
modes: the same script, the same model, the two clients' verdicts identical
scenario for scenario, Qt's server left with exactly the register map the
board's writes produce, and the board's server left with exactly the map the
PC clients' writes produce.

The tables below are printed by `verify_qmodbus.py` from the record;
`--check-doc` fails when they drift.

Record: [`results_qmodbus_2026-09-06.json`](results_qmodbus_2026-09-06.json), taken against commit `2d7e90d`; the burst medians are thirty back-to-back reads of ten registers, round trip as the client measures it. The lifecycle and failure-path cleanup of `SerialAdapter`/`RtuClient` that followed (destructor unbinding the endpoint, write and resource errors ending the transaction, partial writes, the turnaround kept across a reentrant send, a lowerable inter-frame floor) touched no path this normal-flow comparison exercises; those paths are proven on the fake port by `sh src/adapters/qt/tests/run.sh`.

### The board as a server: Qt's client and this repository's client, same script, same server

| Baud | Board server | QModbusRtuSerialClient ok | RtuClient ok | Qt burst median | RtuClient burst median | Qt total | RtuClient total | served by the board |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 115200 | burst | 55/55 | 55/55 | 5.37 ms | 5.34 ms | 1334.36 ms | 1332.74 ms | 106 |
| 1000000 | burst | 55/55 | 55/55 | 2.89 ms | 2.84 ms | 1159.57 ms | 1164.70 ms | 106 |
| 115200 | framing policy | 54/55 | 54/55 | 5.29 ms | 5.24 ms | 2325.42 ms | 2322.43 ms | 104 |
| 1000000 | framing policy | 54/55 | 54/55 | 2.85 ms | 2.84 ms | 2157.58 ms | 2156.56 ms | 104 |

### The board as a client against QModbusRtuSerialServer

| Baud | Board client | steps ok | burst median round trip | writes Qt recorded | steps not ok |
|---:|---|---:|---:|---:|---|
| 115200 | burst | 54/55 | 3.18 ms | 7 | 19 timeout |
| 1000000 | burst | 54/55 | 0.66 ms | 7 | 19 timeout |
| 115200 | framing policy | 54/55 | 3.20 ms | 7 | 19 timeout |
| 1000000 | framing policy | 54/55 | 0.72 ms | 7 | 19 timeout |
