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
`T`/`t` TX generator on/off (64-byte 0x55 frames, back-to-back).
Payload must avoid those four bytes — `bench.py` uses 0x20..0x4F.

Report is one line ending `\r\n`, fields `NAME=<total>cy/<calls>
avg=<cycles> max=<cycles>` for RX/USART/RXDMA/TXDMA/SLOW/TXSTART, then
`BYTES/CH/GAP/TXFR`, `WIN=<ms> CPU=<x.xx>%`, `OVR/ERR/RST`.

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
   `${ProjDirPath}/../../../uart`, `.../libs/spsc/src`, `.../libs/delegate`.
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
