<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Heap / Pool and STM32 CRC comparison

Paired measurements on NUCLEO-H7S3L8, GNU Arm 14.3.1, `-Os`, no LTO.
Production COBS, framed RTU, storage and UART are used unchanged. This is a
bare-metal benchmark using newlib-nano allocation, not FreeRTOS `heap_4`.

See [results and usage](../../../../../../doc/HEAP_AND_HARDWARE_CRC.md).
The [separate live OOM diagnosis](oom/README.md) confirms the nano-runtime
abort path; it does not alter or invalidate these successful-allocation timings.
The [malloc-backed Heap recovery follow-up](recovery/README.md) verifies the
fix. These timing records retain the earlier Heap implementation; do not
present them as post-fix Heap measurements.

## Run

Requires the local ignored H7S Cube project with CRC enabled and
`MX_CRC_Init()` before `bench_init()`, the existing UART benchmark IRQ hooks,
GNU Arm, Git Bash, CubeProgrammer, Python and pyserial. Only one process may
own the board/serial port. The runner backs up all 64 KiB of boot flash,
verifies every flashed image, and restores and reads back the entire backup
in `finally`. It refuses an existing output directory. Runtime failures are
not retried or filtered from evidence.

```powershell
python -B src/wire/tests/hardware/h7s/heap_crc/test_verify.py
python -B src/wire/tests/hardware/h7s/heap_crc/run.py `
  --port COM6 --serial 002A001F3033510135393935 `
  --probe --output path/to/new-probe
python -B src/wire/tests/hardware/h7s/heap_crc/run.py `
  --port COM6 --serial 002A001F3033510135393935 `
  --output path/to/new-full-session
python -B src/wire/tests/hardware/h7s/heap_crc/verify.py `
  path/to/new-full-session --local-images --summary path/to/derived-summary.json
python -B src/wire/tests/hardware/h7s/heap_crc/report.py `
  path/to/new-full-session --check-doc doc/HEAP_AND_HARDWARE_CRC.md
```

The default full session is exactly eight images: two protocols times
NoCrc/Bitwise/Table/peripheral. Both Pool and Heap execute inside each image.
`--probe` is explicitly only one hardware-CRC COBS image and no UART runs.
Each UART window lasts at least two host seconds; `--seconds` accepts 1..10.

## Evidence and gates

`session.json` retains every MCU report line, raw before/after UART telemetry,
counts, observed byte-stream SHA-256, source and ELF/bin/map/dis/nm hashes,
the exact matrix, backup and restoration hashes. Local ignored session files
also retain all transfer TX/RX hex, tool logs and image artifacts. The
independent verifier reconstructs wire bytes, rechecks telemetry, the exact
matrix, and derives metrics. `--local-images` additionally checks those local
artifacts and every individual UART transfer. Without local artifacts the
report does not claim to have rechecked their bytes.

One host-only correction followed the recorded run: GNU `nm` marks the
read-only weak Table object `V`, not `R`. Verification now checks the actual
flash address and `.rodata` map section as well as its 512-byte size. The
old acquisition-time checker is retained byte-exact in `verifier_history/`,
named and authenticated by its recorded SHA-256. No firmware or raw receipt
was changed to accommodate the corrected checker.

Hardware equivalence is checked for 8 offsets times every length 0..1024,
plus empty input. A host bit-level CRC oracle also reconstructs the combined
8,200-result digest. Raw timing checks lengths through 4096, including all
four byte-tail residues. All wire frames in the endpoint matrix have a
separate host oracle, including zero-filled and pseudorandom payloads.

Each image runs 36 endpoint cases: six sizes (0/8/32/128/250, plus a wider
1024-byte format), two patterns, three allocation scenarios. Nine samples
per memory strategy produce 648 records. Pool/Heap order alternates by
sample. IRQs are masked only for the short timing batch (required <1 ms),
not the serial reporting. Every sample must return to the same `mallinfo`
live allocated-byte total (`uordblks`); warm-up, oracle comparisons and reporting are outside
the timed phase sums. Each timing includes RX, TX and eventual release, so
neither newlib `free` nor packet lifetime is left out of the comparison.

The three allocation scenarios are known size, growth from zero by 16-byte
appends, and known size in a synthetic fragmented heap. Fragmentation holds
48 of 96 varied-size allocations and frees alternating entries. It is a
specified repeatable workload, not a bound on arbitrary allocator latency.

UART runs use `Uart<256,4>` at nominal 1 Mbaud, both memory strategies,
8-byte/250-byte/mixed traffic and two repetitions: 12 windows per image.
Each request is echoed before the next is sent. COBS uses its compact
format. Framed RTU uses private function `0x41` with a BE16 function-data
length prefix; neither timing-based RTU boundary detection nor standard
function semantics are implied by that private benchmark traffic.

The firmware reports cumulative service cycles and USART/RX-DMA/TX-DMA IRQ
cycles separately. IRQ cycles are removed from the inclusive service
window before adding them once to CPU work. SysTick and the outer idle/wake
loop are not a whole-system CPU meter. Control-snapshot boundary work is
small but not subtracted. Actual MCU elapsed time and delivered payload
rate accompany measured CPU, so nominal baud is never mistaken for a
continuous input stream. Every echo must match the independent wire oracle;
losses, UART errors or unexpected restarts fail the session.

## Allocator placement

`heap.cpp` supplies only `_sbrk`, backed by an aligned 128 KiB arena in AXI
SRAM. It does not replace `malloc`, `free` or either C++ allocation operator.
The original Cube `sysmem.c` is compiled with its `_sbrk` symbol renamed,
without editing the generated file. Static Pool endpoints occupy the same
AXI bank. The HELLO record verifies both addresses and clocks live.

The generated linker's ordinary `_end`/heap is in DTCM; DMA TX cannot use
those pointers. This harness placement is not silently installed into a
user application. Production integrations must arrange DMA-visible TX
storage according to [the storage contract](../../../../../../doc/STORAGE.md).
