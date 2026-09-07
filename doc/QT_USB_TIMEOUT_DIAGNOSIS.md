<!-- Author: shpegun60
SPDX-License-Identifier: MIT -->

# Qt/VCP receive-fragment timeouts: diagnosis and fix

7 September 2026, follow-up to the [live regression](HARDWARE_REGRESSION_2026-09-07.md).

This is the historical reference-server diagnosis. The later
[client recovery follow-up](QT_CLIENT_RECOVERY.md) fixes separate production
desktop error paths; the unchanged-header statement below applies to this
experiment, not to that later change.

The timeout mechanism is reproduced on the H7S: **Qt's server discards a
valid request fragment when Windows delivers the rest after its default
2-ms receive-fragment deadline**. Configuring that reference server for USB/OS
delivery fixes the reproduced case. Three subsequent full control rounds and
the normal-default invocation passed, with zero unexpected verdicts.

The correction is in the **PC test/reference server configuration**, not in
COBS, CRC, RTU, UART or the production adapters. The STM32 ELF and binary
hashes are identical to the earlier run, for every matching role/baud/framer.
The stack's own `adapters::qt::SerialAdapter` already uses a separate 50-ms
fragment-silence deadline; its behavior needed no change.

## What is established, and what is not

The installed Qt 6.4.3 implementation was inspected directly:
`QtSerialBus/6.4.3/QtSerialBus/private/qmodbusrtuserialserver_p.h`,
`setupSerialPort()`, and `qmodbusdevice_p.h`, `calculateInterFrameDelay()`.
On each `readyRead`, Qt clears a nonempty partial-request buffer when the
elapsed time since its previous delivery exceeds the configured interval.
At 115200/1M the native interval is 2 ms. The bytes may be a complete,
valid UART transmission, but the two process-level deliveries can be
separated by USB buffering or scheduling.

The previous diagnostic run already recorded that discard for step 1. This
follow-up also reproduces the **step-0 signature** with a controlled host
stall and the unchanged board firmware:

1. Qt reads the first five bytes, `0a03000000`, of request
   `0a030000000ac4b6` (read ten holding registers).
2. The diagnostic hook pauses PC-side processing for 12,349 us. The board
   is not paused, and sends the same single DMA span as before.
3. Qt's next delivery logs a fragment discard (`expected: 2`, `max: 12` ms).
   The remainder cannot form the request. The board's step 0 times out.

Board UART overrun/read/write/restart and RTU CRC/gap counters are zero in
that run. The public reference-model verifier rejects the record and the
runner returns exit 1 **after restoring the original firmware**.

This is a controlled reproduction of the failure mechanism, not a claim
that the first historical, untraced timeout has been retrospectively proved
to have that exact cause. Both original failed records remain unchanged and
still fail their ordinary verifier. The new evidence removes the unexplained
behavior from the reproduced case; it does not rewrite history.

## The change and its boundary

The Python hardware runner now configures its Qt reference server with:

```cpp
QModbusRtuSerialServer server;
server.setInterFrameDelay(50'000); // microseconds, before connectDevice()
```

This aligns that server's **RX fragment retention** with the existing
desktop adapter's 50-ms delivery budget. It is not the request timeout and
does not delay a complete valid response by 50 ms: Qt processes a complete
length/CRC-checked request immediately. Only a still-incomplete buffered
fragment gets a larger delivery window.

Unchanged: wire bytes, function-length rules, CRC validation, 1000-ms response
timeout, zero retransmissions, the board's 2-ms inter-request pacing, and the
2500-ms script start delay. The 50-ms setting is a USB/OS transport allowance,
not a claim of strict physical t1.5/t3.5 timing. The native Qt behavior remains
selectable with `--server-inter-frame-us -1` for comparison.

If using this repository's `SerialAdapter`/`RtuClient`, there is no new API
setting to apply: its 50-ms deadline already existed. Do not add a 50-ms sleep
to the MCU loop, UART ISR, protocol endpoint or normal TX path.

## Recorded A/B and controls

NUCLEO-H7S3L8 / ST-Link `002A001F3033510135393935`, COM6, QtSerialBus 6.4.3,
ARM GNU 14.3.1 `-Os`, no LTO. Each suite runs the same 55-step model script.

| Board client case | Qt RX deadline | Actual injected PC stall | Step 0 | Verdict |
|---|---:|---:|---|---|
| Native Qt, 115200 / framed | 2000 us | 12349 us | timeout | rejected, exit 1 |
| USB-configured Qt, 115200 / burst | 50000 us | 19961 us | ok, 23937 us RTT | passed |
| USB-configured Qt, 115200 / framed | 50000 us | 12451 us | ok, 15829 us RTT | passed |

The requested stall is 10 ms; the **measured** pauses are reported above,
not relabelled as exactly 10 ms. The stall triggers once, only after Qt has
actually received an incomplete FC03 request. A run in which it did not
activate is rejected as an invalid injection test, not counted as a pass.

Raw records (each has its own adjacent `.session.json` receipt):

- [Native Qt + injected stall](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_native_stall.json):
  two role runs, deliberately rejected with the reproduced step-0 timeout.
- [USB deadline + injected stall](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_usb_stall.json):
  four role runs, both board framing modes at 115200, passed.
- Full 115200/1M, both roles and both framing modes, no injected stalls:
  [round 1](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_usb_repeat1.json),
  [round 2](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_usb_repeat2.json),
  [round 3](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_usb_repeat3.json).
  Eight role runs per round; all three passed, 1,980 individual step verdicts.
- [Normal-default invocation](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_usb_defaults.json):
  115200/framed, both roles, no explicit deadline/trace options; passed with
  the new 50-ms RX default and the in-memory journal enabled.

In total: six sessions, 32 role images; **2,475 checked step verdicts in the
passing runs**, plus 165 in the intentionally failing native control. These
are reference-model verdicts, not 2,475 successful responses: silence for
broadcasts/foreign units and the documented step-19 unknown-function timeout
are expected. No new exception was added to the verifier. Every passing
board-client script remains 54 `ok` + the single expected step-19 timeout.

Every session restored and read back the same original 64-KiB boot image:
`a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456`.
Per-image ELF hashes, source hashes, raw records and restore receipts are
retained. Full ELFs, flash logs and backups remain in the ignored session
directories; no production firmware or external flash was changed.

## A failed test can no longer look like a successful runner

`run_qmodbus.py` previously completed and returned success after recording
the scenarios, even if the separate verifier would reject their statuses.
It now always restores the board first, runs the verifier, and returns its
exit code. The injected native run demonstrated exit 1; the fixed/control
runs demonstrated exit 0. A failed case is not retried into a green record.

The runner also enables an in-memory Qt journal by default. It records
timestamped RX, parse, discard and response decisions without synchronous
disk/console logging inside `readyRead`. `--no-server-trace` is an explicit
opt-out for untraced timing. It is bounded to 4096 entries per Qt-server
process; overflow invalidates the evidence instead of silently losing lines.
The diagnostic hook and journal live only in
[`ServerTrace.h`](../src/adapters/qt/tests/qmodbus_bench/ServerTrace.h), not in
any production header.

The verifier checks the actual Qt deadline against the selected setting,
the trace enable/overflow state, monotonic timestamps, and actual activation
and duration of any requested stall. It also checks complete role/baud/framer
coverage, the result-file hash, receipt image identities and the board's
reported baud/framer. Historical records remain supported without inventing
trace fields they never recorded.

## Regression tests and reproduction

The Qt host suite passes **164 adapter/client checks** (32 added request-side
checks), plus **22 journal/injection checks** and the existing compile-fail
boundary. The new request-side test replays both observed FC03/FC04 requests
at every byte cut with 10-ms gaps, verifies exactly one intact packet,
rejects a corrupted CRC, and verifies orphan expiry/recovery at the existing
50-ms deadline. No COM port is used for those unit tests.

The ten tests in [test_usb_deadline.py](../src/adapters/qt/tests/hardware/h7s/test_usb_deadline.py)
check all six live records, identical pre/post-mitigation MCU images,
restoration, default options and negative metadata mutations. The earlier
three evidence tests still require the original two failed records to fail.

From the repository root; use a **new output path** and one hardware session
at a time. The native injection command is expected to return exit 1:

```powershell
# Reproduce Qt's native fragment discard on the board; expected verifier failure.
python -B src/adapters/qt/tests/hardware/h7s/run_qmodbus.py --port COM6 --serial 002A001F3033510135393935 --bauds 115200 --framers 1 --server-inter-frame-us -1 --stall-first-read-fragment-ms 10 --output src/adapters/qt/tests/hardware/h7s/results_NATIVE_NEW.json

# Same injected host delay, USB-aware RX setting; expected pass.
python -B src/adapters/qt/tests/hardware/h7s/run_qmodbus.py --port COM6 --serial 002A001F3033510135393935 --bauds 115200 --framers 0,1 --stall-first-read-fragment-ms 10 --output src/adapters/qt/tests/hardware/h7s/results_USB_NEW.json

# Ordinary full run: 50-ms receive-fragment deadline, journal on, no injected stall.
python -B src/adapters/qt/tests/hardware/h7s/run_qmodbus.py --port COM6 --serial 002A001F3033510135393935 --output src/adapters/qt/tests/hardware/h7s/results_FULL_NEW.json

# Offline checks only:
python -B src/adapters/qt/tests/hardware/h7s/test_usb_deadline.py
python -B src/adapters/qt/tests/hardware/h7s/test_regression_evidence.py
```

Run the C++ host suite with `sh src/adapters/qt/tests/run.sh` from Git Bash.

## Remaining limits

This closes the reproduced Qt/USB receive-fragment problem within the
documented delivery budget. It cannot guarantee a USB bridge or desktop OS
will never stall beyond 50 ms, or that a cable/peer cannot fail. Such events
still require normal timeout/error handling; a new failing hardware run now
has a nonzero exit and, by default, diagnostic evidence.

The default **burst RTU endpoint** still requires a complete candidate per
delivery. This PC-server configuration change does not turn it into a stream
framer or erase the high-baud VCP boundary observations. Use the existing
framing policy when the transport delivers arbitrary fragments. No strict
physical-timing or continuous-10M-throughput claim is added by these tests.
