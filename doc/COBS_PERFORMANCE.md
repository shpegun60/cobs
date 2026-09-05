<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# COBS / UART matched-load hardware measurements

**5 September 2026: all 300 observations passed**, covering 1,055,138 echoed
packets and 155,443,006 useful payload bytes in each direction. No UART,
COBS, CRC, allocation, ownership or echo-accounting failures were recorded.
At configured 10M, the 253-byte case costs approximately **6.98% / 21.38% /
9.06% instrumented CPU work** for NoCrc / Bitwise / Table respectively, at
roughly 675–684 kB/s of actual wire traffic per direction. The original
board firmware was restored and verified afterwards.

Raw evidence: [300 measurements](../cobs/tests/hardware/h7s/results_performance_2026-09-05.jsonl)
and [session/restore receipt](../cobs/tests/hardware/h7s/results_performance_2026-09-05.jsonl.session.json).
The measurement session ran 16:23–16:37 UTC (18:23–18:37 Europe/Warsaw).

## What is measured

This benchmark compares **NoCrc, CRC16 Bitwise and CRC16 Table on the same
payload corpus** through the real NUCLEO-H7S3L8 UART/COBS echo path. It does
not change the production libraries or the existing version-2 test firmware.
The earlier migration results remain valid but their NoCrc/1024 and
CRC16/253 stress mixes are not a controlled CRC comparison.

The CPU number is instrumented **UART + COBS + application echo work**, not
just the COBS byte codec. Each useful byte is received and returned, so CRC
is calculated twice: RX validation and TX generation. RX/TX pools, packet
ownership, the application copy, UART/DMA handlers and cache maintenance
are exercised. Main-loop empty/in-flight polls are deliberately excluded;
this is not an RTOS idle-task measurement or total utilization of a spinning
bare-metal `while (1)` loop.

The measured serial traffic is full-duplex 8N1. A wire byte consumes ten
serial bits **in each direction**. RX and TX overlap; their byte counts must
not be added and divided by a single direction's bandwidth.

## Results: CRC Table reduces cost without removing integrity

CPU and wire percentages below use the same board observation window and
combine both repetitions by their raw totals. `wire %` is achieved traffic
divided by nominal 8N1 line capacity, per direction. The CPU qualifications
in [CPU accounting and uncertainty](#cpu-accounting-and-uncertainty) apply
to every table. No row below claims 100% wire saturation.

### 253-byte random payload, window 7

This is the directly comparable default-sized case: even NoCrc is limited
to 253 useful bytes so the length width and application workload match.
At 10M, Table uses about 57.5% fewer measured cycles per echo than Bitwise
(48,477 versus 20,622); the actual throughputs are close, not identical.

| Baud | NoCrc CPU % | Bitwise CPU % | Table CPU % | NoCrc wire % | Bitwise wire % | Table wire % |
|---:|---:|---:|---:|---:|---:|---:|
| 115200 | 0.074 | 0.301 | 0.103 | 96.2 | 95.7 | 95.8 |
| 1000000 | 0.630 | 2.587 | 0.940 | 91.9 | 91.8 | 91.7 |
| 3000000 | 3.403 | 8.558 | 4.151 | 82.7 | 82.6 | 82.4 |
| 6000000 | 5.663 | 15.068 | 6.949 | 74.2 | 74.7 | 74.7 |
| 10000000 | 6.977 | 21.382 | 9.059 | 67.5 | 68.4 | 68.1 |

At 1M, about 92% of nominal capacity is actually exercised; at 10M it is
about 68%. Consequently 21.38% is the Bitwise cost **at the achieved load**,
not a claim that a continuous full 10M echo stream costs only 21.38%.

### 1024-byte random payload, window 7

Larger packets amortize fixed per-packet work. At 10M this case achieves
about 715–717 kB/s per direction. Table costs 8.04% versus 21.28% Bitwise,
or about 62% fewer measured cycles per echo. This is a deliberately
selected H2 format, not the default H1 configuration.

| Baud | NoCrc CPU % | Bitwise CPU % | Table CPU % | NoCrc wire % | Bitwise wire % | Table wire % |
|---:|---:|---:|---:|---:|---:|---:|
| 115200 | 0.059 | 0.287 | 0.091 | 92.9 | 93.0 | 93.0 |
| 1000000 | 0.535 | 2.490 | 0.792 | 91.2 | 92.1 | 91.3 |
| 3000000 | 2.869 | 8.214 | 3.624 | 81.4 | 81.6 | 81.7 |
| 6000000 | 4.676 | 14.722 | 6.207 | 73.0 | 75.7 | 75.6 |
| 10000000 | 5.967 | 21.284 | 8.043 | 71.6 | 71.7 | 71.5 |

### All cases at 10 Mbaud

Packet size and flight window matter in addition to baud. `latency8` is a
single outstanding request–reply workload, not a short-packet saturation
test. Its wire utilization is only about 8–9%; `short32` reaches about 40–41%.
Thus a smaller CPU number on `latency8` does not mean a lower CPU cost at
equal useful-byte throughput. Mixed and boundary/zero cases all passed.

| Case | NoCrc CPU % | Bitwise CPU % | Table CPU % | NoCrc cycles/echo | Bitwise cycles/echo | Table cycles/echo |
|---|---:|---:|---:|---:|---:|---:|
| latency8 | 4.617 | 5.539 | 4.521 | 3946 | 4728 | 3844 |
| short32 | 9.601 | 16.970 | 10.584 | 5026 | 9282 | 5739 |
| random253 | 6.977 | 21.382 | 9.059 | 15887 | 48477 | 20622 |
| zero253 | 7.204 | 21.634 | 9.265 | 16381 | 48946 | 21083 |
| nonzero253 | 6.963 | 21.409 | 9.040 | 15860 | 48510 | 20589 |
| mixed253 | 7.971 | 20.418 | 9.603 | 8295 | 21617 | 10211 |
| random1024 | 5.967 | 21.284 | 8.043 | 51517 | 183724 | 69683 |
| zero1024 | 6.212 | 21.187 | 8.232 | 53469 | 186038 | 71616 |
| nonzero1024 | 5.827 | 21.422 | 7.839 | 51217 | 183731 | 69236 |
| mixed1024 | 6.576 | 21.272 | 8.523 | 19605 | 63391 | 25497 |

These are total instrumented echo-path cycles, not isolated CRC costs.
The small `latency8` Table/NoCrc reversal is not evidence that adding a CRC
has negative cost: each variant measures its complete compiled stack and
event pattern, and the absolute difference is small. Isolated calculator
measurements are linked separately below.

### Why CPU does not scale simply with baud

The 253-byte NoCrc case's first repetition records about **2.05 RX callbacks
per echo at 1M but 10.50 at 3M**. Its USART IRQ count rises from about 2.05
to 11.50 per echo. Similar fragmentation appears for Bitwise/Table. Those
extra callbacks/IDLE-related IRQs explain additional driver work independently
of checksum cost. This is measured behavior of this end-to-end VCP setup;
the experiment does not isolate the USB bridge from host scheduling.

The largest relative CPU difference between the two repetitions anywhere
in the matrix is about 7.9% (`latency8`, Table, 6M). The central 253-byte
10M measurements are much closer: NoCrc 6.962–6.992%, Bitwise
21.362–21.403%, Table 9.050–9.068%. Two repeats do not establish a statistical
confidence interval; retain the raw repeat values for later comparisons.

The practical speed/flash tradeoff is explicit: all **30 ELF images** were
checked against the recorded SHA-256. NoCrc and Bitwise have **zero CRC
lookup bytes**; every Table image has exactly **512 read-only lookup bytes**.
This is lookup storage, not the entire firmware size difference. Choosing
Table preserves CRC/wire semantics; choosing NoCrc removes integrity checking.

## Reproduce

From the repository root, with the board connected to the explicitly named
probe and port:

```powershell
python -B cobs/tests/hardware/h7s/test_performance.py

powershell -NoProfile -ExecutionPolicy Bypass -File `
  cobs/tests/hardware/h7s/run_performance.ps1 `
  -Port COM6 -StLinkSerial 002A001F3033510135393935 `
  -Output cobs/tests/hardware/h7s/results_performance_NEW.jsonl

python -B cobs/tests/hardware/h7s/verify_performance.py `
  cobs/tests/hardware/h7s/results_performance_NEW.jsonl

# Recheck the saved session and every published result-table row:
python -B cobs/tests/hardware/h7s/verify_performance.py `
  cobs/tests/hardware/h7s/results_performance_2026-09-05.jsonl `
  --check-doc doc/COBS_PERFORMANCE.md
```

The runner refuses an existing output file, backs up all 64 KiB of internal
boot flash before programming, and restores/verifies that exact image in
`finally`, including after a failed measurement. No option bytes or external
flash are programmed. Build/flash/restore logs, the backup and every measured
ELF stay in the ignored `cobs/tests/out/performance-<timestamp>/` directory.
The JSONL and its adjacent `.session.json` receipt are durable results.

The optional verifier argument `--nm <path-to-arm-none-eabi-nm.exe>` checks
all retained ELF hashes and, using its sibling `objdump`, verifies that
Table contributes exactly one 512-byte lookup in `.rodata`, with no lookup
in the NoCrc/Bitwise images. Without the local ignored Cube scaffold it
reports that source-identity limitation; library/harness sources and result
arithmetic can still be checked.

## Fixed experiment design

- Board: NUCLEO-H7S3L8, STM32H7S3L8 rev Y, Cortex-M7 at 600 MHz.
- Probe: ST-LINK `002A001F3033510135393935`, firmware V3J17M11, COM6 VCP.
- Compiler: GNU Arm 14.3.1, `-Os`, no LTO, C++20, strict warnings as errors.
- I-cache and D-cache enabled; real `Uart<128,8>`, `wire::Pool<8,2>`.
- Nominal baud rates: 115200, 1M, 3M, 6M, 10M. Hardware oversampling is 8 at
  10M, 16 otherwise. Physical baud is not measured with a logic analyzer.
- Two geometries for **all three policies**: max payload 253 / H1, and
  max payload 1024 / H2. NoCrc deliberately uses 253, not its usual 255,
  in the first group. Thus the payload limits and length-field width match.
- Two independent two-second send windows per case, followed by draining
  every outstanding echo. A second repeat reverses case order; CRC policy
  order rotates across baud rates. This is repeatability evidence, not a
  statistical confidence interval from many independent boards.
- Payload generation, reference CRC, COBS encoding and oracle round trips
  all finish **before** timing. During the measurement the host checks the
  complete encoded echo against the precomputed expected frame, in FIFO
  order, without recalculating CRC or decoding COBS.
- The sender keeps at most seven packets in flight (one for `latency8`). It
  submits another packet as echoes return, and checks short host writes.
  A seven-frame window is below the eight-packet RX pool capacity.
- Each case has a deterministic cyclic corpus. The JSONL records its
  payload-only SHA-256 and exact per-entry counts: the payload corpus is
  identical across CRC variants, although wire frames and completed frame
  counts can differ. Fixed-size cases give the cleanest cost-per-echo
  comparison. Mixed cases can finish at different points in the corpus.

| Case | Payload bytes | Data | In flight | Max payload / header |
|---|---|---|---:|---|
| latency8 | 8 | pseudo-random | 1 | 253 / H1 |
| short32 | 32 | pseudo-random | 7 | 253 / H1 |
| random253 | 253 | pseudo-random | 7 | 253 / H1 |
| zero253 | 253 | all zero | 7 | 253 / H1 |
| nonzero253 | 253 | 1..255 pattern | 7 | 253 / H1 |
| mixed253 | 8, 32, 64, 127, 128, 253 | five patterns | 7 | 253 / H1 |
| random1024 | 1024 | pseudo-random | 7 | 1024 / H2 |
| zero1024 | 1024 | all zero | 7 | 1024 / H2 |
| nonzero1024 | 1024 | 1..255 pattern | 7 | 1024 / H2 |
| mixed1024 | 32, 127, 128, 253, 254, 255, 256, 511, 512, 1024 | five patterns | 7 | 1024 / H2 |

The mixed cases cross every size with random, zero, nonzero, alternating
zero/nonzero and COBS 254-byte boundary patterns. Pure zero/nonzero cases
contain repeated identical payloads; they prove exact bytes and total
accounting, not uniqueness of a sequence number in every frame.

## CPU accounting and uncertainty

The unchanged firmware takes a coherent, IRQ-masked statistics snapshot.
The host independently checks all echo counts, byte counts, pool occupancy,
CRC/COBS/UART error counters and TX completion/release counts. The observing
STATS request is the one additional RX/control packet, not an echoed data
packet. Its RX work is included; its TX response is not.

```text
I = USART IRQ cycles + RX DMA IRQ cycles + TX DMA IRQ cycles
T = UART proceedSlow cycles + application packet-process cycles
    + successful COBS TX-release cycles
C = I + T

seconds = board_window_ms / 1000
CPU_work_percent = 100 * C / (600000000 * seconds)
wire_percent = 100 * echoed_wire_bytes * 10 / (nominal_baud * seconds)
cycles_per_echo = C / echoed_frames
```

`cobs_consume` is nested **inside** `uart_slow`; `uart_tx_start` is inside
`packet_process`; `uart_rx` is inside an IRQ. Adding these diagnostic
counters to `C` would double-charge the same work. They remain available
separately in every raw observation.

IRQ scopes have equal preemption priority, but an IRQ can preempt a thread
scope. Therefore `C` can include a measured IRQ interval twice. The JSONL
also supplies `max(I,T)` and `I+T` as conservative lower/upper bounds for
the **union of the measured scopes**, not for all program execution.
Probe overhead, exception entry/exit outside the measured intervals,
SysTick, empty polling and other uninstrumented work prevent either bound
from being a certified whole-program CPU limit. Measurements must retain
this qualification; decimal precision is not an accuracy guarantee.

The board window starts after the metric-reset ACK has released its DMA
borrow. It includes the host's 50 ms reset-settle wait, final echo drain,
and arrival of the observing STATS request. Throughput and CPU use this
**same board denominator**. Host elapsed time is retained separately, not
mixed into the board CPU calculation. Two-repeat summaries divide summed
cycles/bytes by summed time (or frames), never average rounded row averages.

For an explicitly labelled estimate only:

```text
CPU_full_line_linear_estimate = 100 * (C / wire_bytes) * (baud / 10) / 600000000
```

This projects the observed cycles per wire byte to a gapless full-duplex
echo load. It is **not** a saturated-link measurement, a worst-case bound,
or proof that the same interrupt/chunk pattern persists at full utilization.
ST-Link VCP, USB/host scheduling, the flight window and packet turnaround
all affect the achieved traffic. Low CPU at a configured high baud alone
does not prove that the MCU processed that baud continuously.

### Linear full-line extrapolation, NOT a saturated-line measurement

These numbers answer only "what if the measured cost per byte stayed the
same while line utilization became 100%?" They are deliberately separate
from the live results. At 10M they project approximately 31.3% Bitwise /
13.3% Table for 253-byte echoes, or 29.7% / 11.3% for 1024-byte echoes.
A genuinely saturated source and more exact non-overlapping CPU accounting
would be needed to promote these estimates to full-line utilization proof.

| Case | Baud | NoCrc CPU estimate % | Bitwise CPU estimate % | Table CPU estimate % |
|---|---:|---:|---:|---:|
| random253 | 115200 | 0.08 | 0.31 | 0.11 |
| random253 | 1000000 | 0.68 | 2.82 | 1.02 |
| random253 | 3000000 | 4.11 | 10.36 | 5.04 |
| random253 | 6000000 | 7.63 | 20.16 | 9.30 |
| random253 | 10000000 | 10.34 | 31.26 | 13.30 |
| random1024 | 115200 | 0.06 | 0.31 | 0.10 |
| random1024 | 1000000 | 0.59 | 2.70 | 0.87 |
| random1024 | 3000000 | 3.52 | 10.07 | 4.44 |
| random1024 | 6000000 | 6.41 | 19.44 | 8.21 |
| random1024 | 10000000 | 8.33 | 29.67 | 11.25 |

## Evidence and validation

The versioned source of each number is the matched-load JSONL, not an older
stress table. The verifier checks completeness and duplicate keys, exactly
two repeats of each of 10 cases × 3 policies × 5 baud rates, corpus identity,
source fingerprints, ownership/error accounting and independently recomputed
metrics. Six offline runner/aggregation tests cover corpus parity, wire
fragmentation/coalescing, extra delimiters, denominators, nested counters and
weighted aggregation.

For this session, all 300 records, 43 source-file identities, 30 ELF images,
all corpus/accounting checks and every derived metric passed independent
rechecking. The original flash backup SHA-256 is
`a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456`;
the complete result file SHA-256 is
`2eb2bb06ef5753651425ed294e1a6f599d839edd20060a4f9be6d2a8a3a60d5a`.
The preceding 85 COBS + 127 RTU hardware records and 6,360 ARM-object checks
also still pass their existing source-identity/arithmetic verifier; no
production source or existing test firmware was modified for this experiment.

Related documents:

- [COBS wire format and CRC placement](PROTOCOL.md)
- [Shared-policy migration validation](SHARED_POLICIES_VALIDATION.md)
- [Hardware harness and older results](../cobs/tests/hardware/h7s/README.md)
- [CRC implementation / isolated CRC benchmarks](../crc/README.md)

### Validation assessment: Share with caveats

The experiment supports comparison of instrumented work and actual echo
throughput on this board/toolchain/transport, with repeated matching inputs.
It does not establish total application utilization, error-rate guarantees
for other links, or continuous saturation at 10M. There is no claim of
dynamic instruction count: DWT cycles include stalls and are not instructions.
