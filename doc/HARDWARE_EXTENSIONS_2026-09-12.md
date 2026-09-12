<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Extension-contract fixes: full live H7S regression

Baseline: `3d177cf3b47c22715803c9bec26f621d69d60cb7`, plus the measured
source bytes recorded in each receipt. The two production changes are the
compile-time storage/framer admission fixes in
[EXTENSION_CONTRACT_AUDIT.md](EXTENSION_CONTRACT_AUDIT.md). No protocol wire
format, UART implementation, cache placement or ownership rule changes here.

**Completed: eight fresh-backup sessions, 81 flashed image configurations.**
All supported-mode acceptance gates passed. Deliberately out-of-contract RTU
burst controls, the expected global-new abort and three pre-test ST-Link
download failures remain visible below. No unanticipated runtime failure was
recorded in the supported configurations.

NUCLEO-H7S3L8, STM32H7S3 Cortex-M7 at 600 MHz; I/D caches enabled, real
STM32 HAL/UART/DMA, COM6 through ST-Link serial `002A001F3033510135393935`.
GNU Arm 14.3.1 and CubeProgrammer 2.21.0. Sessions are sequential: only one
runner owns the board. These are the maintained correctness/fault suites;
historical performance experiments and obsolete pre-fix reproducers are
not silently relabelled as new passing measurements.

## Live matrix

| Suite | Images | Evidence and result |
|---|---:|---|
| COBS + burst RTU | 33 | [Receipt](../src/wire/tests/hardware/h7s/results_extensions_2026-09-12/session.json): PASS, 93 COBS + 144 RTU suite records |
| Real FreeRTOS parity | 14 | [Receipt](../src/adapters/tests/hardware/h7s/parity/results_extensions_2026-09-12/session.json): PASS, 704 exchanges, 560 exact echoes, 2,100 MCU-local checks |
| TCP MBAP over UART | 6 | [Receipt](../src/modbus/tcp/tests/hardware/h7s/results_extensions_2026-09-12/session.json): PASS, 1,974 exact UART exchanges, 38 fail-closed trials, MCU core/OOM/RTU cases and exhaustive lengths |
| UART/DMA faults, Os/O2/O3 | 3 | [Receipt](../src/adapters/tests/hardware/h7s/results_extensions_2026-09-12/session.json): PASS, 186 live trials and 2,037 MCU assertions |
| Heap/Pool and software/peripheral CRC | 8 | [Receipt](../src/wire/tests/hardware/h7s/heap_crc/results_extensions_2026-09-12/session.json): PASS, 96 UART windows and 178,312 exact echoes, CRC/core/lifecycle oracles |
| Real Heap OOM/recovery | 1 | [Receipt](../src/wire/tests/hardware/h7s/heap_crc/recovery/results_extensions_2026-09-12/session.json): PASS, 12 refusal/recovery cases, 42 exact wire frames, expected global-new abort control |
| RTU framing and burst controls through 10M | 8 | [Receipt](../src/modbus/rtu/tests/hardware/h7s/results_framing_extensions_2026-09-12.jsonl.session.json): PASS framed acceptance, 192/192 exact boundary trials; burst control failures retained |
| QtSerialBus interoperability | 8 | [Receipt](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_extensions_2026-09-12.json.session.json): PASS, 660 reference-model verdicts, 652 ok and eight expected unknown-function timeouts |

The 33-image matrix uses COBS CRC16 Bitwise/Table with 253-byte payloads and
NoCrc with 1024-byte payloads at 115200/1M/3M/6M/10M. RTU runs all nine
NoCrc/CRC8/16/32/64 Bitwise/Table policies at 115200 and 1M. Coverage includes
negative frames, explicit gaps/BREAK, backpressure, pool exhaustion, ownership
recovery, independent wire/CRC oracles and stress. NoCrc's acceptance of
altered payload is intentional and checked, not called corruption detection.

## RTU framing versus deliberately out-of-contract burst input

All framed smoke/vector suites and all 192 single/split/glued/orphan trials
passed. Burst mode requires one complete externally delimited ADU; an IDLE
chunk from the ST-Link bridge does not satisfy that promise. Its split/glued
losses, 6M smoke/vector failures and 10M vector failure remain observations,
not passing runtime trials. There are 23 JSONL rows and 24 recorded suite
invocations: 6M burst smoke failed before HELLO, so only its exit/log exists.
The portable [raw record](../src/modbus/rtu/tests/hardware/h7s/results_framing_extensions_2026-09-12.jsonl)
and the three failed control logs under
[`results_extensions_2026-09-12`](../src/modbus/rtu/tests/hardware/h7s/results_extensions_2026-09-12)
retain those failures. No runtime retry was used.

| Baud | Endpoint | single-write echoes | split-write echoes | two frames in one write | orphan half then a whole frame | smoke | vectors suite |
|---:|---|---:|---:|---:|---:|---|---|
| 1000000 | framing::None (burst candidate) | 12/12 | 0/12 | 3/12 | 12/12 | passed | passed |
| 1000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |
| 3000000 | framing::None (burst candidate) | 12/12 | 0/12 | 0/12 | 11/12 | passed | passed |
| 3000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |
| 6000000 | framing::None (burst candidate) | 5/12 | 0/12 | 0/12 | 5/12 | FAILED no HELLO response | FAILED response timeout: received 0/21 bytes |
| 6000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |
| 10000000 | framing::None (burst candidate) | 5/12 | 0/12 | 0/12 | 5/12 | passed | FAILED at vector 8 (31 data bytes), no echo |
| 10000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |

## New tests actually built for the MCU

The shared [extension checks](../src/wire/tests/extension_checks.h) accept a
Memory parameter. Host execution uses Heap; the static-only FreeRTOS firmware
uses Pool and rejects linked allocator symbols. Its 36 additional runtime
checks cover const-reference storage through COBS/RTU/TCP and both legal
const-reference and overloaded framers through stream RX, complete ADU RX
and TX. Rejected throwing/deleted/wrong-result types remain compile-time
checks, not claims that invalid code executed on the board.

FreeRTOS telemetry schema 3 requires exactly 150 local checks per image
(114 prior checks plus 36 extension checks). The verifier continues to accept
historical schema 1/2 receipts with their own exact counts. It rejects a
schema 3 image that reports the old 114 count or old firmware version.

[length_checks.h](../src/wire/tests/length_checks.h) is one body shared by
the host and TCP firmware: 786,426 TCP headers plus 262,144 private RTU count
declarations, 2,560,991 checks per execution. Every 16-bit MBAP Length and
nonzero Protocol ID is covered across six CRC/data limits; every BE16 RTU
count across four limits. Rejection-before-allocation, exact allocation
requests and exact OOM remainder skipping are checked. Skipped spans have
real 1024-byte backing storage and are delivered in bounded chunks. These
are MCU-local declaration tests, not million-frame UART traffic or checksum
arithmetic tests. TCP schema 3 requires the exact count and zero failures;
schema 1/2 records retain their original checks.

Across the six TCP images, the original suites total 24,850 core checks,
36 real OOM checks and 318 local RTU data checks, all passed. In addition,
2,560,991 length-domain checks passed **in each image**. These repeated executions are not six different
length domains. Every image programmed on its first attempt; original boot
flash was restored and read back. UART carries MBAP bytes here, with no
Ethernet, socket or TCP/IP stack running.

The eight Heap/Pool images cover COBS and framed RTU with NoCrc, CRC16
Bitwise, Table and the stateful STM32 peripheral calculator. Each peripheral
image passes 8,200 independent checksum vectors; every image completes 648
endpoint samples and all twelve Pool/Heap UART windows. Timing samples are
retained in this new receipt, not substituted into historical performance
tables. This tests real malloc-backed Heap, not a simulated allocator.

The separate OOM session fills the real allocator arena, checks failed TX
construction, failed growth of the same Message, repeated RX refusals while
a Packet remains held, and successful recovery after freeing pressure.
The final direct global `new(std::nothrow)` control intentionally reaches
the original nano-runtime `abort -> _exit`; it is **not** a failure of
`wire::Heap`, which uses malloc/free and passes all refusal/recovery cases.
No global allocation override was added. Both sessions restored their fresh
boot backups and independently verified the read-back.

## Programming observations and restoration

The first COBS Table download at 115200 and at 1M hit ST-Link's previously
observed `failed to download Sector[0]`, before any runtime test started.
Both succeeded with verified download of the same ELF on attempt two. The
complete logs, including the failures, are retained byte-for-byte:

- [115200 log](../src/wire/tests/hardware/h7s/results_extensions_2026-09-12/cobs-table-253-115200.log), SHA-256 `6e26dbd165080daaa51310d6081ac3d53610e9eb76867a95b914dd14123c6eb5`.
- [1M log](../src/wire/tests/hardware/h7s/results_extensions_2026-09-12/cobs-table-253-1000000.log), SHA-256 `ef2706f53230436c5fb99821f69e1066169d86877aca80d3793c8bbeaf7ea1a3`.

The same pre-test download error also occurred on the FreeRTOS COBS Bitwise
115200 image. Its unchanged ELF passed download verification on attempt two,
then all local/UART checks passed. The
[failed first-attempt log](../src/adapters/tests/hardware/h7s/parity/results_extensions_2026-09-12/flash-0-1-115200-1.log)
has SHA-256 `542feb4655657dbfe6a642e17a06095ddc8957176c932c5b74efba74f12ac7d7`.

This does not establish a physical cause or a fix for the external programmer
issue. No runtime failure is retried or filtered into a passing row.

Each runner took a fresh 65,536-byte boot backup, restored it in `finally`,
verified the programming, and read all bytes back. All eight sessions'
actual backup and read-back files were rehashed and match:

```text
a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456
```

No option bytes, external flash, Cube source, ST-Link firmware, HAL or kernel
installation is modified. ELF/BIN/MAP/disassembly and programmer/transfer logs
remain in the ignored session paths named by the portable receipts.

All other image downloads and all restorations verified on attempt one.
The Qt matrix ran both endpoint modes at 115200 and 1M, with each side in
the client/server role. All eight expected timeouts are the unknown function
at script step 19, which a length-table server cannot frame. No unexpected
timeout, data mismatch or transport error occurred; clients used zero retries.

## Host/preflight and evidence checks

After extracting the shared test bodies, the full wire runner again passed
WSL ASan/UBSan and O3/LTO: 156 reader/CRC/storage/framer checks and 2,560,991
length-domain checks per build, plus its other storage/parity/OOM cases.
MSVC x64/x86 passed the full runner, including both new test groups and the
12 finite-wait configurations per architecture. The new FreeRTOS and all
six TCP images compile with strict C++ warnings as errors and exceptions
disabled, and fit the 64-KiB flash region.

Offline verifier controls passed: 16 fault-matrix tests, 12 bounded-flash
retry cases, 8 Heap/CRC oracle tests, 6 OOM/recovery verifier tests, and TCP
schema 1/2/3 positive controls plus 16/20/23 intentional record corruptions.
The new UART/DMA receipt passes all 15 fault-evidence mutation controls.
The real schema 3 FreeRTOS receipt also passes 69 verifier mutations/oracle
checks; the historical schema 2 control still passes its original 67.
These checks do not access the board. The TCP schema 3 preflight control is
explicitly synthetic, not substituted for a live receipt.

The prior audit's host/ARM evidence remains separately described in the
[audit report](EXTENSION_CONTRACT_AUDIT.md). The 18 retained before/after
ARM object pairs were rehashed and remain byte-identical. This is not
execution on all ARM families, Ethernet/TCP-IP interoperability, a guarantee
for arbitrary allocator implementations, or an unbounded reliability claim.

## Reproduce and recheck

The runners refuse existing output locations. Use a new directory/name for
each repeat and the actual connected COM port/probe serial. Individual
instructions and local-artifact verifier switches are in the harness READMEs:

- [COBS/RTU matrix](../src/wire/tests/hardware/h7s/README.md): `run_fault_matrix.py` / `verify_fault_matrix.py --local-images`.
- [FreeRTOS](../src/adapters/tests/hardware/h7s/parity/README.md): `run.py` / `verify.py --local-images`.
- [TCP/MBAP](../src/modbus/tcp/tests/hardware/h7s/README.md): `run.py` / `verify.py --local`.
- [UART/DMA](../src/adapters/tests/hardware/h7s/README.md): `run.py` / `verify.py --local-images`.
- [Heap/CRC](../src/wire/tests/hardware/h7s/heap_crc/README.md): full `run.py` (not `--probe`) / `verify.py --local-images`.
- [Heap recovery](../src/wire/tests/hardware/h7s/heap_crc/recovery/README.md): `run.py` / `verify.py --local-images`.
- [RTU framing](../src/modbus/rtu/tests/hardware/h7s/README.md): `run_framing.py` / `verify_framing.py`.
- [Qt interoperability](../src/adapters/qt/tests/hardware/h7s/README.md): `run_qmodbus.py` / `verify_qmodbus.py`.

Framing and Qt runs in this repeat explicitly select
`C:/ST/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe` so they use
the same 2.21.0 programmer as the other matrices. Qt keeps its documented
50-ms USB-aware server deadline, tracing on, zero retries and no injected
host stall. This does not test Qt's unmodified short native RX deadline.
