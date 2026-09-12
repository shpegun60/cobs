<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Cross-stack audit: TCP, payload limits, DMA ownership and Qt recovery

This audit covers the working tree containing the new transport-independent
Modbus TCP core and the [data-limit API migration](PAYLOAD_LIMITS.md), based on
`ef44ea36e35b2641e831c705dd556d005b7a9f67` plus those pending changes. Older
benchmark receipts are historical, not evidence that this tree was tested.

Status: completed. All recorded final host/assembly checks and five fresh
hardware matrices passed their defined acceptance criteria. The five live
sessions cover **64 flashed images** and restore the original boot firmware;
this is bounded correctness evidence, not a claim that all possible bugs or
hardware failures have been eliminated.

## Review boundary

Reviewed the production paths in `wire`, `crc`, `cobs`, `modbus/rtu`,
`modbus/tcp`, `uart`, and the STM32/FreeRTOS/Qt adapters, including:

- Numeric limits before addition/narrowing; zero data; widest legal ADUs;
  alignment of every RX slot; exact TX descriptors, short grants and overgrants.
- Message growth/moves/finalization/retries; foreign-instance rejection;
  Packet copy/move/reset and queue/partial-frame destruction.
- COBS segmented decoding, canonical in-place encoding and length/CRC errors;
  RTU complete-candidate versus length-framed input; TCP MBAP Header/Body/Skip/
  Failed transitions and explicit new-stream reset.
- Compile-time endian codecs, CRC-width geometry, NoCrc and unused tables;
  stateful/custom calculators retain their documented structural contract.
- UART initialization, RX/TX ownership, overflow/gap ordering, DMA counters,
  teardown gates, cache maintenance, watchdog recovery and interrupt wakeups.
- Adapter attach/detach, partial input, deadlines, empty deliveries, Qt write
  errors/partial writes and the RTU client's transaction lifetime.

No new framing policy, COBS timer, CRC semantic validation, shared parser
cursor, virtual transport interface or protocol-specific storage was added.
Public raw-byte storage and normal wire formats remain as documented.

The standard Modbus defaults remain 252 function-data bytes: RTU derives a
256-byte ADU with CRC16; TCP derives a 260-byte ADU with NoCrc. The normative
size relation is in [Modbus Application Protocol V1.1b3, section 4.1](https://www.modbus.org/file/secure/modbusprotocolspecification.pdf). Explicit
larger frames or nonstandard integrity semantics are private extensions.

## Reproduced defects and fixes

### 1. UART READY was not sufficient evidence to release a DMA buffer

Two actual HAL control flows violated the former assumption:

1. In STM32Cube H7RS V1.3.0's `HAL_UART_IRQHandler`, the IDLE path changes
   `RxState` to READY before calling `HAL_DMA_Abort`, then ignores the abort
   result. A failed channel suspend can therefore reach the driver's RX
   callback with an unconfirmed hardware stop.
2. `UART_DMAError` calls `UART_EndRxTransfer` / `UART_EndTxTransfer` for both
   active UART directions. These helpers change the UART software states;
   they do not abort the independent, non-failing DMA channel. READY on that
   direction is not permission to recycle its buffer.

The previous fake HAL only modelled the fully stopped error path. New
fixtures model an ignored IDLE-abort failure and both cross-direction DMA
faults with persistent abort/reinitialization failures. Against the old
driver, **271 checks produced seven failures**, including double-arming a
still-owned RX destination and clearing TX busy while DMA still borrowed it.

The driver now checks DMA readiness before RX publication and TX completion.
The error ISR retains an unconfirmed RX claim for thread-context restart.
For an unconfirmed TX stop it latches a terminal failure, keeps the original
borrow alive and schedules `finishTx(false)` through the existing work
doorbell. Failed repairs keep that work pending even under CTS. A late TC
cannot change a latched failure into success; the watchdog rechecks the
latched verdict after acquiring its teardown gate as well.

No blocking abort, heap allocation, payload copy or additional cache operation
was added to an ISR. The pending-TX bit occupies existing Cortex-M padding;
UART object/buffer geometry is unchanged. RX cache invalidation still occurs
before arming and only after a confirmed stopped RX DMA before publication.

Final host regressions also cover CTS and late TC: **275 checks, zero
failures**, across ordinary, old-RxEvent fallback, registered-callback,
external-callback, optimized and sanitized builds. An additional **1,000,000**
seeded operations (`0xDEADBEEF`) passed the ownership model.

### 2. Qt COBS could complete a frame across discarded input

`SerialAdapter::discard_incoming()` and `detach()` only discarded the RTU
partial frame. With COBS they left the decoder's building block alive. After
clearing the port, unbinding/rebinding, or replacing the adapter, an old tail
could complete the old packet, despite the known discontinuity.

Both paths now call COBS `notify_gap()`: the building block is returned,
completed queued packets survive, and input is discarded through the next
`0x00`. No timer participates. Tests cover successful and failed port clears,
unbind/rebind and adapter destruction, with both NoCrc and CRC16. Each test
checks Pool occupancy, preservation of a completed packet, rejection of the
old tail and subsequent resynchronization.

### 3. Empty Qt deliveries could keep an RTU frame alive indefinitely

The Qt adapter restarted its existing 50-ms RTU silence timer even when
`deliver({})` contained no bytes. It now updates that timer only for nonempty
input; polling and service notification remain available on an empty event.
Ten empty deliveries over 100 ms no longer retain an abandoned frame.

The two Qt additions reproduced **17 failures out of 264 checks** before the
fix; all **264** pass afterwards. Qt's separate server-trace checks also pass
(**22**). These tests use a real Qt event loop and a QIODevice stand-in, not
an attached serial port.

**COBS has no silence timer.** The shared Qt adapter has a QTimer object which
also supplies its QObject connection context, but its timer is only armed for
framed RTU. COBS core and STM32 COBS adapter remain clockless.

## Assembly and host verification

The new DMA predicates are forced inline: GCC `-Os` initially outlined the
small predicate and TX ISR after the guard was added. Explicit inlining
removes those avoidable calls without removing any checks. The pinned
GCC 14.3.1 Cortex-M4 port guard now asserts:

| Symbol / property | Before | After |
|---|---:|---:|
| RX callback thunk | 100 bytes | 116 bytes |
| RX individual stack frame | 8 bytes | 8 bytes |
| TX callback thunk | 94 bytes | 118 bytes |
| `receiveArm` | 108 bytes | 108 bytes |
| `publishActive` | 40 bytes | 40 bytes |
| idle `proceed` | 36 bytes | 36 bytes |

These are static code sizes, not executed instruction counts or CPU load.
Individual `-fstack-usage` frames do not prove cumulative interrupt stack use.
The disabled-probe disassembly remains byte-identical to the explicitly
stubbed-probe build. F1/G4/H7RS real HAL compilation, callback variants,
`-fanalyzer`, optimized builds and enabled-probe controls pass.

Fresh verification commands (logs are local ignored build output):

```sh
sh src/wire/tests/run.sh
sh src/crc/tests/run.sh
sh src/cobs/tests/run.sh
sh src/modbus/rtu/tests/run.sh
sh src/modbus/tcp/tests/run.sh
sh src/uart/tests/host/run.sh
src/uart/tests/host/out/sanitizers.exe --seed 0xDEADBEEF --steps 1000000
sh src/adapters/tests/run.sh
sh src/adapters/qt/tests/run.sh
sh doc/examples/build.sh
sh src/uart/tests/port/build.sh
sh src/modbus/tcp/tests/check_arm.sh
sh src/wire/tests/check_arm_codegen_matrix.sh
sh src/wire/tests/check_shared_crc.sh
sh src/modbus/rtu/tests/check_arm_layout.sh
sh src/cobs/tests/check_arm_layout.sh
sh src/adapters/tests/check_stm32_crc.sh
sh src/wire/tests/check_gcc_matrix.sh
python -B src/crc/tests/check_arm_matrix.py --jobs 4 --output src/crc/tests/out/audit-all-arm --report src/crc/tests/out/audit-all-arm.json
```

The WSL suites use ASan/UBSan and optimized variants, not merely a successful
compile. In particular: shared payload limits **2,916 / 324 combinations**;
TCP **75,016 core + 18,284 advanced + 1,620 data-limit checks**; COBS **20,361
integrity + 960,800 decoder + 177,146 encoder cases**. RTU includes sanitized
and optimized framing/damaged-stream fuzzing and 11 negative compilation
contracts. All three qmake consumers pass. `check_msvc.ps1` passes its full
MSVC x64/x86 matrix, including TCP and the three-protocol payload tests.
The strict GCC matrix passes under both WSL GCC and MinGW 13, now including
the TCP consumer alongside COBS/RTU, with aliasing/bounds/conversion warnings
as errors and O3/LTO; scalar controls also use short enums and unsigned char.

TCP's ARM matrix checks 72 default NoCrc objects (eight Cortex-M targets,
three optimization levels, little/big/strict access), with no CRC table,
HAL, heap or floating-point-helper dependency in those probes, plus positive
Bitwise/Table controls. The shared matrix passes 96 scalar, 96 protocol and
48 COBS objects; the ELF multi-TU guard still links one CRC16 table when used
by both COBS and RTU, and none for Bitwise/NoCrc. These are code-generation
checks, not runtime validation on all those CPUs.

The complete CRC AArch32 audit was also rerun: **6,360 / 6,360 objects passed
for 106 named CPU targets**. The [case-level report](../src/crc/tests/results_arm_audit_2026-09-12.json)
retains source/compiler identities and every object's/disassembly's hash,
table size and static instruction counts. It covers 5,724 calculator objects
(nine policies, three optimizations, two byte orders per CPU), plus 636
strict-alignment codec controls. M-profile uses Thumb, other targets ARM
state, all soft-float; this is not an AArch64 or all-ABI claim. The existing
[matrix interpretation](../src/crc/tests/ARM_AUDIT.md) still applies.

## Live verification and retained evidence

Board: NUCLEO-H7S3L8, Cortex-M7 at 600 MHz with I/D caches enabled, COM6,
ST-Link `002A001F3033510135393935`. Every session starts with a fresh backup
of the current 64-KiB boot flash and restores it in `finally`, then compares
a full read-back. No option bytes or unrelated memory are programmed.

Completed fresh sessions:

| Matrix | Retained evidence | Result |
|---|---|---|
| COBS/RTU | [Receipt and 33 raw JSONL files](../src/wire/tests/hardware/h7s/results_audit_2026-09-12/session.json) | 33 images; 93 COBS + 144 RTU suite records; all passed |
| DMA / RTU adapter faults | [Receipt](../src/adapters/tests/hardware/h7s/results_dma_audit_2026-09-12/session.json), [trials](../src/adapters/tests/hardware/h7s/results_dma_audit_2026-09-12/results.jsonl) | 3 images (-Os/-O2/-O3), 186 live trials, 2,037 MCU assertions; all passed |
| TCP over UART | [Six-image raw record](../src/modbus/tcp/tests/hardware/h7s/results_audit_2026-09-12/session.json) | 24,850 MCU core checks, 36 real OOM checks, 318 MCU-local RTU data checks, 1,974 exact UART exchanges, 38 live fail-closed trials; all passed |
| QtSerialBus interoperability | [Record](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_audit_2026-09-12.json), [restore/image receipt](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_audit_2026-09-12.json.session.json) | 8 role/baud/framer runs, 660 reference-model verdicts; zero unexpected outcomes |
| Real FreeRTOS API/task path | [Receipt](../src/adapters/tests/hardware/h7s/parity/results_audit_2026-09-12/session.json), [raw exchanges](../src/adapters/tests/hardware/h7s/parity/results_audit_2026-09-12/results.jsonl) | 14 images, 704 exchanges including 560 exact echoes, 280 MCU-local lifecycle checks; all passed |

These counts describe different units; they are not added into one test total.
COBS covers CRC16 Bitwise/Table with 253 data bytes and NoCrc with 1024 bytes,
each at 115200/1M/3M/6M/10M. RTU covers all nine built-in integrity policies
at 115200 and 1M, with the complete-candidate contract. Suites include real
Pool exhaustion, deliberate corruption/gaps and recovered ownership. NoCrc's
acceptance of corruption is tested as such, not called integrity detection.

TCP's six Pool/Heap/policy images each advertise **1024 useful data bytes**,
deriving ADUs of 1032/1034/1036 bytes. All 256 function values, empty/full
payloads, stream cuts and trains are covered. The [TCP report](../src/modbus/tcp/tests/hardware/h7s/README.md#cross-stack-audit-repeat)
distinguishes live byte exchanges from MCU-local loops and actual heap OOM.

The raw backup and read-back files were independently rehashed locally, not
only compared as strings in a JSON receipt. Every completed session matched:

```text
a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456
```

Portable verifiers passed and local-image modes re-read retained artifacts.
The full-matrix verifier also passed 16 mutation checks against this new
record; the expanded DMA verifier passed 15 deliberate evidence corruptions.
TCP's schema 2 control passed 20 mutations against the new record; the
FreeRTOS verifier passed 67 mutations/oracle controls against its new record.
These verifier tests never open hardware.

The Qt repeat uses both MCU roles and both burst/framed RTU at 115200/1M,
with Qt 6.4.3, zero request retries, a 1000-ms response timeout and the
documented USB-aware 50-ms Qt-server fragment deadline. The only non-`ok`
steps are the expected step-19 unknown-function timeouts for a table-framed
server; they are retained, not described as lost standard requests. No
unexpected timeout occurred in this repeat. Eight retained MCU ELFs, their
successful flash logs and actual restored boot bytes were rechecked locally.
The earlier observed Qt failures remain in their own historical receipts.

The FreeRTOS V10.6.2 matrix uses the ordinary `FreeRtosWake::wait(adapter)` /
`adapter.proceed()` loop: COBS and framed RTU with NoCrc/CRC16 Bitwise/Table,
plus burst RTU with Bitwise, at 115200/1M. It checks actual notification wakes,
idle sleeps, framed deadlines, active-borrow bind/unbind refusals, partial-RX
detach/recovery and retained Packet ownership. There were no MCU assertion,
ISR-context or task-context failures. The local verifier additionally checked
the external kernel source hashes and the real restored flash files.

One programming attempt in the 33-image matrix failed before any test ran:
COBS NoCrc/1024 at 115200 reported `failed to download Sector[0]`. The guarded
runner flashed the **same ELF** successfully on its second attempt, then ran
the full suite once. The [complete failed-then-successful programming log](../src/wire/tests/hardware/h7s/results_audit_2026-09-12/cobs-none-1024-115200.log)
is retained byte-for-byte and matches the receipt's `log_sha256`. This run
does not establish the programming failure's physical cause. All images in
the other four sessions flashed on attempt one. Runtime trials were not
automatically retried or silently discarded.
The recorded base commit is the starting point, not an assertion that it
already contains these changes: source fingerprints identify the measured
bytes, and provenance resolves their committed versions after the checkpoint.
Local Cube inputs are deliberately untracked external dependencies; their
hashes are verified locally and are not claimed to be vendored in Git.

The new adapter-fault commands are deliberately precise about what is real:

- `I` receives eight real UART bytes with IDLE publication suppressed, then
  injects UART READY while the real RX DMA is still BUSY. It verifies that
  no prefix is delivered before safe recovery and exactly one gap results.
- `J` stops only RX DMA, calls ST's installed DMA-error callback and proves
  the still-live TX borrow survives until thread-context abort, producing
  exactly one failed terminal verdict.
- `K` mirrors the fault onto TX. RX's buffer is not recycled while its
  original DMA destination is live; recovery gives one gap and a fresh arm.

These are controlled state/callback injections around real running DMA,
not a claim that a physical bus timeout was induced. There are no production
test hooks. Schema 2 receipts add these trials; schema 1 historical receipts
retain their original plan. Verifier mutations check the new ownership
observations, missing trials, short physical input and unexpected preambles.

The early one-round DMA pilot was exploratory and preceded final forced
inlining. It is retained locally with its ignored Cube session, not presented
as evidence for the final source tree; the published three-image session
above was rebuilt after the final production edits.

## Limits of the verdict

The fixes are evidence-backed, not a guarantee against every possible bug.
Custom storage/policies/delegates must still honor their documented lifetime,
alignment, non-overlap, exception and non-reentrancy contracts. Packets are
non-atomic shared owners in one execution domain, not cross-task shared_ptr.
Framing Layouts must use their validated factories; arbitrary corruption of
public fields or invalid input spans is outside the contract.

UART instances/buffers must remain DMA-accessible and alive, including after
a persistent hardware stop failure. Vendor HAL code still executes before
the library callbacks: a HAL ISR which itself never returns cannot be fixed
by a check in a callback that it never reaches. These tests do not claim
recovery from every peripheral/bus wedge or arbitrary NVIC misconfiguration.

RTU IDLE/known-length framing remains the documented pragmatic transport
contract, not a strict physical t1.5/t3.5 implementation. TCP-over-UART tests
prove MBAP/core/ownership behavior, not sockets, Ethernet, TCP retransmission
or multi-connection interoperability. Baud configuration is not proof of
continuous line throughput, and older CPU percentages are not relabelled as
measurements of this UART revision.
