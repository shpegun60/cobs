<!-- Author: shpegun60
SPDX-License-Identifier: MIT -->

# Live hardware regression, 7 September 2026

The COBS/RTU fault matrix passed: **33 images, 93 COBS and 144 RTU suite
records**. The framed RTU endpoint also passed all 240 single/split/glued/
orphan trials through 10M, plus its smoke and vector suites. The final full
Qt interoperability control run passed its existing acceptance criteria.

This is **not an unconditional all-green result**. The default burst RTU
endpoint still loses candidates that the ST-Link bridge splits or glues.
Two Qt runs also contained an unexpected timeout each; both failed records
are retained, and the ordinary Qt verifier still rejects them. The passing
repeats do not erase those observations. No production library was changed.

## Target, source and firmware safety

- NUCLEO-H7S3L8, Cortex-M7 at 600 MHz, USART3/GPDMA, D-cache enabled.
- ST-Link `002A001F3033510135393935`, firmware V3J17M11, VCP COM6.
- ARM GNU 14.3.1; protocol images `-Os`, no LTO. QtSerialBus 6.4.3 on
  Windows, its MinGW GCC 11.2 kit; STM32CubeProgrammer 2.21.0.
- Production source and pre-existing harness baseline:
  `8ebd5314c94b692b3284cc27ee90d95cf22d2aa9`. Each record carries the measured
  source hashes. The new matrix runner and Qt diagnostic collection were
  uncommitted during measurement; provenance resolves their hashes to the
  later commit containing these exact bytes, not to arbitrary current code.
- Six sessions, 69 flashed test images in total: 33 fault-matrix, 10 RTU
  framing, and 26 Qt role images. Before every session the original 64 KiB
  internal boot-flash region was backed up; afterwards it was restored,
  verified and independently read back. All six backups have the same SHA-256:
  `a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456`.
  Only `0x08000000..0x0800FFFF` was written; no external flash or option bytes.

Raw results and receipts are versioned. Exact ELFs, maps, complete flash/log
files and before/after binaries remain in the gitignored session directories
named by the receipts. `--local-images` re-reads those retained files on the
measuring machine; an ordinary checkout can recheck records and source
provenance but cannot independently re-read an absent physical backup.

## COBS and RTU: full existing fault suites

The new [wrapper](../src/wire/tests/hardware/h7s/run_fault_matrix.py) reuses
the existing COBS and RTU `run_matrix.ps1` runners, with one image per phase.
It adds a fixed coverage plan, retained-image identities and an outer
backup/restore/read-back boundary; it does not replace the protocol oracles.

| Configuration | Baud rates | Images | Suite records | Verdict |
|---|---|---:|---:|---|
| COBS CRC16 Bitwise, 253 payload / H1 | 115200, 1M, 3M, 6M, 10M | 5 | 31 | passed |
| COBS CRC16 Table, 253 payload / H1 | 115200, 1M, 3M, 6M, 10M | 5 | 31 | passed |
| COBS NoCrc, 1024 payload / H2 | 115200, 1M, 3M, 6M, 10M | 5 | 31 | passed |
| RTU NoCrc and CRC8/16/32/64 Bitwise/Table, MaxAdu 256 | 115200, 1M | 18 | 144 | passed |

COBS uses `Uart<128,8>`, RTU `Uart<256,4>`; each endpoint uses
`wire::Pool<8,2>`. This regression is not a like-for-like CPU comparison.
The exact [33 raw JSONL files and receipt](../src/wire/tests/hardware/h7s/results_fault_matrix_2026-09-07/)
are checked by [verify_fault_matrix.py](../src/wire/tests/hardware/h7s/verify_fault_matrix.py).

What was actually injected and checked:

- **COBS malformed/truncated/length-mismatch frames:** seven injected inputs
  per configuration, including one harmless bare delimiter. Exact per-phase
  totals: one malformed code, four length mismatches, six lost frames and
  successful sentinel echoes after every input. CRC16 configurations also
  detect one altered checksum; the NoCrc/H2 configuration instead exercises
  one representable oversize length. NoCrc is not credited with corruption
  detection. The H1/CRC16 format cannot represent a declared body above its
  255-byte limit, so that particular oversize-header injection is in H2.
- **RTU corruption:** four altered candidates rejected and four following
  recovery echoes checked for every CRC-bearing policy/baud. NoCrc instead
  echoes all four altered candidates verbatim, with zero CRC errors. Across
  the matrix: 64 CRC rejections and eight deliberately accepted NoCrc mutations.
- **TX backpressure/exhaustion:** exactly one `Busy` and one TX-pool refusal
  in each of the 33 firmware self-tests, followed by usable transport/storage.
- **RX-pool exhaustion:** COBS retains the first eight of 32 packets and
  records 24 allocation failures; RTU retains the first eight of 16 and
  records eight failures. FIFO order and the next recovery packet are checked.
  At the final STATS observation, only that control request owns one RX block;
  no TX block remains held.
- **Physical UART loss:** 15 COBS gap suites, one at every baud/configuration,
  stall processing and flood real DMA reception. Each observes a nonzero
  UART overrun, COBS loss/resynchronization and a successful recovery echo.
  This is not merely a simulated endpoint allocation failure.
- **Healthy traffic:** vector suites, 10-second COBS stress at every baud plus
  30 seconds at 10M; five-second RTU stress and paced traffic for every
  policy/baud plus 15-second windows at 1M. All expected echoes and counters
  pass. Configuring 10M does not prove continuous full-line traffic through VCP.
- **CRC calculation:** 18 on-board benchmark suites, eight lengths and nine
  samples per length (1,296 samples), checked against independent host CRC
  oracles. Image inspection confirms no lookup table in Bitwise/NoCrc images
  and the expected table size in Table images. This run is not a new
  all-ARM/all-optimization disassembly matrix.

## RTU framing and the default burst boundary

[Raw framing record](../src/modbus/rtu/tests/hardware/h7s/results_framing_2026-09-07.jsonl)
and its adjacent session receipt contain 30 suite observations / 10 images.
The following table is generated by `verify_framing.py`, not inferred from
the fact that the runner completed.

### RTU frame boundaries on the H7S ST-Link bridge: default burst framing versus the framing policy

| Baud | Endpoint | single-write echoes | split-write echoes | two frames in one write | orphan half then a whole frame | smoke | vectors suite |
|---:|---|---:|---:|---:|---:|---|---|
| 115200 | framing::None (burst candidate) | 12/12 | 6/12 | 3/12 | 12/12 | passed | passed |
| 115200 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |
| 1000000 | framing::None (burst candidate) | 12/12 | 0/12 | 3/12 | 12/12 | passed | passed |
| 1000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |
| 3000000 | framing::None (burst candidate) | 12/12 | 0/12 | 0/12 | 12/12 | passed | passed |
| 3000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |
| 6000000 | framing::None (burst candidate) | 4/12 | 0/12 | 0/12 | 3/12 | FAILED response timeout: received 0/21 bytes | FAILED at vector 4 (2 data bytes), no echo |
| 6000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |
| 10000000 | framing::None (burst candidate) | unavailable: response timeout: received 0/21 bytes | unavailable: response timeout: received 0/21 bytes | unavailable: response timeout: received 0/21 bytes | unavailable: response timeout: received 0/21 bytes | FAILED response timeout: received 0/20 bytes | FAILED at vector 12 (32 data bytes), no echo |
| 10000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |

The burst endpoint requires a complete candidate per delivery. Partial or
concatenated candidates are outside that boundary; it is not a stream framer.
At 10M the burst framing suite lost a control response before returning a
trial summary. Its cells are therefore **unavailable**, not `0/12` or `12/12`.
The verifier previously assumed even the burst suite must return a summary;
it now preserves such a failed observation and checks its nonzero child exit
code. A failed framed suite is still rejected. Four offline regression tests
pin that distinction; all six older framing records still verify.

## Qt interoperability: failures and repeats, all retained

All runs use the same 55-step reference script, 1,000 ms response timeout,
**zero retries**, 2,500 ms board-client start delay and a 10-second Qt server
window. Full sessions cover both board roles, both framing modes and 115200/1M.

| Record | Scope | Unexpected result | Ordinary verifier |
|---|---|---|---|
| [Initial run](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07.json) | full, 8 role runs | board client, 115200/framed: step 0 timeout; 53/55 ok | FAIL, exit 1 |
| [Repeat 1](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_repeat1.json) | 115200/framed, both roles | none; board step 0 answered in 5,490 us | PASS, exit 0 |
| [Repeat 2, diagnostic](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_repeat2_full.json) | full, Qt debug output forced to stderr | board client, 115200/burst: step 1 timeout; 53/55 ok | FAIL, exit 1 |
| [Repeat 3, control](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_repeat3_control.json) | full, debug logging disabled | none | PASS, exit 0 |

The known step-19 timeout remains separate: an unknown function has no entry
in a length-driven server's framing table. Thus 54/55 `ok` plus this one
expected timeout is a passing script against Qt's server or the framed board
server. The burst board server answers it with exception 01 and scores 55/55.
Qt's client still reports Diagnostics as `InvalidResponseError` while its raw
response is correct; acceptance is based on the raw reference-model response,
as before. None of these expectations was relaxed for the new timeouts.

The [retained diagnostic excerpt](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_repeat2_full.trace.json)
binds the first 13 lines to the full local log's SHA-256 and the result file.
For repeat 2, Qt receives the step-1 request as `0a`, then
`040000000a7176`. Between them it logs that it is dropping the older fragment
because its measured delay exceeds the 3.5-character threshold (`expected: 2`,
`max: 6`, in milliseconds). It then cannot frame the remaining bytes. The
board reports no UART overrun/error/restart or CRC error in that run.

This establishes a **Qt-side fragment discard for that observed timeout**.
It does not measure the electrical inter-byte timing, distinguish USB/VCP
delivery from Windows scheduling, or establish that verbose logging had no
effect. The first run had no wire trace, so its step-0 timeout remains
unexplained; attributing it to the same mechanism would be an inference.
Repeat 1 requested Qt categories but did not force stderr, so it captured no
low-level trace. The final untraced control passed without changing firmware,
timeouts, framing rules or retry count. No error-rate estimate is made from
these differently instrumented runs.

### Final control: board server, two PC clients

| Baud | Board server | QModbusRtuSerialClient ok | RtuClient ok | Qt burst median | RtuClient burst median | Qt total | RtuClient total | served by the board |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 115200 | burst | 55/55 | 55/55 | 5.36 ms | 5.26 ms | 1334.09 ms | 1421.46 ms | 106 |
| 1000000 | burst | 55/55 | 55/55 | 2.71 ms | 2.88 ms | 1158.35 ms | 1256.30 ms | 106 |
| 115200 | framing policy | 54/55 | 54/55 | 5.45 ms | 5.46 ms | 2336.55 ms | 2432.28 ms | 104 |
| 1000000 | framing policy | 54/55 | 54/55 | 2.86 ms | 2.78 ms | 2163.74 ms | 2257.46 ms | 104 |

### Final control: board client against QModbusRtuSerialServer

| Baud | Board client | steps ok | burst median round trip | writes Qt recorded | steps not ok |
|---:|---|---:|---:|---:|---|
| 115200 | burst | 54/55 | 3.15 ms | 7 | 19 timeout |
| 1000000 | burst | 54/55 | 1.18 ms | 7 | 19 timeout |
| 115200 | framing policy | 54/55 | 3.26 ms | 7 | 19 timeout |
| 1000000 | framing policy | 54/55 | 0.93 ms | 7 | 19 timeout |

These are host-observed round trips, not endpoint CPU cost. The existing
[matched performance comparison](PROTOCOL_COMPARISON.md) remains the source
for that different question.

## Reproduce and verify

The output path of each hardware runner must be new. These commands **flash
the selected board**; do not run them concurrently with another serial/debug
session. The fault wrapper validates the backup before its first write and
restores it in `finally`, including after a failed child runner.

```powershell
python -B src/wire/tests/hardware/h7s/run_fault_matrix.py --port COM6 --serial 002A001F3033510135393935 --output src/wire/tests/hardware/h7s/results_fault_matrix_NEW
python -B src/modbus/rtu/tests/hardware/h7s/run_framing.py --port COM6 --serial 002A001F3033510135393935 --bauds 115200,1000000,3000000,6000000,10000000 --output src/modbus/rtu/tests/hardware/h7s/results_framing_NEW.jsonl
python -B src/adapters/qt/tests/hardware/h7s/run_qmodbus.py --port COM6 --serial 002A001F3033510135393935 --output src/adapters/qt/tests/hardware/h7s/results_qmodbus_NEW.json
```

Offline checks of this recorded session (no flashing/COM access):

```powershell
python -B src/wire/tests/hardware/h7s/verify_fault_matrix.py src/wire/tests/hardware/h7s/results_fault_matrix_2026-09-07 --local-images
python -B src/modbus/rtu/tests/hardware/h7s/verify_framing.py src/modbus/rtu/tests/hardware/h7s/results_framing_2026-09-07.jsonl --check-doc doc/HARDWARE_REGRESSION_2026-09-07.md
python -B src/adapters/qt/tests/hardware/h7s/verify_qmodbus.py src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-07_repeat3_control.json --check-doc doc/HARDWARE_REGRESSION_2026-09-07.md
python -B src/wire/tests/hardware/h7s/test_fault_matrix.py
python -B src/modbus/rtu/tests/hardware/h7s/test_framing_evidence.py
python -B src/adapters/qt/tests/hardware/h7s/test_regression_evidence.py
```

The three new offline test files have 16, four and three tests respectively.
They reject missing coverage, changed bytes/policy/baud, incorrect CRC/NoCrc
counts, absent physical overrun, leaked ownership and incorrect restore/CPU
claims; preserve failed burst observations; and explicitly require the two
failed Qt records to keep failing their ordinary verifier. Passing that last
regression suite means **the evidence is represented honestly**, not that
the two failed hardware exchanges passed.

## What this hardware repeat does not claim

Compile-fail contracts, all host fuzz/randomized malformed-input cases,
sanitizer checks, fabricated JSON corruption, fake-HAL abort/error races and
QIODevice failure injection are host tests. They were not somehow replayed
physically by this matrix. Its board coverage is the existing suites listed
above, not every possible fault, all UART instances, all ARM cores, RS-485
electrical timing or continuous 10M throughput. RTU too-short/oversize and
every function-table branch remain covered by the separate host suites,
not by a new claim of exhaustive on-wire injections here.

No protocol, CRC, storage, UART or adapter production behavior was changed.
The changes are repeatable orchestration, diagnostic collection, evidence
verification/tests and documentation. Qt/VCP intermittency is documented as
an integration limitation requiring timeout handling, not silently declared
fixed by the successful control repeat.
