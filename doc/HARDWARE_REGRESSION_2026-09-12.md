<!-- Author: shpegun60 -->
<!-- SPDX-License-Identifier: MIT -->

# H7S regression of the September paranoid-audit fixes

This is the live-board follow-up to [the host/ARM audit](PARANOID_AUDIT_2026-09-11.md).
The measured tree starts at `464ccc50d519ab53e8a4ee751597a95e563e8f88` plus
the uncommitted audit fixes. It is not a claim about the unmodified commit.
No production header was changed during this hardware session.

Board: NUCLEO-H7S3L8, STM32H7S3 Rev Y, Cortex-M7 at 600 MHz, ST-Link
`002A001F3033510135393935`, V3J17M11, COM6. Programming uses STM32CubeProgrammer
2.21.0. Firmware builds use GNU Arm 14.3 and the same ignored Cube scaffold
as earlier hardware records. IRQ priorities, normal DMA, and cache maintenance
remain the production paths.

## Targeted faults: completed

[Raw trials](../src/adapters/tests/hardware/h7s/results_2026-09-12/results.jsonl),
[session receipt](../src/adapters/tests/hardware/h7s/results_2026-09-12/session.json),
[firmware, runner and command contract](../src/adapters/tests/hardware/h7s/README.md).

The three `-Os/-O2/-O3` images passed **132 trials / 1623 device assertions**.
Each image runs local boundary tests and three full fault/control rounds.
All use the actual UART/GPDMA and real HAL tick with I/D caches and UART FIFO
enabled; the UART runs at 9600 so the slow-frame deadline is exercised.

| Boundary | What physically happened / was checked | Result |
|---|---|---|
| Stale HAL RX state with DMAR off | The harness cleared the actual peripheral request bit. No HAL error was forged. | Exactly one restart and one ordered gap in all nine trials; a subsequent physical command was received. |
| Lost normal-DMA completion | RX DMA IRQ delivery and IDLE publication were masked, then 256 physical bytes exhausted the DMA. The firmware observed zero remaining with HAL still BUSY_RX. | Exactly one restart/gap in all nine trials; no lost-buffer bytes were delivered, DMA rearmed to 256. |
| Quiet receiver | No incoming bytes during 800 ms, longer than three watchdog check periods. | No restart, error or gap. |
| Frozen nonzero progress | After a full prefix chunk, one physical byte entered the next DMA chunk with IDLE suppressed. | One extension only; expiry at 650 ms after the full chunk in all nine trials, Pool RX capacity restored, no UART restart. |
| Real progress | A second physical byte advanced that unpublished counter, followed by the remaining tail. | Two legitimate extensions, correct complete packet, no stale-frame error. |
| Empty transport input | `on_rx({})` was explicitly called after the full physical chunk; the physical tail was delayed by 100 ms. | The 325 ms full-chunk window was unchanged; all 300 data bytes and CRC were accepted. |
| Multi-chunk RX | One continuous 306-byte private ADU spans the 256-byte UART chunk. | Correct packet and returned Pool block. |
| Count and offset bounds | On-M7 local TX/RX loopback with one-byte counts around 255, Bitwise/Table/NoCrc, two-byte BE/LE counts, and a volatile overflowing offset through both receiver entry points. | 265 assertions per image including final storage reclamation; invalid TX stays Building and reaches neither CRC nor transport. |

The boundary vectors in the last row execute on the MCU but are **local
Endpoint loopback**, not UART transmissions. The 306-byte ADUs are deliberate
private RTU formats, not standard Modbus-sized frames. The host constructs
their CRC independently and the MCU checks every payload byte.

The targeted verifier passed on Windows with `--local-images` and under WSL
for the portable receipt/source checks. Its nine negative mutation checks
also passed: damaged evidence cannot become green merely by recomputing the
outer results-file hash. These verifier checks do not access the board.

All targeted images fit the 64 KiB boot region: load images are 37,944 /
52,260 / 61,608 bytes for `-Os/-O2/-O3`. None links an allocator. `-O3` emits
three GCC warnings in ST's unused linked-list DMA HAL implementation; those
are not warnings in the audited C++ headers. The library uses normal DMA,
not that linked-list path. This is not a warning-free build of all vendor C.

## Full protocol matrix: completed

The [complete repeat receipt](../src/wire/tests/hardware/h7s/results_faults_2026-09-12_repeat/session.json)
binds **33 images / 93 COBS + 144 RTU suite records**. Independent verification
with `--local-images` passed, including exact negative counters, ELF/bin/log
identities and the firmware read-back. No download retry was needed in this
complete repeat. Its first 27 completed ELF files match the first attempt's
corresponding ELF bytes; no runtime implementation was swapped to obtain a pass.

- COBS: CRC16 Bitwise/Table with 253-byte maximum payload, NoCrc with 1024;
  each at 115200, 1M, 3M, 6M and 10M. Vectors, malformed frames, explicit
  gaps/BREAK, backpressure, Pool exhaustion and stress, with the extended
  10M/window-7 phase, all meet their acceptance checks.
- RTU: NoCrc and CRC8/16/32/64 Bitwise/Table at 115200 and 1M. Vectors,
  corruptions, backpressure, Pool exhaustion, CRC reference checks, stress
  and paced/extended phases all meet their acceptance checks. NoCrc's
  intentional acceptance of altered payload bytes is checked, not labelled
  corruption detection.

The first attempt is preserved in
[results_faults_2026-09-12](../src/wire/tests/hardware/h7s/results_faults_2026-09-12/session.json).
It completed 27 images, then ST-Link reported `failed to download Sector[0]`
while programming `rtu-crc32-table-115200`. That image's runtime suites never
started. The attempt remains **incomplete**, not green. The original firmware
was restored and read back successfully before another run was started.
The [complete failed build/flash log](../src/wire/tests/hardware/h7s/results_faults_2026-09-12/rtu-crc32-table-115200.log)
is retained byte-for-byte with the hash in the failed phase receipt.

Both PowerShell protocol runners now allow at most three attempts for that
specific pre-test download error, keeping every attempt in the flash log.
Success requires exit code zero **and** `Download verified successfully`.
Unknown errors and missing verification fail immediately. Runtime test failures
are never retried automatically. Twelve mocked runner cases prove these
boundaries without touching the board:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src/wire/tests/hardware/h7s/test_flash_retry.ps1
```

This handles an observed programmer failure; it does not establish its physical
USB/ST-Link cause or promise that programming cannot fail again. The complete
matrix was repeated from its first image in a new evidence directory;
no rows from the failed attempt were spliced into a passing session.

## Framing versus burst candidates: completed

[Raw record](../src/modbus/rtu/tests/hardware/h7s/results_framing_2026-09-12.jsonl),
[receipt](../src/modbus/rtu/tests/hardware/h7s/results_framing_2026-09-12.jsonl.session.json).
Eight flashed images, **23 records**: the default burst endpoint's 6M vectors
run timed out before HELLO, so it has an exit-code entry but no JSONL row.
That missing row is explicitly retained as a failure, not inferred successful.
The framed endpoint passed all vectors and **192/192 boundary trials**.

| Baud | Endpoint | single-write echoes | split-write echoes | two frames in one write | orphan half then a whole frame | smoke | vectors suite |
|---:|---|---:|---:|---:|---:|---|---|
| 1000000 | framing::None (burst candidate) | 12/12 | 0/12 | 3/12 | 12/12 | passed | passed |
| 1000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |
| 3000000 | framing::None (burst candidate) | 12/12 | 0/12 | 0/12 | 12/12 | passed | passed |
| 3000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |
| 6000000 | framing::None (burst candidate) | 5/12 | 0/12 | 1/12 | 5/12 | passed | FAILED no HELLO response |
| 6000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |
| 10000000 | framing::None (burst candidate) | 5/12 | 0/12 | 0/12 | 4/12 | passed | FAILED at vector 7 (31 data bytes), no echo |
| 10000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |

`framing::None` requires the caller to supply one complete ADU. An IDLE-ended
VCP burst does not guarantee that boundary; it is not a supported stream
reassembler. These failures are the control observation, not a regression
in the framed endpoint and not an argument for assuming a bad CRC will always
recognize a fragment. The length-prefixed harness is a private framing policy;
standard function-table interoperability is exercised separately with Qt.

The first framing image needed two programming attempts after another
pre-test `Sector[0]` download error. Both logs and the attempt count are
retained in its session. The successful download was verified, and original
firmware restoration/read-back succeeded after the matrix.

## Qt interoperability: completed

[Raw record](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-12.json),
[receipt](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-12.json.session.json).
All **eight runs / eight flashed images** passed the independent reference-model
verifier: 115200 and 1M, burst and framed endpoints, with the MCU in each role.
Qt 6.4.3 and this repository's PC client each run the 55-scenario script
against the board's server; the MCU client also runs 55 against Qt's server.
That is **660 scenario verdicts: 652 `ok`, eight expected `timeout`**.

Every expected timeout is script step 19, an unknown function for which the
peer has no length-table entry. Burst MCU servers answer it; framed MCU
servers and Qt's server cannot frame it. There are no unexpected timeouts,
data mismatches or transport errors. Qt's Diagnostics API limitation is still
judged against the actual correct wire response, as in the existing verifier.

Both PC clients run with **zero retries**. Qt's reference server uses the
previously validated **50 ms USB-aware RX fragment deadline**, with tracing
enabled and no injected host stall. This is not a claim about Qt's unmodified
native 2 ms deadline under arbitrary Windows scheduling. All eight images
programmed on the first attempt, and the initial firmware was restored and
read back after the final run.

## Reproduce the accepted evidence

The live commands require exclusive ownership of COM6/ST-Link and a **new**
output path. The verifiers below only inspect recorded evidence:

```powershell
python -B src/wire/tests/hardware/h7s/verify_fault_matrix.py src/wire/tests/hardware/h7s/results_faults_2026-09-12_repeat --local-images
python -B src/adapters/tests/hardware/h7s/verify.py src/adapters/tests/hardware/h7s/results_2026-09-12 --local-images
python -B src/modbus/rtu/tests/hardware/h7s/verify_framing.py src/modbus/rtu/tests/hardware/h7s/results_framing_2026-09-12.jsonl --check-doc doc/HARDWARE_REGRESSION_2026-09-12.md
python -B src/adapters/qt/tests/hardware/h7s/verify_qmodbus.py src/adapters/qt/tests/hardware/h7s/results_qmodbus_2026-09-12.json
```

Live runners: [full matrix](../src/wire/tests/hardware/h7s/run_fault_matrix.py),
[targeted audit](../src/adapters/tests/hardware/h7s/run.py),
[framing](../src/modbus/rtu/tests/hardware/h7s/run_framing.py),
and [Qt interoperability](../src/adapters/qt/tests/hardware/h7s/run_qmodbus.py).

## Firmware preservation and scope

Every hardware runner takes a fresh read-back of the current 64 KiB before
flashing, restores it in `finally`, verifies programming and reads the entire
region back to compare SHA-256. The initial firmware hash for this session is:

```text
a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456
```

Final local-file verification checked all five sessions' actual `before.bin`
and `after.bin` sizes and hashes, including the interrupted attempt. Every
pair is 65,536 bytes and matches the value above. Retained framing and Qt ELF
files were also re-hashed against their receipts, in addition to the full
matrix and targeted verifier's `--local-images` checks. The board is back
on its original image; no test runner still owns COM6.

Retained ELF/bin/map/disassembly, flash logs and before/after firmware copies
are in each receipt's ignored local session directory. Raw records and
receipts are durable repository artifacts. The targeted record includes
155 source identities: ordinary library inputs, exact pinned dependency
checkout bytes, and the local Cube inputs. Uncommitted audit inputs are
reported explicitly by the verifier; historical records are not rewritten.

These runs verify particular fault/recovery and protocol scenarios. They
do not prove all possible interrupt interleavings, repair permanently disabled
interrupts, make NoCrc detect corruption, or turn length-based framing into
t1.5/t3.5 framing. Configured UART baud is not a claim of continuous ST-Link
VCP throughput. No new CPU-percent or instruction-execution benchmark is
claimed by this follow-up.
