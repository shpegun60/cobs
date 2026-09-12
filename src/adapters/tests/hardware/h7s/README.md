# H7S live audit regressions

Author: shpegun60. License: [MIT](../../../../../LICENSE).

Latest extension-contract repeat: [receipt](results_extensions_2026-09-12/session.json)
and [raw trials](results_extensions_2026-09-12/results.jsonl). All **186 trials
and 2,037 MCU assertions** passed again over Os/O2/O3; all 15 verifier mutation
controls passed. Every image programmed on attempt one and the fresh boot
backup was restored and read back. See the
[full repeat report](../../../../../doc/HARDWARE_EXTENSIONS_2026-09-12.md).

Earlier schema 2 [receipt](results_dma_audit_2026-09-12/session.json) and
[raw trials](results_dma_audit_2026-09-12/results.jsonl): **186 live trials,
2,037 MCU assertions, three images**, all passed, including I/J/K in three
rounds for each optimization. Original boot firmware restored and read back.
See the [cross-stack audit](../../../../../doc/PARANOID_AUDIT_2026-09-12.md)
for the reproduced HAL ownership faults and the associated host/ARM checks.

These tests supplement, rather than replace, the [full COBS/RTU hardware
matrix](../../../../wire/tests/hardware/h7s/run_fault_matrix.py) and
[RTU framing suite](../../../../modbus/rtu/tests/hardware/h7s/run_framing.py).
The separate firmware uses the normal Cube hooks, `Uart<256,4>`, a framed RTU
endpoint, and the production `UartAdapter`. I/D caches stay enabled and all
fault/timing checks use the real millisecond tick. There is no fake HAL and
no test hook or layout change in a production library.

| Command | Check on the board |
|---|---|
| H | Real RX after each recovery, armed normal DMA, 600 MHz, 9600 baud, UART FIFO and both caches enabled. |
| L | On-M7 local RX/TX loopback: one-byte counts 0/1/254/255 accepted, 256/300/ADU ceiling refused before CRC/TX; Bitwise/Table/NoCrc; two-byte BE/LE large frames; overflowing Layout offsets/counts refused without writes. A volatile offset also exercises both receiver entry points at runtime. This part does not send those vectors over UART. |
| Q | An armed, quiet receiver is not restarted during 800 ms. |
| I | After eight physical bytes, inject UART READY while real RX DMA remains BUSY. Reject publication/re-arm, retain the claim until thread-context repair, then observe exactly one gap/restart and no prefix delivery. |
| J | Stop only RX DMA and call ST's installed DMA-error callback. The sibling TX DMA remains live: its borrow must survive until thread-context abort, with exactly one failed terminal verdict, never success. |
| K | Mirror J onto TX. The original RX destination must not be recycled while its DMA remains live; require one gap/restart and a fresh arm. |
| D | Clear the actual peripheral DMAR bit while HAL still says BUSY_RX; require exactly one debounced restart/gap and a fresh armed DMA. |
| Z | Disable RX DMA NVIC delivery and IDLE publication, then receive 256 physical bytes. Require an observed zero hardware counter with stale BUSY_RX, exactly one restart/gap, and no delivery of the lost block. TX/SysTick remain enabled. |
| P | Receive a full RTU prefix chunk, suppress IDLE and physically receive one more byte. Its nonzero but frozen counter buys one window only, then the stale frame expires and returns the RX block. No driver restart is allowed. |
| A | Control for P: a second physical byte advances the counter at the next window; the frame remains alive and the final tail completes with correct CRC/data. |
| E | Inject an empty public `on_rx({})` after a full physical chunk at 9600. The 325 ms window must remain unchanged; a delayed physical tail must complete. |
| F | Continuous physical 306-byte private RTU ADU, spanning the 256-byte UART chunk. |

`P/A/E/F` use a two-byte length-prefixed private function, not a standard
Modbus ADU limit. The Python sender constructs its CRC independently. The
firmware checks all 300 data bytes and the Pool free count. The control lines
are fixed ASCII and travel through the production UART TX path from persistent
storage. No heap allocator is linked into the test firmware.

`I/J/K` are controlled state/callback injections around real running DMA,
not induced physical bus timeouts. They use the installed HAL error callback
and public registry dispatch, with no production test hooks. TX test bytes
are persistent newline bytes; their bounded arrival is recorded separately
from the assertions. Schema 2 adds these trials while the verifier keeps
schema 1's original plan for historical receipts.

The harness restores publication masks and rearms through the public API
after each trial. A permanently disabled NVIC
is not something the library promises to repair. Each trial has a four-second
firmware limit and a seven-second host limit; failures remain in the record.

## Reproduce on the attached board

Close all other users of the COM port/ST-Link first. The ignored local Cube
scaffold is the same one required by the other H7S suites. No Cube source
is edited. The build uses its 64 KiB internal-flash linker region and fails
if it does not fit; other memories and option bytes are not programmed.

```powershell
python -B src/adapters/tests/hardware/h7s/run.py --port COM6 --serial 002A001F3033510135393935 --output src/adapters/tests/hardware/h7s/results_YYYY-MM-DD
python -B src/adapters/tests/hardware/h7s/verify.py src/adapters/tests/hardware/h7s/results_YYYY-MM-DD --local-images
```

Defaults: `-Os/-O2/-O3`, three fault/control rounds per image. The runner
backs up the **current** 64 KiB before flashing; its `finally` restores that
backup, verifies the programming and reads all 64 KiB back to compare hashes.
It refuses to overwrite an existing results directory. Raw trial JSONL and
the receipt are portable; ELF/map/disassembly, programming logs and firmware
backups stay in the ignored Cube `out/paranoid-<UTC stamp>/` directory.

The record binds each trial to a flashed ELF and hashes every included
library header and Cube input. The verifier checks the exact trial plan,
assertion counts, READY sequence, physical write sizes and recovery counters.
Uncommitted input versions are explicitly reported as such. These are
correctness checks, not CPU utilization or instruction-execution benchmarks.

The verifier itself has fifteen negative mutation checks (missing trials or
assertions, wrong image/READY/DMA evidence, short physical input, doubled TX
verdicts, unsafe prefix delivery, unexpected preambles, and failed restoration).
They read a completed schema 2 record and never access hardware:

```powershell
python -B src/adapters/tests/hardware/h7s/test_verifier.py src/adapters/tests/hardware/h7s/results_dma_audit_2026-09-12
```
