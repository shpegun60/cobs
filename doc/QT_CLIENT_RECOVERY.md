<!-- Author: shpegun60
SPDX-License-Identifier: MIT -->

# Qt client: bounded failure and event-order recovery

7 September 2026. Follow-up to the
[Qt/USB fragment diagnosis](QT_USB_TIMEOUT_DIAGNOSIS.md).

This follow-up fixes **production desktop code** in
[`RtuClient.h`](../src/adapters/qt/RtuClient.h) and
[`SerialAdapter.h`](../src/adapters/qt/SerialAdapter.h). It is separate from
the earlier Qt reference-server configuration fix. COBS, CRC, Modbus RTU,
STM32 UART and the MCU adapter are unchanged.

## Reproduced failures and fixes

The first regression extension failed **8 of 181 checks** on the preceding
implementation. After those fixes, an additional nested-error test failed
**1 of 198 checks**. Neither result was counted as a passing run.

| Case | Previous behavior | Fixed behavior |
|---|---|---|
| RX delivered before `bytesWritten` | `Sending` drained and discarded a valid response before entering `Waiting` | Write completion is processed first; a response received while TX still appears busy stays in the endpoint queue |
| Accepted write never drains | No timer ran until write completion; the queue could wait forever | The write phase has a deadline; failed output is cleared best-effort, the borrowed block is released, and the request completes once as `WriteError` |
| Repeated `Busy` results | Every poll consumed an attempt/retry, with no wait bound | Busy polls share one write deadline and do not consume attempts; expiry completes the waiting request without aborting an older transfer it did not send |
| Rebind/send from a cancellation callback | `unbind()` kept cancelling requests newly added by the callback | Cancellation snapshots the detached session's queue; new-session requests survive |
| Error emitted by `clear()` during error recovery | Nested errors recursively called `clear()` and repeated notifications/gaps | The outer recovery suppresses re-entry until output cleanup and block release finish |

Related boundaries are now covered too:

- A completed packet left in the endpoint before a request is discarded at
  the new attempt boundary, alongside buffered port bytes and partial RX.
- Response timeout immediately discards that request's incomplete response;
  it no longer leaves cleanup to the independent 50-ms stale-frame timer.
- If that cleanup synchronously reports a resource error and finishes the
  request, the timeout handler does not access the removed queue head.
- `INT_MAX` retries uses unsigned arithmetic for the initial attempt budget;
  `retries + 1` no longer has signed overflow.
- A missing `bytesWritten` notification is not by itself a failed write:
  the deadline polls the endpoint once and observes whether the port has
  actually drained. Observable completion starts the response window.
- A failed output clear cannot retain the endpoint's borrowed block. If the
  port itself remains busy, the next queued request also ends within its own
  write budget. Work succeeds again once the port becomes usable.

## Timeout and ownership contract

`set_response_timeout_ms(T)` keeps its default of 1000 ms and minimum of 1 ms.
The same interval now bounds **two successive phases**, using the existing
timer, not an additional timer or MCU field:

1. When the head request first attempts to use the port, its write deadline
   starts. Time spent receiving `Busy` and time spent draining its accepted
   write share that budget; neither a Busy poll nor a partial notification
   restarts it.
2. When write completion becomes observable, a non-broadcast request gets a
   fresh full `T` for its response. A broadcast completes without a response
   and preserves the configured turnaround deadline.
3. A response timeout can retry according to `set_retries()`. A write failure
   or write timeout does **not** automatically retry a possibly partial ADU.
   Completion stops the old timer before invoking application code.

Choose `T` long enough for the entire ADU at the selected baud rate, as well
as the peer's response budget. This is not a total queue-latency limit: a
request can wait behind earlier requests and their inter-frame/turnaround
gaps. Qt event-loop deadlines are serviced late if the event loop is blocked;
they are not a hard real-time watchdog.

`Response::attempts` counts accepted endpoint sends, including a write that
later stalls. A Busy/refused send is not an accepted attempt. `WriteError`
therefore may report zero attempts when the port never accepted this request.

`SerialAdapter::abort_outgoing()` is the explicit upper-layer write-abort
entry point. While bound, it performs best-effort output clear, releases the
endpoint's borrow, records `TransportError::Write` and invokes the service
handler. On an unbound adapter it does nothing. Qt holds a copy of the bytes,
so block release is safe even when clearing the port fails. Bytes already
sent on the line cannot be retracted.

Cancellation callbacks may rebind and queue requests. The client, endpoint,
port and callback captures must remain alive during calls; all operations
belong to the port's event-loop thread.

## Regression coverage

[`serial_adapter_test.cpp`](../src/adapters/qt/tests/serial_adapter_test/serial_adapter_test.cpp)
now executes **202 checks**, up from 164, using real endpoints and a real Qt
event loop with a controllable `QIODevice` port. Five consecutive Release
repeats and a separate Debug build with `_GLIBCXX_ASSERTIONS` passed. These
are **host fault-injection tests**, not claims that USB or the
physical board produced each injected ordering/error.

The suite also checks the self-contained public headers, the compile-time
rejection of burst RTU over arbitrary Qt serial chunks, and builds the real
PC interop client. The separate Qt journal/injection test passes 22 checks.
The 10 USB-deadline evidence tests and 3 historical-regression evidence tests
still pass; historical failing records still fail their ordinary verifier.

```powershell
& 'C:/Program Files/Git/bin/bash.exe' src/adapters/qt/tests/run.sh
python -B src/adapters/qt/tests/hardware/h7s/test_usb_deadline.py
python -B src/adapters/qt/tests/hardware/h7s/test_regression_evidence.py
python -B src/adapters/qt/tests/hardware/h7s/test_client_recovery.py
```

The Qt kit used here is 6.4.3 with MinGW GCC 11.2, warning-free. This local
kit has no ASan/UBSan runtime, and the installed WSL environment has no Qt
SerialPort/SerialBus kit. No Qt sanitizer result is claimed.

## Live interop follow-up

The updated desktop client is exercised with the existing 55-step reference
script on NUCLEO-H7S3L8, COM6, ST-Link
`002A001F3033510135393935`: 115200/1M, burst/framed board endpoints, board
server against both PC clients and board client against Qt's server.
This uses the runner's 50-ms Qt-server RX setting, zero retries and the
unchanged 1000-ms response timeout. It does not inject physical USB faults.

Both new full rounds passed the ordinary verifier, exit 0:

- [Round 1](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_recovery1.json),
  [restore receipt](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_recovery1.json.session.json).
- [Round 2](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_recovery2.json),
  [restore receipt](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_recovery2.json.session.json).

Each round contains 8 role/baud/framer runs and 660 reference-model step
verdicts: **16 runs, 1,320 verdicts in total, with no unexpected results**.
These are not 1,320 successful responses: the script includes intentional
silence. Unknown function step 19 still times out against a framed server;
no new exception was added to the verifier and no failed case was retried
into a pass.

Both sessions restored and read back the original 64-KiB boot image,
SHA-256 `a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456`.
All 16 MCU ELF and binary hashes match their corresponding preceding control
images. Among the recorded build inputs, only the two desktop headers
changed. The five tests in
[`test_client_recovery.py`](../src/adapters/qt/tests/hardware/h7s/test_client_recovery.py)
check the ordinary verdicts, exact coverage and indices, both restoration
receipts, unchanged MCU images, desktop-only source changes and board-client
UART/CRC error counters.

The records identify the measured source bytes by SHA-256. Until those two
header versions are committed, the verifier explicitly reports them as
`CAVEAT ... measured sources are not committed yet` and checks the working
tree; committing the same measured bytes removes that provenance caveat.
It does not change any recorded verdict or firmware hash.

## Limits that are not removed by these fixes

RTU has no transaction ID. Clearing input rejects **already buffered** late
responses; it cannot distinguish a response that arrives later with the same
address/function as a new request. Applications needing stronger correlation
must supply it at their application/protocol level, not infer it from this
reset. The raw client also leaves function-data semantics to the caller.

The serial adapter's 50-ms fragment allowance remains finite. A USB/OS stall
longer than that, a failed cable, a silent peer, or a stopped event loop can
still prevent communication. The change makes the tested error paths release
their state and complete predictably once the event loop services them; it
does not promise that hardware can never fail. MCU burst RTU still requires
a complete candidate per delivery; stream transports still need a framer.
