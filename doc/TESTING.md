<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Testing and evidence: what passed, and what that means

<!-- toc -->

Contents

- [Fast checks for a user or documentation change](#fast-checks-for-a-user-or-documentation-change)
- [Host regression suites](#host-regression-suites)
  - [Documentation refresh checkpoint, 2026-09-12](#documentation-refresh-checkpoint-2026-09-12)
  - [Documentation preservation follow-up, 2026-09-13](#documentation-preservation-follow-up-2026-09-13)
  - [Commands for the complete library suites](#commands-for-the-complete-library-suites)
- [Consumer, compiler and assembly checks](#consumer-compiler-and-assembly-checks)
- [Latest full H7S hardware checkpoint](#latest-full-h7s-hardware-checkpoint)
- [Hardware and benchmark entry points](#hardware-and-benchmark-entry-points)
  - [Historical raw records: direct links retained](#historical-raw-records-direct-links-retained)

<!-- /toc -->

[Documentation](README.md) · [Examples](EXAMPLES.md) · [Build/toolchains](BUILD.md)

Tests are grouped by the question they answer. A host fake, an ARM compile,
a disassembly guard and a live exchange are different kinds of evidence.
Do not infer one from another or attribute an old receipt to newly edited code.

## Fast checks for a user or documentation change

```sh
python -B doc/check_docs.py
sh doc/examples/build.sh
sh doc/examples/qt/build.sh
```

These check navigation/excerpts, 19 portable/fake-platform cookbook
configurations, and three Qt event-loop programs. The Qt TCP example opens
localhost sockets; the serial self-tests do not open COM ports. Compilation
and execution of examples do not replace the complete library regression.

## Host regression suites

### Documentation refresh checkpoint, 2026-09-12

The documentation/examples update was checked without flashing the board:

| Gate | Observed result |
|---|---|
| Maintained navigation/excerpts | 25 documents; local links/anchors and synchronized snippets pass `check_docs.py` |
| Checker negative controls | 9 unit tests pass, including missing files/anchors and malformed excerpt markers |
| Cookbook, WSL GCC | all 16 configurations pass ASan/UBSan, including process-exit destruction |
| Cookbook, MinGW GCC 13 | all 16 configurations compile and execute successfully |
| Qt 6.4.3 cookbook | COBS, RTU and localhost TCP programs pass self-tests and invalid-option rejection |
| Existing Qt adapter suite | 264 checks plus 22 server-trace checks pass; no COM port used |
| Real FreeRTOS/H7RS headers | both COBS and RTU task-entry TUs compile with ARM GCC, strict warnings, no DOC_HOST |

ASan exposed a cross-translation-unit destruction-order bug in the old host
fixtures: the fake HAL model died before static UARTs. The cookbook's host
wrapper now establishes their order in one translation unit; destructors run
normally and the sanitizer repeat passes. The active library code changed
only a historical-source comment. The removed legacy tree is recorded in
[the removal/recovery note](LEGACY_REVIEW.md). Saved hardware receipts and
the connected board were not modified by this checkpoint.

### Documentation preservation follow-up, 2026-09-13

The [content-preservation record](DOC_PRESERVATION.md) maps the previous
README/integration sections to their current homes. It records restored
manual RTU/UART examples, split adapter servicing, the README masthead and
the benchmark/raw-evidence links. The initial 16-configuration checkpoint
above is unchanged; the expanded cookbook now has 19 configurations.

| Gate repeated for the preservation follow-up | Observed result |
|---|---|
| Cookbook, WSL GCC with ASan/UBSan | 19/19 configurations pass, including process-exit destruction |
| Cookbook, MinGW GCC 13 | 19/19 configurations compile and execute successfully |
| Manual RTU/UART client | 53 checks without wake; 55 with wake |
| Combined/split RTU adapter recipe | both pass Busy, queued-continuation, empty-input and frozen-progress controls |
| Real H7RS HAL / FreeRTOS headers | four strict ARM GCC compiles: COBS task, RTU task, manual RTU with/without wake; no DOC_HOST |
| Qt cookbook | all three event-loop programs pass help/self-test/invalid-option checks; no COM port |
| Existing STM32/FreeRTOS adapter suite | `sh src/adapters/tests/run.sh` passes under WSL, including sanitized execution, parity, and the finite-wait tick matrix |
| Navigation/excerpts and negative controls | 26 maintained documents; 12 checker tests pass, including deletion of a still-existing target's link |
| Pre-reorganization README local destinations | 64/65 still directly linked by maintained guides; the sole exception is approved doc/old deletion with Git recovery |

This is documentation/example validation. No production implementation was
changed and no board was flashed. The raw hardware files remain byte-identical
to the pre-reorganization baseline; older results are not relabelled as fresh.

The manual RTU recipe has a deliberately application-owned, fixed request
budget. It is not a new library timer or a copy of the UART adapter's stale
rule. Both servicing forms of that adapter also check queued continuation
at deadline, empty input and frozen DMA progress.

### Commands for the complete library suites

Run from the repository root with a C++20 compiler and the documented tools:

| Suite | Command | Purpose |
|---|---|---|
| COBS | `sh src/cobs/tests/run.sh` | codec oracles, integrity, ownership, failure paths, compile-fail contracts |
| RTU | `sh src/modbus/rtu/tests/run.sh` | CRC policies, layouts, burst/framed input, malformed data, fuzz and ownership |
| TCP | `sh src/modbus/tcp/tests/run.sh` | MBAP stream parsing, fragmentation/trains, OOM skip, fail-closed, CRC extensions |
| Shared wire/API | `sh src/wire/tests/run.sh` | scalar readers/writers, type parity, storage and extension boundaries |
| CRC | `sh src/crc/tests/run.sh` | named-model checks, independent random oracle, codecs, NoCrc and custom policies |
| UART host | `sh src/uart/tests/host/run.sh` | fake-HAL interleavings, borrow safety, errors, recovery and invalid configuration |
| STM32/FreeRTOS adapters | `sh src/adapters/tests/run.sh` | wake, deadlines, lifecycle, UART/parser composition on fakes |
| Qt adapters | `sh src/adapters/qt/tests/run.sh` | real Qt event loop, serial fake, queued-client recovery and trace tests |

WSL provides the recorded ASan/UBSan path; optimized builds cover different
compiler behavior. MinGW/WSL suites can share `out/` directories: do not run
different toolchains concurrently against the same output. Read each script's
platform diagnostic rather than treating an unsupported sanitizer as a pass.

## Consumer, compiler and assembly checks

| Check | Entry point |
|---|---|
| Real downstream COBS consumer | `sh src/cobs/tests/qmake_consumer/run.sh`; [complete source](../src/cobs/tests/qmake_consumer/main.cpp) |
| Real downstream RTU consumer | `sh src/modbus/rtu/tests/qmake_consumer/run.sh`; [complete source](../src/modbus/rtu/tests/qmake_consumer/main.cpp) |
| Real downstream TCP consumer | `sh src/modbus/tcp/tests/qmake_consumer/run.sh` |
| Strict GCC/LTO consumers | `sh src/wire/tests/check_gcc_matrix.sh` |
| MSVC consumers/contracts | `src/wire/tests/check_msvc.ps1` |
| Shared CRC table emission | `sh src/wire/tests/check_shared_crc.sh` (ELF tools, not MinGW COFF) |
| COBS ARM layout | `sh src/cobs/tests/check_arm_layout.sh` |
| RTU ARM layout/CRC codegen | `sh src/modbus/rtu/tests/check_arm_layout.sh`, `check_arm_crc_codegen.sh` in that directory |
| TCP ARM layout/codegen | `sh src/modbus/tcp/tests/check_arm.sh` |
| Scalar ARM hot paths/matrix | `sh src/wire/tests/check_arm_hotpath.sh`, `check_arm_codegen_matrix.sh` in that directory |
| CRC ARM guard / full CPU matrix | `sh src/crc/tests/check_arm_codegen.sh`, `python -B src/crc/tests/check_arm_matrix.py` |
| Finite FreeRTOS wait codegen | `sh src/adapters/tests/check_wake_codegen.sh` |
| Real HAL port builds | `sh src/uart/tests/port/build.sh` |

The ARM matrix verifies emitted objects for the installed compiler's target
list, not runtime execution on every ARM chip. Unused-table and NoCrc claims
are object-emission properties with dedicated guards. Stack-usage reports
describe individual compiled frames, not a complete worst-case task stack.
See [Build](BUILD.md) and [the ARM audit](../src/crc/tests/ARM_AUDIT.md).

## Latest full H7S hardware checkpoint

The committed [2026-09-12 extension-contract repeat](HARDWARE_EXTENSIONS_2026-09-12.md)
records **81 image configurations in eight sessions** on NUCLEO-H7S3L8:
COBS/RTU/TCP, DMA fault/recovery, real FreeRTOS, Heap/Pool, real OOM and
peripheral CRC. Use that report's source/image hashes and restore receipts
for the exact claims. The firmware was restored with read-back verification.

Important retained limits:

- TCP ADUs transported over UART test the MCU protocol/parser path, not
  Ethernet, IP/TCP retransmission or a production socket adapter.
- Burst RTU framing controls remain failures where UART chunks are not whole
  ADUs. They are not relabelled as supported high-baud configurations.
- Deliberately unsupported-function Qt timeouts are expected controls, not
  successful function responses.
- Pre-test ST-Link programming failures and retries are recorded separately
  from executed library tests; successful retry is not a programmer-firmware fix.
- Nominal UART baud is not necessarily sustained VCP payload throughput.

## Hardware and benchmark entry points

These guides document prerequisites, flashing/restoration, independent host
oracles and evidence verification. Read the relevant runner's options before
touching a connected board; a documentation check is not authority to flash.

| Area | Guide |
|---|---|
| COBS over UART | [COBS H7S harness](../src/cobs/tests/hardware/h7s/README.md) |
| RTU and high-baud framing | [RTU H7S harness](../src/modbus/rtu/tests/hardware/h7s/README.md) |
| TCP core over UART | [TCP H7S harness](../src/modbus/tcp/tests/hardware/h7s/README.md) |
| UART CPU/chunks | [UART bench](../src/uart/tests/bench/README.md) |
| COBS host codec/Endpoint hot paths | [host benchmark](../src/cobs/tests/bench/README.md); `sh src/cobs/tests/bench/run.sh` |
| Matched protocol comparison | [shared H7S harness](../src/wire/tests/hardware/h7s/README.md), [comparison](PROTOCOL_COMPARISON.md) |
| CRC width/method | [live CRC benchmark](../src/modbus/rtu/tests/hardware/h7s/CRC_BENCHMARK.md) |
| Heap/Pool + peripheral CRC | [measurements and usage](HEAP_AND_HARDWARE_CRC.md) |
| True Heap exhaustion/recovery | [OOM harness](../src/wire/tests/hardware/h7s/heap_crc/recovery/README.md) |
| Real FreeRTOS and Qt interop | [full hardware report and session links](HARDWARE_EXTENSIONS_2026-09-12.md) |

The [documentation index](README.md#tests-measurements-and-hardware-receipts)
links earlier audits, plans and performance reports. Their dates, counts and
raw JSON/JSONL/CSV remain historical snapshots, not automatically updated test
results. This documentation change leaves those evidence files untouched.

### Historical raw records: direct links retained

These are the original raw entry points from the pre-reorganization README,
not new runs or performance promises for the current revision. Use each
harness/report for configuration, provenance and acceptance limits. In
particular, failed high-baud burst controls remain failures.

| Original checkpoint | Raw record / explanation |
|---|---|
| COBS audited baseline, 2026-09-01 | [JSONL](../src/cobs/tests/hardware/h7s/results_audited_2026-09-01.jsonl) |
| COBS concise Format/Pool API, 2026-09-01 | [JSONL](../src/cobs/tests/hardware/h7s/results_format_api_2026-09-01.jsonl) |
| UART default 128x8 at 10M, 2026-09-01 | [CSV](../src/uart/tests/bench/results_default128x8_10M_audited_2026-09-01.csv); [chunk comparison](../src/uart/tests/bench/README.md#fresh-audited-run-2026-09-01) |
| RTU accepted baseline, 2026-09-02 | [JSONL](../src/modbus/rtu/tests/hardware/h7s/results_audited_2026-09-02.jsonl) |
| RTU scalar API, 2026-09-02 | [JSONL](../src/modbus/rtu/tests/hardware/h7s/results_scalar_api_final_2026-09-02.jsonl) |
| RTU paranoid Os, 2026-09-02 | [JSONL](../src/modbus/rtu/tests/hardware/h7s/results_paranoid_final_2026-09-02.jsonl) |
| RTU O2, 2026-09-02 | [JSONL](../src/modbus/rtu/tests/hardware/h7s/results_paranoid_o2_2026-09-02.jsonl) |
| RTU O3/LTO, 2026-09-02 | [JSONL](../src/modbus/rtu/tests/hardware/h7s/results_paranoid_o3_lto_2026-09-02.jsonl) |
| RTU extracted CRC module, 2026-09-05 | [JSONL](../src/modbus/rtu/tests/hardware/h7s/results_crc_library_2026-09-05.jsonl) |
| RTU Bitwise/Table A/B, 2026-09-05 | [JSONL](../src/modbus/rtu/tests/hardware/h7s/results_crc_policy_2026-09-05.jsonl) |
| RTU 3M IDLE-boundary probe, 2026-09-02 | [JSONL](../src/modbus/rtu/tests/hardware/h7s/results_high_baud_probe_2026-09-02.jsonl) |
| RTU framing versus burst, 2026-09-05 | [JSONL](../src/modbus/rtu/tests/hardware/h7s/results_framing_2026-09-05.jsonl) |

The historical 10M COBS extended-run counts (61,611 frames / 19,133,016 payload
bytes) remain in the [COBS H7S report](../src/cobs/tests/hardware/h7s/README.md).
The CPU/throughput methodology remains in [COBS performance](COBS_PERFORMANCE.md)
and [matched COBS/RTU comparison](PROTOCOL_COMPARISON.md). A host benchmark run
does not validate the current board or reproduce those old measurements.
