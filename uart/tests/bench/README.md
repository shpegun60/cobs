<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# H7S hardware bench for `uart/Uart.h`

Measures the driver on real silicon (STM32H7S3L8, USART3 = ST-LINK VCP,
GPDMA1 Ch11 RX / Ch10 TX, SYSCLK 600 MHz, I/D-cache ON) with `uart_probe.h`
(`UART_ENGINE_PROBE=1`) plus three full-IRQ DWT counters in the CubeMX
USER CODE sections. This directory is the canonical copy; the files inside
the Cube project (`stm32_cube_test/h7s_cobs_test`, gitignored) are copies.

## What is measured

| counter              | written from                        | contains                                |
| -------------------- | ----------------------------------- | --------------------------------------- |
| `g_bench_usart_irq`  | `USART3_IRQHandler`                 | HAL + our Rx callback (IDLE path), TX TC, errors |
| `g_bench_rx_dma_irq` | `GPDMA1_Channel11_IRQHandler`       | HAL + our Rx callback (TC path)          |
| `g_bench_tx_dma_irq` | `GPDMA1_Channel10_IRQHandler`       | HAL TX DMA completion stage              |
| `probe.rx`           | inside `isrRxEvent`                 | ONLY the library's RX hot path           |
| `probe.slow`         | inside `proceedSlow`                | the periodic audit                       |
| `probe.tx_start`     | inside `send`                       | the TX start path                        |

**CPU accounting.** The hardware IRQ counters already CONTAIN `probe.rx`, so:

```
UART cycles = Δusart_irq + Δrx_dma_irq + Δtx_dma_irq + Δslow + Δtx_start
CPU% = 100 * UART cycles / (SystemCoreClock * window_s)
```

`probe.rx` is never added to the total — it exists for decomposition only:

```
continuous RX (TC-heavy):   HAL TC cost   ≈ rx_dma_irq.avg − probe.rx.avg
bursty RX (IDLE-heavy):     HAL IDLE cost ≈ usart_irq.avg  − probe.rx.avg
full-duplex:                usart_irq blends IDLE + TX-TC — compare scenarios,
                            don't try to split one IRQ vector
```

`tx_dma_irq` and the USART TX-TC interrupt are sequential stages of one
frame, not the same interval twice — both legitimately count.

Everything is a per-window delta: `R` zeroes all counters, `S` freezes and
reports. Lifetime averages are never used ("привиди попереднього
експерименту").

## Board-side protocol (uart_bench.cpp)

Single-byte commands, sent alone after a line pause (arrive as their own
1-byte chunk via IDLE): `R` reset window, `S` freeze + send report,
`T`/`t` TX generator on/off (64-byte 0x55 frames, back-to-back), and `1`/`3`
deferred live changes to 115200/3M baud.
Payload must avoid every command byte. `bench.py` builds an explicit
command-free 48-byte pattern, preserving the historical 1,536-byte continuous
writes and 96-byte bursts even after the live-baud commands were added.

Report is one line ending `\r\n`, fields `NAME=<total>cy/<calls>
avg=<cycles> max=<cycles>` for RX/USART/RXDMA/TXDMA/SLOW/TXSTART, then
`BYTES/CH/GAP/TXFR`, `WIN=<ms> CPU=<x.xxx>%`, `OVR/ERR/RST`.

## Wiring into the Cube project (already done; repeat after a regen if lost)

1. `Boot/Core/Inc/uart_bench.h`, `Boot/Core/Src/uart_bench.cpp` — copies of
   the files here.
2. `stm32h7rsxx_it.c` USER CODE sections: include `uart_bench.h`; in the
   USART3 / GPDMA1 Ch10 / Ch11 handlers read `DWT->CYCCNT` in section `0`,
   `bench_counter_add(...)` in section `1`. CubeMX regeneration keeps these.
3. `main.c` USER CODE: include `uart_bench.h`; `bench_init();` in
   `USER CODE 2`; `bench_loop();` in `USER CODE 3`.
4. CubeIDE, once: right-click project → **Convert to C++**; MCU G++ Compiler
   → dialect `gnu++20`, add `-fno-exceptions -fno-rtti`; include paths
   `${ProjDirPath}/../../../uart`, `.../libs/spsc`,
   `.../libs/spsc/src`, `.../libs/delegate`. Both SPSC paths are required:
   its public headers live under `src` and include the library-owned root
   `basic_types.h`.
5. **Optimization at a real level** (`-O2`, or a Release build) — a `-O0`
   Debug build measures the compiler, not the driver.

`bench_init()` enables I/D-cache (AXI SRAM is cacheable, so the driver's
AN4839 maintenance is real), calls `uart_probe::init()` and traps in
`Error_Handler()` if `Uart::init(&huart3)` rejects the configuration.
ChunkSize sweep: rebuild with `-DBENCH_CHUNK_SIZE=128/256/512`.

## Running

```bash
pip install pyserial
python bench.py COM5 --all --seconds 10 --csv results.csv   # scenarios 0..5
python bench.py COM5 --scenario 2 --seconds 30              # one scenario
```

Scenario order is deliberate: idle → continuous RX → bursty RX →
**continuous TX** (clean TX baseline BEFORE full-duplex) → TX+continuous RX
→ TX+bursty RX.

Note on 115200 baud: cycles-per-event numbers (avg/max) are baud-independent
— they are the LL-decision inputs. CPU% at this baud will be tiny; project
it to a target baud as `events/s × cycles/event` rather than re-running at
every speed.

## Fresh audited run (2026-09-01)

The current UART audit was flashed to a NUCLEO-H7S3L8 revision Y and reran the
complete historical `256x4` matrix at 115200/1M/3M/6M/10M, the `128x4` and
`512x4` 10M chunk points, and all six 10M scenarios on the current `128x8`
default. All 40 recorded windows had zero `GAP`, `OVR`, `ERR`, and `RST`.
Live 115200 -> 3M -> 115200 changes also passed.

- `results_256_audited_2026-09-01.csv`: 30 historical-matrix windows;
- `results_chunk128x4_10M_audited_2026-09-01.csv`: 128x4 RX points;
- `results_chunk512x4_10M_audited_2026-09-01.csv`: 512x4 RX points;
- `results_default128x8_10M_audited_2026-09-01.csv`: six current-default
  scenarios.

The detailed old/new comparison and exact silicon verdict live in
`doc/UART_PARANOID_AUDIT.md`.

## Design verdict (from the 115200→10M sweep, results_*.csv)

```text
HAL backend:
    ACCEPTED

LL backend:
    REJECTED by measured ROI
    (full RX IRQ ≈ 1100 cy / 256 B chunk = 4.3 cy/B; even the VCP-shaped
     worst case — 8.5k IDLE events/s at 10 Mbaud full duplex — costs 2.2%
     of one 600 MHz M7)

Default ChunkSize:
    workload-dependent
    128 better for short/IDLE-heavy bursts on M7 (cache-invalidate scales
    with chunk size: 603/637/692 cy RX for 128/256/512 at 10M over VCP)
    larger chunks expected to benefit true continuous (TC-heavy) streams

Verified:
    H7S3 @ 600 MHz, D-cache enabled, up to 10 Mbaud, full duplex
    zero OVR / ERR / RST / GAP at every point of the sweep
```

A future loopback run (true TC-heavy continuous, no USB bridge in the path)
is a characterization test only — one more point on the graph to confirm the
128/256/512 crossover. Nothing in `Uart.h` changes from its results unless
they show OVR/GAP or a concrete functional defect.
