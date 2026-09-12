<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Heap storage and peripheral CRC on NUCLEO-H7S3L8

This measurement compares two independent choices: packet allocation
(`wire::Pool` / `wire::Heap`) and checksum calculation (NoCrc, CRC16 Bitwise,
CRC16 Table, STM32 peripheral). At acquisition, COBS, RTU, UART, shared
storage and CRC core implementations were unchanged. The added peripheral policy is an adapter,
not a hardware dependency in either protocol.

Baseline outcome: at a known 250-byte size, Heap added roughly 0.6-6.8% to the endpoint
echo across these policies. Incremental growth costs much more. At 250 input
bytes the hardware CRC is 19.28x faster than Bitwise and 3.31x faster than
Table; these are CRC-only speedups, not whole-protocol speedups.

**Later correction:** current `wire::Heap` uses `malloc/free`; its real OOM
refusal and recovery are [verified separately](../src/wire/tests/hardware/h7s/heap_crc/recovery/README.md).
The numeric Heap tables below remain the earlier nothrow-new baseline;
post-fix Heap throughput/CPU was not remeasured. The CRC16 calculation paths
did not change; a later CRC8-only compile-time cleanup removes an MSVC warning.
TX memory still must be DMA-accessible. Direct global nothrow new
still aborts on OOM in this nano runtime; that global implementation was not replaced.

<!-- heap-crc-measurements:start -->
## Measured results

Source: [complete live receipt](../src/wire/tests/hardware/h7s/heap_crc/results_2026-09-12/session.json). Tables are regenerated and checked by `report.py --check-doc`.

### Packet allocation: known-size 250-byte pseudorandom body

Endpoint echo only, excluding UART. Time includes receive, transmit construction and release. One microsecond is 600 core cycles on this board.

| Protocol | CRC | Pool cycles | Heap cycles | Heap delta |
|---|---|---:|---:|---:|
| COBS | NoCrc | 6194.0 | 6358.2 | +2.65% |
| COBS | Bitwise | 32843.0 | 33028.2 | +0.56% |
| COBS | Table | 11075.8 | 11233.2 | +1.42% |
| COBS | STM32 | 7874.0 | 8072.2 | +2.52% |
| RTU | NoCrc | 2701.2 | 2884.5 | +6.78% |
| RTU | Bitwise | 29340.0 | 29534.8 | +0.66% |
| RTU | Table | 6979.0 | 7170.2 | +2.74% |
| RTU | STM32 | 4179.0 | 4368.5 | +4.53% |

### Growth and fragmentation with the STM32 CRC

Cycles per endpoint echo, pseudorandom body. Grow means a zero-capacity hint and 16-byte appends; it includes the extra append calls as well as allocations/copies. Fragmented means the specified 48-live-block layout, still with a known-size hint.

| Protocol | Body bytes | Pool known | Heap known | Pool grow | Heap grow | Heap fragmented |
|---|---:|---:|---:|---:|---:|---:|
| COBS | 8 | 1786.0 | 1973.0 | 1820.0 | 2244.8 | 2032.2 |
| COBS | 250 | 7874.0 | 8072.2 | 9168.0 | 14141.8 | 8139.5 |
| COBS | 1024 | 27237.0 | 27371.0 | 34321.0 | 45072.0 | 28196.0 |
| RTU | 8 | 1501.0 | 1691.5 | 1523.0 | 1944.5 | 1750.5 |
| RTU | 250 | 4179.0 | 4368.5 | 5281.0 | 8615.5 | 4452.8 |
| RTU | 1024 | 12581.0 | 12737.0 | 17972.0 | 27543.0 | 13580.0 |

### CRC calculation alone

Cycles per calculation including setup/reset, offset 0, warm AXI SRAM, medians of nine samples from the COBS images. NoCrc performs no checksum calculation and therefore has no raw-calculator row. Other offsets and all sizes remain in the receipt.

| Input bytes | Bitwise | Table | STM32 | Bitwise / STM32 | Table / STM32 |
|---|---:|---:|---:|---:|---:|
| 0 | 30.31 | 26.19 | 61.25 | 0.49x | 0.43x |
| 1 | 88.31 | 41.19 | 69.25 | 1.28x | 0.59x |
| 4 | 247.31 | 67.19 | 69.25 | 3.57x | 0.97x |
| 8 | 459.31 | 103.19 | 77.25 | 5.95x | 1.34x |
| 32 | 1731.31 | 319.19 | 137.25 | 12.61x | 2.33x |
| 128 | 6819.31 | 1183.19 | 377.25 | 18.08x | 3.14x |
| 250 | 13285.31 | 2281.19 | 689.25 | 19.28x | 3.31x |
| 1024 | 54324.00 | 9262.00 | 2632.00 | 20.64x | 3.52x |
| 4096 | 217140.00 | 36910.00 | 10312.00 | 21.06x | 3.58x |

### Live UART: 250-byte bodies at nominal 1 Mbaud

Pseudorandom and zero-filled frames alternate. Values are medians of two windows. The rate column spans all four Pool/Heap windows in that row. CPU is measured communication work only, at the observed VCP-paced delivery rate, not full system load.

| Protocol | CRC | Pool CPU | Heap CPU | Pool cycles/echo | Heap cycles/echo | Delivered kB/s |
|---|---|---:|---:|---:|---:|---:|
| COBS | NoCrc | 0.288% | 0.294% | 9139.6 | 9332.8 | 47.20-47.30 |
| COBS | Bitwise | 1.109% | 1.117% | 35832.0 | 36098.1 | 46.41-46.45 |
| COBS | Table | 0.436% | 0.444% | 13999.9 | 14226.2 | 46.66-46.83 |
| COBS | STM32 | 0.338% | 0.350% | 10844.5 | 11212.0 | 46.73-46.86 |
| RTU | NoCrc | 0.172% | 0.178% | 5483.9 | 5635.3 | 47.01-47.38 |
| RTU | Bitwise | 1.006% | 1.014% | 32522.8 | 32760.6 | 46.41-46.45 |
| RTU | Table | 0.312% | 0.320% | 10043.1 | 10211.6 | 46.51-46.98 |
| RTU | STM32 | 0.224% | 0.242% | 7189.1 | 7710.8 | 46.68-47.03 |

### Live UART: short/mixed traffic with the STM32 CRC

Different delivery rates make CPU percentages across these cases incomparable on their own.

| Protocol | Body pattern | Pool CPU | Heap CPU | Pool cycles/echo | Heap cycles/echo | Pool / Heap kB/s |
|---|---|---:|---:|---:|---:|---:|
| COBS | short8 | 1.571% | 1.664% | 4310.7 | 4621.2 | 17.49 / 17.28 |
| COBS | mixed | 0.479% | 0.501% | 6918.7 | 7247.2 | 43.37 / 43.24 |
| RTU | short8 | 1.580% | 1.763% | 4070.9 | 4723.8 | 18.63 / 17.91 |
| RTU | mixed | 0.365% | 0.410% | 5279.1 | 5903.1 | 43.32 / 43.54 |

Full session: **8 images, 5,184 endpoint timing records, 1,728 raw CRC timing records, 16,400 hardware/software CRC comparisons, 96 UART windows, 184,181 exact echoes**. No recorded protocol, allocation-lifecycle or UART errors. The original 65,536 flash bytes were restored and independently read back; backup/read-back SHA-256: `a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456`.
<!-- heap-crc-measurements:end -->

## Use the hardware calculator

```cpp
#include "adapters/stm32/Crc16.h"
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"

extern CRC_HandleTypeDef hcrc;

using Integrity = crc::stm32::Crc16;
using Memory = wire::Heap;             // or wire::Pool<8, 2>
using Cobs = cobs::Endpoint<Memory, cobs::Format<Integrity>>;
using Rtu = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<Integrity>>;

Cobs cobs_link{Integrity{hcrc}};
Rtu rtu_link{Integrity{hcrc}};
```

The endpoints pass the same policy object to RX and TX. Switching to Table
or Bitwise changes `Integrity`, not application packet/message handling or
wire bytes. `wire::Heap` is already the default memory strategy in both
endpoints; selecting it does not require a new protocol implementation.

The constructor only retains a handle; static construction before hardware
initialization is allowed. The handle must remain alive, its peripheral
clock must be enabled before calculation, and the STM32 CRC IP must support
programmable 16-bit polynomial and input/output reversal. Live validation
here covers H7S3, not all STM32 families.

The adapter programs CRC-16/MODBUS on every calculation: normal polynomial
`0x8005` (reflected equivalent `0xA001`), initial value `0xFFFF`, 16-bit
width, per-byte input bit reversal, output reversal. Its inherited codec
stores the result in two bytes, little-endian. Configuration, reset and final
read are included in the reported hardware timings. The library does not
validate arbitrary user calculators at runtime; equivalence checks belong
only to this test harness.

ST's [16-bit reversed-CRC example](https://raw.githubusercontent.com/STMicroelectronics/STM32CubeL4/master/Projects/NUCLEO-L476RG/Examples/CRC/CRC_Data_Reversing_16bit_CRC/Src/main.c)
documents the result in the low 16 bits. The H7S-specific equivalence here
is established separately by live comparisons, not inferred from the L4 example.

Four-byte chunks are loaded with alias-safe `memcpy`, reordered to the
peripheral byte order, then written as words. The remaining 0..3 bytes use
byte writes. Input can have any byte alignment and may be in CPU-readable
DTCM: this policy feeds the peripheral from the CPU, not DMA. It allocates
no memory, keeps no input pointer, and adds one handle pointer as policy
state (4 bytes on this target), with no CRC table.

**Peripheral ownership is explicit.** COBS and RTU may use the same CRC
instance sequentially in one execution context. No other task, ISR or DMA
may touch that instance during a calculation. There is no hidden mutex or
IRQ masking in the policy. Every call overwrites the register configuration;
it does not preserve a preceding calculation or update HAL's cached `Init`
fields. An application mixing HAL calculations must configure them as needed.

## Heap behavior and placement

Current `wire::Heap` uses `std::malloc/std::free` and handles a returned
`nullptr` through the existing Message/Endpoint API. It bypasses global
new/delete replacements and new_handler. It does not convert failed
allocation into successful sends or promise fixed execution time, freedom
from fragmentation, or thread safety. Retained Packets keep their RX
allocations alive until the final reference is released; a DMA TX borrow
keeps its allocation alive until completion is polled.

**Original, hardware-confirmed toolchain OOM limitation:** in
the flashed GNU Arm 14.3.1 `libstdc++_nano` image, the nothrow allocation
operator is a tail branch to the ordinary allocation operator. After a
failed `malloc`, that operator calls `get_new_handler()` and then `abort`
when there is no handler. Thus in this exact build heap exhaustion does
not follow the library's intended nullable-allocation path. Initially found
in disassembly, it is now reproduced by a separate
[live OOM diagnostic](../src/wire/tests/hardware/h7s/heap_crc/oom/README.md):
direct nothrow new plus COBS/RTU TX, RX and message growth reach the real
`abort` and `_exit(1)` in all seven cases. `malloc` returns null normally;
Pool works with Heap exhausted, and Heap recovers after the control test
frees the held blocks. Observers report then forward to the real abort/exit;
silence is not used as the sole proof. The earlier throughput session itself
used successful allocations, so its timings remain valid.
The subsequent fix changes only the shared Heap backend to malloc/free.
Its [live recovery record](../src/wire/tests/hardware/h7s/heap_crc/recovery/results_2026-09-12/session.json)
proves nullable allocation, retained-packet lifetime and same-message retry
after failed growth in both protocols. No global allocation operators were
replaced; direct global nothrow new remains an intentionally failing control.

The [exact disassembly excerpt](../src/wire/tests/hardware/h7s/heap_crc/results_2026-09-12/disassembly_excerpt.txt)
retains both allocation operators, the abort path and the hardware CRC loop,
with hashes of the complete measured ELF and disassembly.

If the payload size is known, provide the existing capacity hint:

```cpp
auto cobs_message = cobs_link.make_message(payload.size());
auto rtu_message = rtu_link.make_message(unit, function, function_data.size());
```

This avoids intermediate growth allocations and copies. Unknown sizes are
still supported through the existing geometric growth policy and
`reserve()`. A Pool grants its full TX slab immediately; a Heap grants the
requested physical size, so repeated appends can produce different costs.

The user's current Cube linker puts `_end` and the ordinary newlib heap in
DTCM, while ordinary static Pool objects are in AXI SRAM. DTCM TX pointers
cannot be read by this UART DMA. The test therefore supplies a **test-only
`_sbrk` backed by 128 KiB of AXI SRAM**, without replacing newlib-nano
`malloc/free` or `operator new/delete`. Both strategies use the same memory
bank; live addresses are recorded. The original generated linker/sysmem
files are not edited. This test arena is not installed into the application:
production heap-backed UART TX also needs DMA-visible backing memory.

## Measurement scope

- NUCLEO-H7S3L8 / Cortex-M7: 600 MHz core, 300 MHz HCLK, I/D caches on.
- GNU Arm 14.3.1, `-Os`, no LTO, bare metal, actual newlib-nano allocator.
- Each of eight images contains both memory strategies; execution order
  alternates between paired core samples. No cross-bank allocator comparison.
- Core echo: receive + pop, make + append + send, completion + release.
  Byte comparisons, warm-up and serial printing are outside the summed
  phases. All samples check equal live heap allocation before and after.
- Six sizes, two patterns (pseudorandom / zeros), three allocation scenarios
  (known size / 16-byte incremental appends / synthetic fragmentation),
  nine samples per strategy: 648 timing records per image.
- Raw CRC: identical bytes for every algorithm, sizes 0..4096, offsets 0/1,
  nine samples. Per-call timings include loop/timestamp overhead; no
  speculative subtraction of an empty-loop baseline.
- UART: `Uart<256,4>`, nominal 1 Mbaud, one outstanding request/echo, 8-byte,
  250-byte and mixed bodies, two windows per memory strategy and case.
  This is actual VCP-paced exchange, not sustained one-way line saturation.
- Framed RTU uses benchmark private function `0x41` and its BE16 data-length
  prefix. Standard-size cases fit ADU 256; the 1024-byte body deliberately
  uses private MaxAdu 1030. COBS uses max payload 253 or a wider 1024 format.

Core total is the median of each sample's `(RX + TX + release) / iterations`,
not the sum of three independent medians. Ratios match protocol, CRC,
payload, pattern and scenario. CPU is computed independently from raw
telemetry as:

```text
work_cycles = delta(service_cycles) + delta(USART/RX-DMA/TX-DMA IRQ cycles)
communication_CPU_percent = 100 * work_cycles / (core_hz * MCU_elapsed_seconds)
cycles_per_echo = work_cycles / verified_echo_count
delivered_payload_B_per_second = verified_payload_bytes / MCU_elapsed_seconds
```

Service scopes subtract those measured IRQ cycles to avoid counting
preemption twice. SysTick and the outer idle/wake loop are not a
whole-system CPU meter. Small snapshot-control boundary costs remain in the
integration deltas. Do not compare percentages at different delivered rates
as though the input workload were identical; cycle cost per echo is also
reported. Nine warm microbenchmark samples and one specified fragmented
layout are not worst-case execution-time or long-running fragmentation proofs.

## Evidence

The [harness and commands](../src/wire/tests/hardware/h7s/heap_crc/README.md)
describe raw records, independent byte oracles and restoration safeguards.
The [initial hardware probe](../src/wire/tests/hardware/h7s/heap_crc/results_probe_2026-09-12/session.json)
is separate from the [full matrix](../src/wire/tests/hardware/h7s/heap_crc/results_2026-09-12/session.json).

The original post-run ELF checker incorrectly treated GNU `nm` weak-object
type `V` as non-read-only and rejected the Table images. The corrected check
requires all of: a 512-byte symbol, an address inside boot flash, and the
matching `.rodata` input section in the linker map. Negative tests reject
RAM, `.data`, wrong extent, duplicate tables and tables in non-Table images.
All recorded images then passed independent verification, including every
saved UART transfer. Firmware and measurements were not changed or retried.
The exact checker bytes used during acquisition are preserved under
`verifier_history/63c8c69406c9409b3a77407ed8f4d683aca69befe085b14c697f02bc40f876c7.py`,
authenticated against the receipts' original SHA-256; receipts were not rewritten.

Compile-only checks additionally pass on M7 at `-Os/-O2/-O3`, with default
and strict alignment. The flashed default-alignment CRC loop uses
`LDR / REV / STR` and a byte-write tail, with no helper calls in that loop.
This compiler uses a `memcpy` helper under `-mno-unaligned-access`; the
default-alignment timing is not a measurement of the strict build. Static
assertions prove Hardware and Bitwise have identical Storage, Message and
Packet types for both protocols. Host wire/CRC/adapter suites also passed
under WSL with ASan+UBSan and their release variants.

Related contracts: [storage](STORAGE.md), [CRC policies](../src/crc/README.md),
[wire protocol](PROTOCOL.md), [earlier protocol comparison](PROTOCOL_COMPARISON.md),
[earlier hardware API-parity regression](HARDWARE_API_PARITY_2026-09-12.md).
