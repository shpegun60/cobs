<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# Shared policies validation, 2026-09-05

The shared-storage migration, configurable RTU ADU ceiling and CRC-bearing
COBS default passed the checks below. This report describes the working tree
continued from `a1ac09c98863678cc15060712d79d78638e6c5c0`, not a claim that
an older green commit validates uncommitted changes. Hardware records carry
source SHA-256 values and exact image identities. The executable recheck is:

```bash
python -B wire/tests/verify_hardware_migration.py
```

It verifies 85 COBS and 127 RTU records, the current measured source hashes,
the 6360-object ARM report, ownership/accounting assertions and derived CPU
arithmetic. Historical hardware JSONL and benchmark tables were preserved.

## Delivered boundary

```cpp
using Memory = wire::Pool<8, 2>; // same specification in both protocols
using Cobs = cobs::Endpoint<Memory, cobs::Format<>>;
using Rtu = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<>>;
```

- COBS defaults to CRC16 Bitwise and 253 useful bytes. Explicit
  `Format<crc::NoCrc, 255>` retains the old H1 wire format.
- RTU defaults to CRC16 Bitwise and a 256-byte physical ADU. Its configurable
  ceiling is independent of the memory strategy; useful size is MaxAdu-2-W.
- A shared memory specification implements `For<Geometry>` and four raw-byte
  operations. Geometry has exactly three alignment/maximum constants.
- CRC policy is structural, not a semantic whitelist. Sum, custom-width,
  custom-codec and stateful calculator policies remain supported.
- Protocol-specific Packet/Message/Receiver and owning delegates remain.
  UART code, its framing boundary and DMA/cache ownership were not redesigned.

The [plan](SHARED_POLICIES_PLAN.md), [storage contract](STORAGE.md),
[COBS protocol](PROTOCOL.md) and [RTU guide](../modbus/README.md) are the
current usage/architecture references.

## Host and compile-time evidence

| Check | Result / scope |
|---|---|
| `sh crc/tests/run.sh` | GCC ASan+UBSan O1 and O3/NDEBUG; 20 contract/vector/property checks per build |
| `sh wire/tests/run.sh` | Raw BlockPool; Heap/Pool, real COBS/RTU Geometry, NDEBUG checks; shared custom-memory and API-parity suites |
| `sh cobs/tests/run.sh` | Headers, 9 intended compile failures, legacy v1 suites, inverse geometry and 20,360 integrity checks at O1 and O3 |
| Codec differential oracle | 960,800 decoder streams and 177,146 encoder/headroom cases in each checked/optimized run |
| `sh modbus/rtu/tests/run.sh` | Headers, 9 compile failures, every policy, 104 geometry checks, ownership and 100,000 random candidates; checked/sanitized and optimized runs |
| RTU integration | Fake-HAL UART IDLE/TC/gap/borrow suite passed |
| qmake | Both real consumer executables passed with Qt 6.10.1 / MinGW 13.1 |
| `wire/tests/check_gcc_matrix.sh` | Strict O3/LTO, aliasing/conversion warnings, perturbed scalar ABI flags |
| `wire/tests/check_msvc.ps1` | Eight CRC/storage/protocol/layout suites on both MSVC x64 and x86, O2/NDEBUG; no sanitizer claim for these builds |

The new COBS integrity suite includes every built-in policy, all input splits
for short frames, empty and maximum payloads, H1/H2 transition, payload and
trailer corruption, a sum with a big-endian codec, a 300-byte custom trailer,
and non-default-constructible stateful CRC. It proves retry does not recalculate
CRC and RX uses the exact TX calculator instance. Fully delimited CRC failure
drops one frame without consuming the next valid frame.

The mixed-version test explicitly demonstrates v2 `41 42` arriving at a v1
NoCrc peer as `41 42 B1 D1`. There is no auto-detection or universal rejection
of mismatched configurations.

RTU boundaries include minimum legal ADUs for all trailer widths, MaxAdu
64/1024/65535, and rejection of 0, 1 and an insufficient CRC envelope before
any unsafe subtraction/allocation. A smaller MaxAdu is only local capacity.

Shared custom-memory tests preserve the exact original descriptor for grants
above Geometry's maximum and reject undersized grants transactionally. RX
slot alignment, retained packets and transport borrowing are tested. Custom
storage does not need a Geometry alias, protocol metadata or independent
quotas unless it chooses to promise them; built-in independent quotas are
tested explicitly.

## Layout and machine code

On GNU ARM32, both private RX headers stay 16 bytes and both Packet handles
stay 4 bytes. COBS Message is now 28 instead of 24 bytes: one cached size_t
holds useful capacity, avoiding inverse-geometry arithmetic on every append.
Its additional CRC-error counter grows Receiver 72 to 76 bytes and Endpoint
160 to 168 bytes after ABI padding. RTU Message remains 24 and Endpoint 128.

The new default COBS `Pool<8,2>` Endpoint is 2904 bytes on GNU ARM32. Its
Geometry is RX=272, TX=259, alignment=4. TX=259 is the maximum allocation,
not the payload limit; the user still sees 253 useful bytes. Pool free-list
alignment/padding is included in sizeof(Endpoint), not in that TX request.

GCC x64 COBS Message/Endpoint are 56/288 bytes. The legacy H2/1024 Pool
Endpoint is 10784 bytes. MSVC has separate measured ABI assertions: its H2
Heap/Pool endpoints are 272/10768 bytes on x64 and 168/10592 on x86. Do not
assume the standard no_unique_address hint forces identical compression on
every compiler; the MSVC x64 Pool snapshot includes an extra padded empty
policy slot compared with the pre-CRC layout. Bitwise/Table of equal width
remain identical in footprint and in Storage/Packet/Message types.

ARM verification:

- [6360/6360 new CRC objects](../crc/tests/results_shared_policies_arm_2026-09-05.json),
  all 106 named GNU AArch32 CPU targets, Os/O2/O3, little/big endian and strict
  alignment codecs. Real nm/objdump inspections, not execution on 106 boards.
- Shared scalar/protocol matrix: 96 scalar, 60 protocol and 30 COBS objects.
- Focused endpoint/layout and CRC emission guards passed.
- `wire/tests/check_shared_crc.sh` passed on native GCC and GNU ARM Cortex-M7:
  COBS and RTU compiled in two translation units link **one** 512-byte CRC16
  lookup for Table, and no lookup for Bitwise or NoCrc, at Os/O2/O3.

The ARM matrix is not AArch64, all optional ISA/ABI combinations, or physical
big-endian board execution. The matrix scope is detailed in
[the ARM audit](../crc/tests/ARM_AUDIT.md). NoCrc inlines away in production;
its separately callable hardware benchmark still measures call/harness cost.

## COBS on NUCLEO-H7S3L8

Board: H7S3 rev Y, 600 MHz, caches enabled, ST-Link serial
`002A001F3033510135393935`, COM6. Builds use Os, no LTO, `Uart<128,8>`,
eight RX and two TX packet blocks. No UART production code was changed.

All three configurations passed vectors, exact fault classification,
backpressure self-test, pool exhaustion and stress at **115200, 1M, 3M, 6M
and 10M**. Gap recovery was exercised at the lowest and highest baud. Each
matrix also restored and smoke-tested its own 115200 image.

| Configuration | Records | 1M stress frames | Useful bytes | Instrumented CPU |
|---|---:|---:|---:|---:|
| [CRC16 Bitwise / 253](../cobs/tests/hardware/h7s/results_crc_default_2026-09-05.jsonl) | 29 | 3505 | 423404 | 2.608745% |
| [CRC16 Table / 253](../cobs/tests/hardware/h7s/results_crc_table_2026-09-05.jsonl) | 28 | 3449 | 416507 | 1.058586% |
| [Legacy NoCrc / 1024](../cobs/tests/hardware/h7s/results_legacy_shared_storage_2026-09-05.jsonl) | 28 | 1229 | 380545 | 0.540598% |

These are 5-second windowed echo workloads, not a guaranteed UART line-rate
load or a fixed number of identical frames. NoCrc/1024 uses a different
payload mix, so its CPU row is not a controlled CRC-only A/B comparison.
The default also passed a 10-second, window-7 10M stress run: 28,255 frames
and 3,413,204 payload bytes, 11.148% instrumented CPU. Tested baud does not
mean the ST-Link/host achieved continuous line saturation.

The hardware harness now has an explicit CRC/maximum-payload configuration
and an independent Python peer for both H1/H2 and CRC/NoCrc. HELLO verifies
the selected geometry. Harness protocol version 2 returns STATS in two
bounded pages of the **same captured snapshot**, allowing the real default
253-byte payload to be tested without silently enlarging its Format. This
harness version is not a COBS on-wire negotiation feature.

## RTU on the same board

[127 fresh records](../modbus/rtu/tests/hardware/h7s/results_shared_storage_2026-09-05.jsonl)
cover all nine policies at 115200 and 1M: vectors, corruptions/recovery,
backpressure, pool exhaustion, isolated CRC measurements, stress and paced
traffic. The final record is a restored standard Bitwise/115200 smoke test.
Each record includes the exact flashed binary manifest and disassembled CRC
probe, with its source hashes and lookup bytes.

The table uses 246 input bytes for isolated calculator cost. CPU is the
5-second paced 300-frame/s **whole UART/RTU instrumented workload**, not
the cost of the isolated CRC call:

| Policy | Calculator cycles/byte | Paced CPU | Lookup bytes | Static probe instructions |
|---|---:|---:|---:|---:|
| NoCrc | 0; harness is 12.25 cycles/call | 0.166452% | 0 | 2 |
| CRC8 Bitwise | 50.133130 | 0.608206% | 0 | 29 |
| CRC8 Table | 6.084350 | 0.223767% | 256 | 13 |
| CRC16 Bitwise | 64.137195 | 0.722560% | 0 | 27 |
| CRC16 Table | 9.133130 | 0.249763% | 512 | 23 |
| CRC32 Bitwise | 43.133638 | 0.548924% | 0 | 28 |
| CRC32 Table | 9.129573 | 0.256373% | 1024 | 25 |
| CRC64 Bitwise | 49.149390 | 0.602498% | 0 | 33 |
| CRC64 Table | 9.141768 | 0.251192% | 2048 | 29 |

Static instructions are the compiled probe plus any directly outlined CRC
functions, not retired instructions per packet. All linked probes have no
external helper calls. Measured CPU sums the selected DWT scopes and divides
by the board's observation window; instrumentation and preemption effects
remain, so it is not a universal processor-utilization guarantee. See the
[benchmark methodology](../modbus/rtu/tests/hardware/h7s/CRC_BENCHMARK.md).

RTU still requires one complete candidate. CRC does not repair a split input,
and NoCrc cannot identify one. These fresh tests do not broaden the documented
burst-adapter guarantee to arbitrary RTU timing or high-baud host behavior.

## End state

The board's original 64 KiB internal-flash image was saved before these tests
and restored afterward with programmer read-back verification. The backup
and build logs stay in ignored test output. No option bytes or external flash
were changed. Source changes and fresh evidence are not committed or pushed
by this continuation; the unrelated user scratch document was left alone.
