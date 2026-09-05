<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# NUCLEO-H7S3L8 COBS + UART hardware integration test

## Matched performance comparison through 10M

See also the [direct COBS/RTU comparison](../../../../doc/PROTOCOL_COMPARISON.md):
identical useful data and CRC policies, endpoint-only live cycles, paired
UART cadence, and separate high-baud physical-framing probes.

The separate [matched-load benchmark](../../../../doc/COBS_PERFORMANCE.md)
uses the unchanged v2 firmware to compare NoCrc / CRC16 Bitwise / Table with
identical payload corpora, 253/H1 and 1024/H2 geometries, short/long/mixed
packets and two repeats at each of five baud rates. It reports actual wire
throughput alongside instrumented CPU work, with the scope-nesting and
IRQ-preemption limitations made explicit.

`run_performance.ps1` backs up and restores the original 64 KiB board image
even on a failed measurement. `cobs_performance.py` precomputes all host
CRC/COBS work before timing; `verify_performance.py` independently checks
the records and produces weighted comparison tables.

The [5 September results](results_performance_2026-09-05.jsonl) contain
300 passed measurements / 1,055,138 echoes. The adjacent
[session receipt](results_performance_2026-09-05.jsonl.session.json) records
verified restoration of the original firmware. See the benchmark document
for the CPU/throughput tables, including the distinction between measured
10M-configured traffic and an extrapolated continuous full-line load.

## Current shared-storage / CRC v2 validation

The current harness defaults to the real `cobs::Format<>`: CRC16 Bitwise,
253 useful bytes, one-byte length, `Endpoint<wire::Pool<8,2>, Format<>>`.
Its harness protocol is version 2; STATS is a two-page coherent snapshot so
the control response does not require an enlarged application payload.

All three configurations passed the full 115200/1M/3M/6M/10M matrix:

- [CRC16 Bitwise / 253](results_crc_default_2026-09-05.jsonl), 29 records;
- [CRC16 Table / 253](results_crc_table_2026-09-05.jsonl), 28 records;
- [explicit legacy NoCrc / 1024](results_legacy_shared_storage_2026-09-05.jsonl), 28 records.

See [the current validation report](../../../../doc/SHARED_POLICIES_VALIDATION.md)
for tables, caveats and source/image identities. Recheck without touching a
board using `python -B wire/tests/verify_hardware_migration.py` from the root.

From the repository root (these commands build and flash the selected board):

```powershell
powershell -ExecutionPolicy Bypass -File cobs/tests/hardware/h7s/run_matrix.ps1 `
  -Port COM6 -StLinkSerial 002A001F3033510135393935 -Crc bitwise

# Alternative policies/limits use the same runner:
# -Crc table                  CRC16 Table, payload 253
# -Crc none -MaxPayload 1024   exact legacy NoCrc/H2 wire
```

For manual builds, `COBS_HW_CRC` is 0=NoCrc, 1=Bitwise, 2=Table and
`COBS_HW_MAX_PAYLOAD` is optional. The Python peer takes matching `--crc`
and `--max-payload` options and validates HELLO before testing. These are
explicit test configurations, not automatic wire-version detection.

## Historical v1 measurements

The geometry, wire/control version and numeric tables below describe the
original 2026-09-01 images. They remain comparison evidence, not current
default settings or the current storage API.

Status: audited on real silicon, 2026-09-01

Raw evidence:

- `results_audited_2026-09-01.jsonl` — baseline audit (29 records);
- `results_format_api_2026-09-01.jsonl` — full matrix after the concise
  `Format`/`Pool` API migration and VCP gap-runner hardening (29 records).

This is not a UART-only throughput generator. It exercises the production
stack end to end in both directions:

```text
independent Python codec
    <-> ST-Link VCP / COM port
    <-> USART3 + GPDMA
    <-> Uart<128, 8>
    <-> cobs::Endpoint<cobs::Pool<8, 2, cobs::Format<1024>>>
```

Ordinary application bodies are echoed byte for byte. Test control requests
and their replies travel through exactly the same length header, COBS codec,
UART DMA engine, pools, and endpoint ownership path as ordinary data. There is
no unframed command channel that could accidentally bypass the code under test.

## Files

- `cobs_bench.cpp` is the board application and DWT instrumentation.
- `cobs_hardware.py` is an independent PC encoder/decoder, protocol client,
  strict oracle, and suite runner.
- `build.sh` performs a fresh strict ARM build against the local Cube scaffold.
- `run_matrix.ps1` builds, verifies, flashes, tests every baud, records JSONL,
  and restores a verified 115200-baud image on success.
- `results_audited_2026-09-01.jsonl` is the immutable result set quoted below.

Build products stay under the gitignored
`stm32_cube_test/h7s_cobs_test/out/cobs-hardware/` directory.

## Fixed geometry

| Property | Value |
|---|---:|
| MCU/board | STM32H7S3L8 / NUCLEO-H7S3L8 rev Y |
| core clock | 600 MHz |
| link | USART3 through ST-Link VCP |
| UART RX chunks | 8 x 128 bytes |
| COBS RX pool | 8 blocks |
| COBS TX pool | 2 blocks |
| maximum RX/TX application body | 1024 bytes |
| decoded length field | 2-byte little-endian |
| COBS delimiter | `00` |
| caches | I-cache and D-cache enabled |
| optimization | `-Os`, C++20, no exceptions/RTTI |

The board reports this geometry in `HELLO`; the PC refuses to run a suite if
even one field, the configured baud, or the 600 MHz clock differs. The final
115200 image occupied 23,616 bytes text, 12 bytes data, and 14,576 bytes BSS.
The 10 Mbaud oversampling-8 image differed only slightly: 23,628 bytes text.

## Wire and control contract

Every wire frame is:

```text
COBS( body_length:u16-le | body[body_length] ) | 00
```

The PC codec is not built from the C++ implementation. Before opening the COM
port it checks known canonical encodings, 55 boundary/pattern round trips,
empty and one-byte engine vectors, and malformed encodings.

A valid test-control body is at least nine bytes:

```text
C7 43 42 53 | command:u8 | token:u32-le | optional argument:u32-le
```

The response repeats the magic and token and sets bit 7 of the command. The
commands are `HELLO`, `STATS`, `RESET_METRICS`, `HOLD_PACKETS`, `STALL_LOOP`,
and `BACKPRESSURE_SELFTEST`. Bodies outside that complete envelope are normal
application payloads.

`STATS` is a coherent IRQ-masked snapshot. This matters on the 32-bit M7:
each DWT total is 64-bit and an unguarded live read could otherwise tear. The
snapshot is taken while the STATS request packet itself is owned and before
its response allocates TX storage, so a leak-free observation intentionally
reports:

```text
RX: available=7, in_use=1
TX: available=2, in_use=0
```

The PC asserts that exact occupancy after every suite.

## Suites and exact acceptance rules

| Suite | What is exercised | Required outcome |
|---|---|---|
| `smoke` | HELLO, metric reset, one payload containing zero, STATS | exact echo, exact ownership/counter accounting, no failures |
| `vectors` | sizes 0, 1, 2, 31, 32, 63, 64, 127, 128, 253-256, 511, 512, 1023, 1024; zero, non-zero, alternating, boundary and seeded-random patterns | 81 exact echoes / 22,680 payload bytes; no loss or error |
| `faults` | bare delimiter, absent header, structurally malformed COBS, short body, long body, zero declaration with body, oversize declaration | the bare delimiter is a no-op; exactly 6 lost frames classified as malformed=1, oversize=1, length_mismatch=4, resyncs=2; every following sentinel echoes |
| `selftest` | ACK owns TX block 1; a second message owns block 2 and attempts send while busy; a third allocation is attempted | exactly one `SendResult::Busy`, the second owner remains valid, exactly one TX-pool exhaustion, no corruption |
| `pool` | hold dequeue for 400 ms while UART and COBS RX keep running; send 32 distinct 64-byte frames | exactly the first 8 frames survive FIFO order; exactly 24 allocation failures/lost frames/resyncs/RX exhaustions; all owners recover and a sentinel echoes |
| `gap` | stall the main loop for 500 ms and stream 64 maximum-size frames | a real UART chunk overrun occurs, exactly one COBS gap/resync is observed, an explicit delimiter re-establishes trust, then a sentinel echoes |
| `stress` | pipelined exact echoes over sizes 32, 64, 127, 128, 253-256, 511, 512, 1024 | every sequence and byte matches; all unexpected UART/COBS/pool counters stay zero; per-frame process, DMA, TX-start and TX-release call counts match |

`all` runs vectors, faults, selftest, pool, then stress. `gap` remains explicit
because it deliberately destroys bytes. At high baud the entire flood can end
before the thread announces the transport gap; the explicit bare delimiter is
therefore required by the decoder contract before a deliverable frame. The
delimiter is synchronization material, not a hidden retry.

While producing the gap, the PC runner strictly decodes and discards irrelevant
flood echoes in small batches. This prevents the host-side ST-Link VCP receive
queue from overflowing at 115200 while leaving malformed-response detection
enabled; only the board's RX direction is meant to lose bytes in this test.

## CPU accounting

The reported `integrated_cpu_percent` is the sum of non-overlapping,
data-driven regions, divided by `600 MHz * window_seconds`:

```text
USART IRQ + RX-DMA IRQ + TX-DMA IRQ
    + uart_slow
    + cobs_tx_release
    + packet_process
```

Do not add `uart_rx`: it is inside a full USART/RX-DMA IRQ. Do not add
`cobs_consume`: it is invoked inside `uart_slow`. Do not add `uart_tx_start`:
it is inside `packet_process`. `packet_process` includes RX dequeue, TX
allocation/copy/COBS encode/UART start, and the final checked RX-pool release.
`cobs_tx_release` measures the terminal poll that returns the borrowed TX
block. Idle and still-busy spin checks are deliberately not instrumented;
their rate belongs to the application's scheduler, not to bytes processed.

## Reproducing one build and suite

Prerequisites on the recorded Windows machine are Python 3 with `pyserial`,
Git Bash, STM32CubeProgrammer 2.21, the CubeIDE GNU 14.3 ARM toolchain, and the
local Cube-generated `stm32_cube_test/h7s_cobs_test` scaffold. That scaffold
must retain the `bench_init()`/`bench_loop()` calls and the three IRQ DWT hooks
described by `uart/tests/bench/README.md`. `build.sh` rejects a stale local
`uart_bench.h` before compiling.

```powershell
python -m pip install pyserial

$env:COBS_HW_BAUD = '10000000'
& 'C:\Program Files\Git\bin\bash.exe' `
  'cobs/tests/hardware/h7s/build.sh'

& 'C:\ST\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe' `
  -c port=SWD sn=<STLINK_SERIAL> mode=UR reset=HWrst freq=4000 `
  -w 'stm32_cube_test/h7s_cobs_test/out/cobs-hardware/cobs_hardware_bench.elf' `
  -v -rst

python -B cobs/tests/hardware/h7s/cobs_hardware.py COM6 `
  --baud 10000000 --suite all --seconds 10 --window 4
```

## Reproducing the audited matrix

Windows script execution is disabled by policy on the recorded host, so invoke
the checked-in runner through a process-local bypass:

```powershell
& 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' `
  -NoProfile -ExecutionPolicy Bypass `
  -File 'cobs/tests/hardware/h7s/run_matrix.ps1' `
  -Port COM6 `
  -StLinkSerial <STLINK_SERIAL> `
  -StressSeconds 10 `
  -ExtendedSeconds 30 `
  -Output 'cobs/tests/hardware/h7s/results_new.jsonl'
```

The runner refuses to append into an existing output file. It performs a fresh
build, flash and CubeProgrammer verification at 115200, 1M, 3M, 6M and 10M;
runs `all` at every baud; runs the destructive gap/recovery test at the lowest
and highest baud; runs an additional 30-second 10M/window-7 stress; then
rebuilds, flashes and smoke-checks the default 115200 image.

## Audited silicon result

The concise API follow-up repeated the complete matrix with
`Endpoint<Pool<8,2,Format<1024>>>`. All five baud rates, both physical gap
tests, and the final restored 115200 smoke test passed. The extended 10 Mbaud
run delivered 61,611 exact frames / 19,133,016 payload bytes in 30 seconds at
0.608 MiB/s and 5.977% measured CPU, with zero unexpected UART or COBS loss.
The verified 115200 image remained 23,616 bytes text, 12 bytes data, and 14,576
bytes BSS.

Environment observed by CubeProgrammer: board rev Y, MCU device ID `0x485`,
silicon revision Y, ST-Link V3J17M11, target voltage 3.26 V, verified SWD
programming at 3.3 MHz.

| Configured baud | Window | Exact echoes | Payload bytes | Payload MiB/s | Measured CPU | Unexpected loss/OVR/ERR/RST |
|---:|---:|---:|---:|---:|---:|---:|
| 115,200 | 10 s | 297 | 92,232 | 0.009 | 0.058% | 0 |
| 1,000,000 | 10 s | 2,445 | 758,575 | 0.072 | 0.542% | 0 |
| 3,000,000 | 10 s | 6,610 | 2,051,992 | 0.196 | 2.513% | 0 |
| 6,000,000 | 10 s | 12,078 | 3,750,768 | 0.358 | 4.253% | 0 |
| 10,000,000 | 10 s | 18,563 | 5,763,650 | 0.550 | 5.273% | 0 |
| 10,000,000 / window 7 | 30 s | 61,235 | 19,015,336 | 0.604 | 5.929% | 0 |

The 30-second 10M run had zero response/self-test failures, UART overrun/error/
restart, COBS loss/allocation/malformed/oversize/length/resync/send failure, and
RX/TX rejection/exhaustion. Its useful decomposition was:

| Region | Calls | Average cycles | Maximum cycles |
|---|---:|---:|---:|
| full USART IRQ | 455,776 | 882 | 1,036 |
| TX DMA IRQ | 61,235 | 277 | 286 |
| UART slow service | 394,685 | 846 | 2,763 |
| COBS consume, nested in UART slow | 394,541 | 635 | 1,720 |
| complete packet processing | 61,235 | 5,079 | 13,995 |
| UART TX start, nested in packet processing | 61,235 | 504 | 1,544 |
| COBS TX owner release | 61,235 | 76 | 1,099 |

The ST-Link VCP delivered RX as IDLE-terminated fragments in this run, so the
RX-DMA transfer-complete IRQ count was zero; the full USART IRQ count captures
those real fragment costs. Payload throughput is consequently a property of
this complete PC/VCP/full-duplex echo setup, not a claim about the raw USART
line's theoretical maximum.

All intentional negative counters also matched exactly at every tested baud:
fault classification was `1/1/4/2`, pool exhaustion was `24/24/24/24`, and
backpressure was one Busy plus one TX exhaustion. Both 115200 and 10M physical
gap tests produced one UART overrun and one ordered COBS loss/resync, then
recovered without reset.

Verdict: the production COBS/UART composition is functionally correct on this
H7S setup through 10 Mbaud, pool/backpressure ownership behaves exactly, and
the measured data-path cost leaves substantial CPU headroom. The matrix runner
finished by flashing and smoke-verifying the 115200-baud COBS harness; that is
the board state left after the audit.
