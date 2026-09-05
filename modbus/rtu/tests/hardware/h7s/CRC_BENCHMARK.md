<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# Nine-policy CRC benchmark on NUCLEO-H7S3L8

This harness compares NoCrc and Bitwise/Table implementations of CRC8, CRC16,
CRC32 and CRC64. It measures the production header and RTU/UART integration,
not a separate hand-written approximation of the algorithms.

## Accepted results, 2026-09-05

**82/82 records passed.** Across the 36 stress/paced windows the board echoed
143,249 exact frames / 12,220,789 function-data bytes, with zero unexpected
RTU/UART/pool failures. All nine policies also passed the functional,
corruption/no-integrity, ownership and exhaustion suites.

[Raw results with image manifests and disassembly](results_crc_all_2026-09-05.jsonl)
are the source for every number below. The offline verifier rechecks all
sample checksums, geometry identities, accounting and derived formulas.

### Calculator, identical 246-byte input

Median of nine samples; each sample contains eight calls. Cycles are gross,
including the small benchmark overhead described below.

| Policy | Cycles/call | Cycles/byte | Calculation MiB/s | Static CRC instructions | Lookup ROM bytes |
|---|---:|---:|---:|---:|---:|
| NoCrc | 12.25 (floor) | not applicable | not applicable | 2 | 0 |
| CRC8 Bitwise | 12,332.75 | 50.133 | 11.414 | 29 | 0 |
| CRC8 Table | 1,496.625 | 6.084 | 94.053 | 13 | 256 |
| CRC16 Bitwise | 15,777.75 | 64.137 | 8.922 | 27 | 0 |
| CRC16 Table | 2,246.75 | 9.133 | 62.652 | 23 | 512 |
| CRC32 Bitwise | 10,611.00 | 43.134 | 13.266 | 28 | 0 |
| CRC32 Table | 2,246.00 | 9.130 | 62.672 | 25 | 1,024 |
| CRC64 Bitwise | 12,090.75 | 49.149 | 11.642 | 33 | 0 |
| CRC64 Table | 2,248.875 | 9.142 | 62.592 | 29 | 2,048 |

The table speedups for the same width/input are approximately 8.24x, 7.02x,
4.72x and 5.38x. CRC width alone does not rank execution cost: these are
different algorithms and GCC-generated loops, not a generic "wider is slower"
rule. NoCrc's standalone probe is just MOVS r0,#0 and BX lr; production
inlining removes it.

The longest observed IRQ-off sample was 131,342 cycles, about 219 microseconds.
All eight input lengths and every raw timing sample are retained in the JSONL.

### Integrated UART/RTU cost, extended 15-second windows

Every paced policy echoed **4,499 frames / 384,051 useful bytes**. Actual rates
were 299.902–299.923 frames/s; maximum host schedule lateness was 4.584–5.701 ms.
The CPU percentages below are instrumented-path costs, not total CPU usage.

| Policy | Cycles/frame at paced rate | Observed paced CPU | Normalized to exactly 300 fps | Max-cadence frames/s | Max-cadence CPU |
|---|---:|---:|---:|---:|---:|
| NoCrc | 3,357 | 0.167% | 0.168% | 515.44 | 0.287% |
| CRC8 Bitwise | 12,357 | 0.614% | 0.618% | 502.84 | 1.029% |
| CRC8 Table | 4,388 | 0.218% | 0.219% | 503.56 | 0.366% |
| CRC16 Bitwise | 14,805 | 0.736% | 0.740% | 495.37 | 1.215% |
| CRC16 Table | 5,174 | 0.257% | 0.259% | 499.65 | 0.428% |
| CRC32 Bitwise | 10,992 | 0.547% | 0.550% | 494.01 | 0.900% |
| CRC32 Table | 5,036 | 0.250% | 0.252% | 494.77 | 0.413% |
| CRC64 Bitwise | 12,211 | 0.607% | 0.611% | 474.79 | 0.960% |
| CRC64 Table | 5,056 | 0.251% | 0.253% | 478.79 | 0.401% |

CRC16 Table reduced observed paced path CPU by about 65.1%, while its
calculator was about 7x faster. The UART, ownership, allocation-pool,
copying and packet-building work remains, so integrated CPU cannot improve
by the full calculator-only factor.

All images have **12 bytes data and 6,864 bytes BSS**; selecting a Table adds
no static RAM. The linked text increase from Bitwise to Table is 244/504/
1,016/2,044 bytes for CRC8/16/32/64 respectively. These are whole-harness
differences; lookup size itself is the exact value in the first table.
NoCrc's whole instrumented image has 23,008 text bytes.

Finally the runner rebuilt, flashed, verified and smoke-tested default
CRC16 Bitwise at 115200. The restored load-image SHA-256 is
149c952891c53878acf49bdca7ec468a34c496a27d2c0676cf614c64929e06c3.
It has 23,176 text bytes, 12 data bytes and 6,864 BSS bytes.

## What each measurement means

Three different quantities must not be conflated:

- **Live calculation cycles:** Cortex-M7 DWT CYCCNT, measured on the board.
- **Instrumented path CPU percentage:** cycles in selected RTU/UART/IRQ scopes,
  divided by the board's observation interval.
- **Static instructions:** decoded instructions in the CRC entry point and
  any directly called shared CRC implementation in the exact flashed ELF.

Static instruction count is not the number of instructions executed per byte.
Loops execute many times, and a Cortex-M7 cycle is not synonymous with an
instruction. The DWT event counters are not an INST_RETIRED counter; this
harness does not fabricate a retired-instruction count by subtracting their
values from CYCCNT. See the official
[CMSIS DWT register definition](https://arm-software.github.io/CMSIS_5/Core/html/structDWT__Type.html).
Dynamic instruction retirement was **not measured**.

## Target and calculation workload

- NUCLEO-H7S3L8, STM32H7S3L8 Rev Y, Cortex-M7, 600 MHz.
- USART3/GPDMA through ST-Link VCP, 1,000,000 baud, 8N1.
- Uart<256,4> and Endpoint<Pool<8,2>, Policy>.
- GNU Arm C++20, -Os, no LTO, no exceptions/RTTI; the manifest records the
  complete compiler version and the load-image SHA-256.
- I-cache and D-cache enabled, verified through the returned SCB CCR.
  Code and tables are in internal FLASH; the calculation input is on the
  DTCM stack in the existing Cube linker layout.
- Identical input for every policy: byte i = (i * 37 + 0xA5) & 255.
- Lengths **0, 1, 8, 32, 64, 128, 246, 256**. These are calculator input
  lengths, independent of a particular RTU policy's maximum function data.
- For each length: nine independent samples, eight calls per sample.
  Report the median and preserve all raw samples and min/max.

One warm-up call precedes each sample. A sample disables interrupts, brackets
the repeated calls with CYCCNT reads, and restores the original PRIMASK.
Every returned window must be positive and less than 600,000 cycles (1 ms).
The PC independently verifies both the checksum and the sum/XOR result used
to keep every measured call observable.

The stable, non-inlined probe has a compiler memory barrier and noipa.
The result-mixing loop has a compiler barrier before the second timer read.
The empty compiler barriers do not add MCU instructions. Reported cycles are
**gross**: they include call/return, loop control, 64-bit result mixing
and amortized counter-read overhead. They are not a baseline-subtracted
promise for arbitrary production call sites.

NoCrc does not read the input. Its measured cycles are the harness floor,
not checksum throughput; calculation_mib_s is therefore null. When inlined
into the protocol, its calculation/store/load disappear. The standalone
benchmark must still make an observable call so its floor can be measured.

At -Os, GCC can share a CRC implementation between the Endpoint and the
benchmark entry point. inspect_image.py follows these direct CRC calls and
counts the whole reachable implementation, not just its one-branch wrapper.
It rejects indirect/external helper calls. This is static compile-time
selection, not virtual dispatch. The decoded counts exclude literal-pool
data and relocation records; alignment NOPs, if present in the displayed
function body, count as decoded instructions.

## End-to-end workload and CPU accounting

The host precomputes 1,100 independent-oracle wire images **before** resetting
board metrics. Every policy gets the same function-data distribution:
0, 1, 2, 31, 32, 63, 64, 127, 128, 245, 246 bytes. Python CRC calculation and
payload generation are absent from the timed loop. Every response must equal
the complete precomputed request, including the policy's trailer.

Each policy runs two traffic modes for 5 seconds and again for 15 seconds:

- Maximum host-paced request/echo rate (stress suite).
- Target **300 frames/s** (paced suite), scheduled from an absolute monotonic
  timeline. Actual achieved rate and maximum lateness are recorded.

The 300-frame target is an average rate. A 256-byte request/response pair alone
takes longer than 1/300 s at this baud, so the scheduler catches up on shorter
frames; it does not claim zero per-packet jitter. Trailers have different
widths, so on-wire byte counts necessarily differ even with equal useful data.
The maximum-rate test is bounded by UART/VCP/host cadence; it is not the maximum
CRC calculator throughput.

The exact existing counter formula is:

    C = usart_irq + rx_dma_irq + tx_dma_irq
      + uart_slow + packet_process + rtu_tx_release

    observed CPU % = 100 * C / (600,000,000 * window_ms / 1000)
    cycles/frame  = C / echoed_frames
    300-fps normalized CPU % = 100 * cycles/frame * 300 / 600,000,000

rtu_receive is already inside uart_slow, so it is not added again.
packet_process includes dequeue, Message construction, TX integrity
calculation, send and packet release. The snapshot includes one final STATS
request's RX work, but not construction/transmission of its response.
The 80 ms reset-settle interval is included in the board observation window.

These are instrumented-path percentages, **not total chip utilization**:
busy main-loop polling, all instrumentation overhead, exception
entry/exit and unrelated work are not fully included. An IRQ interrupting a
thread scope can also be present in both measured elapsed scopes; this harness
does not subtract such overlap. The counters are useful for a same-target,
same-harness comparison, not a complete scheduler/idle-time profile.

## Correctness gates

Every policy must pass the same independent Python oracle and HELLO geometry
check before measurement. The live suites additionally require:

- 31 payload/pattern vectors, including an exact 256-byte ADU;
- four detected corruptions with immediate recovery for every CRC policy;
- for NoCrc, four changed frames accepted and echoed verbatim, with zero
  CRC errors (no integrity protection is claimed);
- TX Busy retains ownership and a third live TX allocation fails cleanly;
- a 16-frame flood into eight RX blocks retains exactly the first eight;
- exact echo counts, data-byte counts, DMA/callback counts, settled ownership,
  and zero unexpected RTU/UART/pool failures during healthy traffic.

The physical ADU remains 256 bytes; useful data is 254/253/252/250/246 bytes
for NoCrc/CRC8/CRC16/CRC32/CRC64. Only the default CRC16 model/codec is standard
Modbus RTU. The other choices intentionally exercise the private,
policy-selected RTU-like format; both peers must agree on it.

## Reproduce

Use the existing local Cube scaffold described in the
[hardware guide](README.md). Run the independent PC tests first:

    python -B modbus/rtu/tests/hardware/h7s/test_oracle.py

Recheck the accepted evidence without a board:

    python -B modbus/rtu/tests/hardware/h7s/verify_results.py modbus/rtu/tests/hardware/h7s/results_crc_all_2026-09-05.jsonl

Then run the full matrix from a PowerShell session allowed to execute the
repository's test script:

    & ./modbus/rtu/tests/hardware/h7s/run_matrix.ps1 -Port COM6 -StLinkSerial <STLINK_SERIAL> -BaudRates @(1000000) -CrcPolicies @('nocrc', 'crc8-bitwise', 'crc8-table', 'crc16-bitwise', 'crc16-table', 'crc32-bitwise', 'crc32-table', 'crc64-bitwise', 'crc64-table') -Optimization Os -Lto 0 -StressSeconds 5 -ExtendedSeconds 15 -Output modbus/rtu/tests/hardware/h7s/results_crc_all_new.jsonl

The runner inspects each ELF **before** flashing that same file, records the
binary SHA-256, private-table symbols, complete CRC disassembly and image
text/data/BSS sizes, and keeps ELF/bin/map/manifest snapshots in the ignored
Cube output directory. Results include the manifest on every row. Image size
is the size of this instrumented harness, not the isolated library footprint.

Unless explicitly requested otherwise, the finally path rebuilds, verifies
and smoke-tests default Bitwise at 115200 even after a test failure.
The bitwise and table CLI aliases still select crc16-bitwise and crc16-table.

For other CPUs, use the separate
[ARM code-generation audit](../../../../../crc/tests/check_arm_matrix.py).
Those are compile/disassembly checks, not live measurements on other boards.
