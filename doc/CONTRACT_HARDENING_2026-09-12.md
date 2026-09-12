<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Reader, CRC-policy and FreeRTOS wait contract fixes

Follow-up to [the cross-stack audit](PARANOID_AUDIT_2026-09-12.md), starting
from `6e27630387753ea90c0b1e19238266c193e4d8b1`. This is scoped to reproduced
contract failures; no wire format, protocol framing, UART DMA ownership,
cache/ISR path, storage geometry or default payload limit was redesigned.

Status: completed. Fixed-source host/ARM checks and **53 final hardware
images** passed their defined runtime criteria. An additional 14-image
initial FreeRTOS receipt is pinned by `56961392ae10` before the compile-time
1-kHz optimization. One pre-test programming failure is retained below;
the underlying programmer/flash-controller issue is not claimed resolved.

## Reproductions and fixes

### CRC call expression, not checksum semantics

`crc::Policy` checked `store(destination, const_value)` with a const lvalue.
COBS instead supplied `calculate(...)` directly, a temporary. An accepted
policy with a deleted rvalue-result overload compiled in RTU/TCP but failed
in COBS. A throwing rvalue overload instead reached `std::terminate` inside
the `noexcept` COBS send path. Built-in policies were unaffected.

COBS now names a const checksum first, matching RTU/TCP. The common concept
also checks temporary span and computed pointer arguments: an unchecked
rvalue overload must not escape the exception contract. `verify()` keeps its
trailer pointer const, matching the checked expression, and still avoids
null-pointer arithmetic for empty NoCrc frames.

The shared [contract checks](../src/wire/tests/contract_checks.h) instantiate
deleted and potentially-throwing rvalue-result overloads in all three
protocols. The latter records any wrong call on the MCU without requiring
exceptions. Negative concept assertions reject throwing temporary-span,
destination-pointer and source-pointer overloads. A policy is still free to
return a sum or any other checksum; there is no semantic CRC validation.

### Enum reader domain

Reading `FF FF FF FF` into `enum Plain { Zero, One };` reproduced a UBSan
invalid-enum load in the shared native reader. Bounds checking alone does
not make every object representation valid for an unfixed enum. Documentation
already advised fixed-width wire types, but the reader accepted the unsafe
type at its call boundary.

All reader namespaces now accept **scoped enums (`enum class`) only**.
C++20 provides no portable trait to distinguish fixed from unfixed unscoped
enums, so both unscoped forms are rejected. For an existing unscoped enum,
read an integer and validate its range before converting, or change the
declaration to `enum class Op : uint16_t`. This source-compatibility change
is explicit; writer support for already-valid unscoped values is unchanged.

The restriction is compile-time only. It adds no enum-range branch to the
integer hot path. An unnamed value of a scoped enum is still accepted;
application meaning is not library framing. Tests cover native/BE/LE values
including `0x8000` and `0xFFFF`, short-input failure without cursor/output
changes, and compile-time rejection through all protocol facades.

### Finite FreeRTOS wait conversion

The installed FreeRTOS V10.6.2 macro uses a narrow multiplication before
division. At 1 kHz with a 32-bit tick, `4,294,968 ms` became zero ticks and
`7,200,000 ms` became `2,905,032` ticks although the correct results fit.
The old host fake was an identity and concealed this kernel behavior.

`FreeRtosWake::wait()` now computes using a wide intermediate, truncates to
whole ticks and saturates at `portMAX_DELAY - 1`. A finite duration cannot
silently become the indefinite-wait sentinel. Application overrides of
`pdMS_TO_TICKS` do not change this bounded conversion. The 50-ms default,
adapter deadline selection, task-notification ownership and ISR remain
unchanged. A coarser tick still permits a zero-tick wait below one tick, as
documented; that is distinct from arithmetic overflow.

GCC Cortex-M0 `-Os` initially retained a 64-bit multiply in the clamp even
at 1 kHz. An explicit `if constexpr` removes that arithmetic. The inspected
M0/M7 default conversion is just `movs r0, #50; bx lr`. Dynamic 1-kHz
conversion is a finite-range clamp with no arithmetic helper call. There
are no new fields, allocations, COBS timers or interrupt-side operations.

## Host and assembly evidence

All following commands executed against the fixed sources; saved local logs
are in each suite's existing ignored `out/` directory.

- `sh src/wire/tests/run.sh`: sanitized and O3/LTO, including 120 new
  reader/policy checks per build over Heap and Pool, 244 lifecycle checks,
  2,916 payload-limit checks and real allocator-interposed OOM tests.
- COBS, RTU, TCP, CRC, UART and adapter host suites passed. Examples include
  20,361 COBS integrity checks, 960,800 exhaustive decoder streams,
  177,146 encoder cases, RTU damaged-stream fuzz, TCP 75,016 core / 18,284
  advanced / 1,620 limit checks, and 275 UART assertions per host variant.
- `sh src/adapters/tests/run.sh`: 16/32/64-bit ticks at 100/1000/1024/10000 Hz,
  100,013 duration checks per configuration in sanitized and optimized builds
  (**2,400,312 executions**), plus the existing adapter lifecycle suites.
- MSVC x64/x86 `/WX`, Qt adapters (264 checks + 22 trace checks), integration
  examples, qmake consumers and the Qt application build passed. No Clang
  execution or remote CI result is claimed.
  MSVC also executed all twelve tick-width/rate combinations on both host
  architectures against the final optimized conversion.
- The strict GCC O3/LTO consumer matrix passed on WSL and MinGW, including
  the new contracts under `-fshort-enums -funsigned-char` and strict bounds,
  aliasing and alignment warnings.
- ARM UART real-HAL F1/G4/H7RS guards and TCP 72 default objects plus CRC
  controls passed. The shared codegen matrix passed 96 scalar, 96 protocol
  and 48 COBS objects. Shared Table linking still emits one 512-byte lookup;
  NoCrc/Bitwise emit no table.
- [CRC report](../src/crc/tests/results_arm_contracts_2026-09-12.json):
  **6,360 objects, 106 named AArch32 CPU targets, zero failed guards**.
  This is compilation/disassembly, not execution on 106 CPUs.
- `sh src/adapters/tests/check_wake_codegen.sh`: **108 objects** across
  M0/M4/M7, Os/O2/O3, 16/32/64-bit ticks and four tick rates. Constant default
  waits are helper-free; 1-kHz dynamic conversion is helper-free too.

## Live board evidence

NUCLEO-H7S3L8, ST-Link `002A001F3033510135393935`, COM6, 600-MHz M7,
FreeRTOS V10.6.2 where used. Each guarded session takes a fresh full 64-KiB
boot backup, verifies every image before testing, restores in `finally`,
and compares a fresh read-back hash. Runtime test failures are not retried.

The [initial real-FreeRTOS receipt](../src/adapters/tests/hardware/h7s/parity/results_contracts_2026-09-12/session.json)
passed **14 images, 704 exchanges, 560 exact echoes, 1,596 MCU-local checks**.
It is tied to `56961392ae10`, before the final 1-kHz codegen optimization.
Each image executes all three protocols locally with the custom CRC policies
and scoped-enum readers. It also checks 13 conversion boundaries, creates a
static notifier task, and proves two large waits actually block until that
task notifies after ten ticks. It does not wait for an hours-long timeout.

The [final optimized FreeRTOS receipt](../src/adapters/tests/hardware/h7s/parity/results_contracts_final_2026-09-12/session.json)
repeats all **14 images, 704 exchanges, 560 exact echoes and 1,596 local
checks** against the explicit 1-kHz path. All passed, as did the independent
verifier's 67 mutation/oracle checks. Every image flashed on attempt one;
the original boot image was restored and read back. This is the final
FreeRTOS evidence; the initial receipt above remains a separate checkpoint.

The [full COBS/RTU matrix](../src/wire/tests/hardware/h7s/results_contracts_2026-09-12/session.json)
passed **33 images, 93 COBS + 144 RTU suite records**, with exact negative
counters, retained Packet ownership, deliberate pool exhaustion and recovery.
COBS NoCrc/Bitwise/Table ran at 115200/1M/3M/6M/10M; all nine RTU policies
ran at 115200/1M. One pre-test programming attempt failed (below); its
verified retry and runtime tests passed. Full backup/restore read-back matched.

The [TCP-core UART receipt](../src/modbus/tcp/tests/hardware/h7s/results_contracts_2026-09-12/session.json)
passed all **six images**, over Heap/Pool with NoCrc and selected CRC16/32
policies: **24,850 MCU TCP checks, 36 OOM checks, 318 local RTU data checks,
1,974 byte-exact UART exchanges and 38 live fail-closed trials**. The useful
data limit is 1024 bytes. Every image flashed on attempt one. These tests
do not constitute Ethernet/TCP-IP interoperability, and no new CPU-utilization
benchmark is claimed.

All four sessions (initial FreeRTOS and the three final matrices) restored
their fresh boot backup and independently read all 65,536 bytes back. All
backup/read-back hashes matched:
`a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456`.
Independent receipt verifiers passed against the retained local artifacts.
The FreeRTOS verifier also passed its 67 mutations on both new receipts;
the TCP verifier passed its 16 original-schema and 20 data-limit-schema
corruption controls. Historical records were not overwritten.

## The separate ST-Link programming failure

Verbose (`-vb 3`) programming is now enabled in the relevant runners. A new
pre-test failure occurred while flashing the COBS Bitwise 10-Mbaud image in
the full matrix. The [complete verbose failure and retry log](../src/wire/tests/hardware/h7s/results_contracts_2026-09-12/cobs-bitwise-253-10000000.log)
is preserved byte-for-byte (SHA-256
`ac787655126f3d50b581bd5a160bd01e78e5fe6ebbb06e2b1d160730bcab2256`).
The same ELF subsequently programmed, verified and passed
its runtime tests. The failed attempt is evidence, not a passed flash.

The log shows successful attachment, loader initialization and erase, then
`R0 = 0` from ST's `Write` entry at `0x2000004E` during the first flash write.
Inspection of the installed `0x485.stldr` identifies its final test of
`FLASH_ISR` at `0x52002024` against mask `0x1F3E0000`; the explicit failure
return is selected when a tested flag is set. Loader SHA-256:
`9d27f9c89463e02fda497e2538b0916c595d261dc66bf3fbb6b88b160fcde4af`.

This localizes the observed rejection much better than the old short log,
but does not identify which status flag caused it or why. The exact ISR
value was not captured before the retry. A later CubeProgrammer attachment
can itself clear ECC status flags, so reading registers after a new attach
must not be misrepresented as a preserved failure snapshot. No programmer,
ST-Link firmware, option-byte or vendor flash-loader modification was made.
The underlying programming problem is **not claimed fixed** by retries or
by the library changes.
