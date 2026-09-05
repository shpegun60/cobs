<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# CRC AArch32 code-generation audit, 2026-09-05

**6,360/6,360 objects passed**, covering all **106 named CPU targets** accepted
by the installed GNU Arm compiler. This is compilation and inspection of real
machine-code objects, not execution on 106 boards.

[Raw case-level results](results_arm_matrix_2026-09-05.json) include the full
CPU list, source hashes, each object's/disassembly's SHA-256, table bytes and
static instruction counts.

Compiler: GNU Tools for STM32 14.3.rel1.20251027-0700,
arm-none-eabi-g++ 14.3.1 20250623.

## Matrix

| Dimension | Coverage |
|---|---|
| Policies | NoCrc; CRC8, CRC16, CRC32, CRC64, each Bitwise and Table |
| Optimization | -Os, -O2, -O3 |
| CPU byte order | little-endian and big-endian |
| Ordinary objects | 106 CPUs × 9 policies × 3 optimizations × 2 byte orders = 5,724 |
| Extra strict-alignment objects | 106 CPUs × 3 optimizations × 2 byte orders = 636 |
| Codec probes in every object | store/load × little/big wire order × 8/16/32/64 bits = 16 |
| NoCrc verifier in every object | complete generic verify path |

The M-profile group includes M0, M0+, M1 and their small-multiply variants;
M3, M4, M7; M23, M33, M35P, M52, M55, M85; and STAR-MC1.
R-profile includes R4, R4F, R5, R7, R8, R52 and R52+.
The remaining compiler targets include Cortex-A, Neoverse, classic ARM7/9/11,
StrongARM/XScale and the other aliases/composite tuning targets listed in the
raw report. A composite tuning target is one compiler configuration, not an
additional physical board.

M-profile targets use Thumb; other targets use ARM state. All use soft-float,
C++20, strict conversion/shadow warnings as errors, no exceptions/RTTI and no
LTO. The strict-alignment objects add -mno-unaligned-access. All 16 codecs are
present in those objects even though their selected calculator is NoCrc.

This deliberately bounded claim does **not** cover AArch64, all optional
FP/SIMD/MVE/ABI configurations, every alternate ISA state of every CPU,
linker/OS combinations, or live silicon other than the separately tested M7.
Big-endian compiler acceptance also does not mean a particular board exposes
a usable big-endian boot configuration.

## What passed

- Every calculator probe is present and decodes into real instructions.
  None retains a BL/BLX helper call or a direct tail branch to an external
  function in these isolated objects.
- All 101,760 codec-probe inspections are helper-call-free and contain no
  runtime conditional branch. Endianness and wire width are compile-time
  choices.
- In all 6,360 objects the complete NoCrc verifier folds to constant true
  without memory access, a helper call or a conditional branch.
- Selected NoCrc calculation contains no memory access.
- All 3,816 non-table objects emit **zero CRC lookup bytes**, even though the
  source names all four Table types and checks that their classes are empty.
- Each of the 2,544 selected-table objects emits **exactly one read-only
  private class table**: 256/512/1,024/2,048 bytes for CRC8/16/32/64.

Representative disassembly was also read directly. For example, CRC64 on M0
uses two 32-bit halves with ADD/ADC and inline polynomial XORs, without a
64-bit runtime helper. M85 at -O3 uses its low-overhead loop instructions.
Strict-alignment M7 codecs use byte memory accesses; opposite wire order is
resolved through fixed shifts/reversals, not a runtime endian test.

This establishes the stated invariants, not a proof that GCC chose the
globally smallest or fastest instruction sequence. Static instruction counts
include decoded padding NOPs when present, but exclude literal-pool data and
relocations. The common calculation probe returns uint64_t, so narrow results
also include zero-extension to the probe ABI; do not equate its count with
the native-result benchmark entry point.

The initial runner incorrectly chose ARM state for STAR-MC1 from its name.
That configuration was rejected by GCC before compiling library code. The
runner now selects Thumb for it, and the entire final matrix was rerun.

## Reproduce and compare with silicon

    python -B crc/tests/check_arm_matrix.py

The compiler CPU list is queried, not copied into a potentially stale list.
Override the compiler with --cxx; its matching nm/objdump are used.
Object/ASM files remain in the ignored crc/tests/out/all-arm directory.
The default report is in that directory; --report selects a durable location.
An existing report is never overwritten.

For a short diagnostic subset:

    python -B crc/tests/check_arm_matrix.py --cpus cortex-m0 cortex-m7 cortex-m85 cortex-r52 cortex-a53 arm7tdmi --output crc/tests/out/subset

The [real H7S3 benchmark](../../modbus/rtu/tests/hardware/h7s/CRC_BENCHMARK.md)
separately measures all nine policies on Cortex-M7 and inspects each exact
flashed ELF. In a complete -Os image GCC can share an outlined CRC function
between callers, unlike the isolated probes here. That image inspector follows
and counts the direct CRC call graph instead of pretending its entry branch
is the entire implementation.
