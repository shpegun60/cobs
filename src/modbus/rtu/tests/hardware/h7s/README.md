<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# NUCLEO-H7S3L8 Modbus RTU + UART hardware verification

Latest [live regression, 7 September](../../../../../../doc/HARDWARE_REGRESSION_2026-09-07.md):
144 passed fault/vector/pool/CRC/stress records across all nine policies at
115200/1M, and a fresh 30-record burst/framed matrix through 10M. All framed
trials passed; burst losses and unavailable trial summaries remain visible.
The report also retains two intermittent Qt-server timeouts and their repeats.

The [direct comparison with COBS](../../../../../../doc/PROTOCOL_COMPARISON.md)
has matched endpoint-only DWT measurements and equal-work UART results.
Its 3M/6M/10M probes retain incomplete-ADU failures separately from valid
CPU comparisons. Those failures are the reason the endpoint now has an
optional framing policy; [its measurement](#framing-policy-at-high-baud-2026-09-05)
on the same bridge is the newest record here.

Current shared-storage validation: [127 records](results_shared_storage_2026-09-05.jsonl)
cover all nine policies at 115200 and 1M, with exact image/source identities,
functional/fault/pool tests, CRC samples, stress and paced traffic.
The [current report](../../../../../../doc/SHARED_POLICIES_VALIDATION.md) contains
the new results; `python -B src/wire/tests/verify_hardware_migration.py` checks
their identities and derived metrics against this working tree.

The records and measurements described below predate shared storage and
configurable MaxAdu. They are retained as historical evidence; current
configuration is `Endpoint<wire::Pool<Rx,Tx>, modbus::rtu::Format<Crc,MaxAdu>>`.

Status: audited on real silicon after the protocol-independent CRC extraction,
including CRC-policy A/B, 2026-09-05.

Raw evidence:

- [`results_framing_2026-09-05.jsonl`](results_framing_2026-09-05.jsonl)
  — the default burst endpoint against the optional framing policy at 1M,
  3M, 6M and 10M: single, split and glued frames, smoke and the full vector
  suite, with every image manifest and the restored-firmware receipt
  (`verify_framing.py`);
- [Nine-policy CRC benchmark](CRC_BENCHMARK.md) and
  [raw results](results_crc_all_2026-09-05.jsonl): CRC8/16/32/64 Bitwise/Table
  and NoCrc, live calculation cycles, fixed-rate CPU comparison and the
  disassembly of each flashed image;
- [`results_crc_library_2026-09-05.jsonl`](results_crc_library_2026-09-05.jsonl)
  — final post-extraction `crc::Crc16Bitwise` versus `crc::Crc16Table` matrix
  at 115200/1M, extended 1M stress, and restored default-image smoke test;
- [`results_crc_policy_2026-09-05.jsonl`](results_crc_policy_2026-09-05.jsonl)
  — same-target `crc::Bitwise` versus `crc::Table` functional, fault, pool,
  5-second and 15-second stress comparison;
- [`results_paranoid_final_2026-09-02.jsonl`](results_paranoid_final_2026-09-02.jsonl)
  — final `-Os` 115200/1M matrix, extended 1M stress, and restored smoke;
- [`results_paranoid_o2_2026-09-02.jsonl`](results_paranoid_o2_2026-09-02.jsonl)
  — independent `-O2` 1M functional/fault/pool/stress run;
- [`results_paranoid_o3_lto_2026-09-02.jsonl`](results_paranoid_o3_lto_2026-09-02.jsonl)
  — independent `-O3 + LTO` 1M run and extended stress;
- [`results_scalar_api_final_2026-09-02.jsonl`](results_scalar_api_final_2026-09-02.jsonl)
  — fresh universal-scalar API 115200/1M matrix and restored smoke test;
- [`results_audited_2026-09-02.jsonl`](results_audited_2026-09-02.jsonl) —
  original accepted 115200/1M baseline plus its restored smoke test;
- [`results_high_baud_probe_2026-09-02.jsonl`](results_high_baud_probe_2026-09-02.jsonl)
  — the separate 3M UART-IDLE boundary probe, including its captured
  failure counters.

This harness verifies the production Modbus RTU and UART layers together on
real STM32H7S3L8 silicon:

```text
independent Python CRC/ADU oracle
    <-> ST-Link VCP / COM port
    <-> USART3 + GPDMA
    <-> Uart<256, 4>
    <-> modbus::rtu::Endpoint<Pool<8, 2>, Policy>
```

The board does not use the C++ CRC implementation to generate PC requests.
The Python side has independent bit-level CRC8/16/32/64 oracles, checks the
named models' standard check values, rejects every single-bit mutation of a
canonical ADU, and round-trips every legal function-data length before opening
the serial port. Maximum data depends on the policy: 254/253/252/250/246 bytes
for NoCrc/CRC8/CRC16/CRC32/CRC64. The offline test also checks the canonical
Modbus wire vector and CRC32 against Python's separate zlib implementation.
NoCrc deliberately has no corruption detection. Only the default CRC16
model/codec is standard Modbus RTU; the others are private wire formats.

## Physical framing scope

This is the same pragmatic v1 boundary documented by the library: one
continuous `Uart<256,N>` ReceiveToIdle burst is one candidate RTU ADU. The
runner waits for a response between ordinary requests and inserts explicit
silence between the pool-flood requests. UART IDLE is roughly one character,
earlier than the Modbus t1.5 invalid-frame threshold, so this adapter requires
an uninterrupted peer burst and does not claim strict t1.5/t3.5
interoperability.

The exact 256-byte case ends through DMA transfer-complete; short cases end
through UART IDLE. The selected integrity policy decides whether each candidate
is published; NoCrc does not reject based on integrity.

## Board protocol

Every non-control valid ADU is rebuilt and echoed with identical address,
function and function data. The library calculates the response CRC and sends
one contiguous DMA span.

Harness control also uses normal RTU packets:

```text
address = F7
function = 41
data = "MRTU" | command:u8 | token:u32-le | optional arguments:u32-le[]
```

Control responses preserve address/function, set bit 7 of the command byte,
and repeat the token. `HELLO`, `STATS`, `RESET_METRICS`, `HOLD_PACKETS`, and
`BACKPRESSURE_SELFTEST` therefore exercise the same CRC, Packet, Message,
Pool, `send()`, UART borrow and `poll()` paths as ordinary traffic.
Protocol version 3 also has `CRC_BENCHMARK`: two arguments select input length
and iterations. It returns cycles, independently checkable checksum/mix,
DWT state, cache state and CPU identity. Protocol version 4 appends the
framing mode to `HELLO`: with `MODBUS_HW_FRAMER=1` the board is built with
the framing policy, every function's data starts with a two-byte big-endian
body length that the library owns on both sides, the UART callback feeds
`consume()` instead of `receive_adu()`, and the PC peer must be run with
`--framer`; a peer started in the wrong mode is refused by `HELLO`. STATS
keeps its layout, so records of either version stay verifiable.

## Suites

| Suite | Real path and required outcome |
|---|---|
| `smoke` | custom function with an embedded zero echoes exactly; all counters and owners settle |
| `vectors` | data sizes 0, 1, 2, 31, 32, 63, 64, 127, 128, max-1 and max across zero/alternating/random patterns; includes exact 255- and 256-byte ADUs |
| `faults` | four corruptions are dropped, each followed by recovery; NoCrc instead must echo all four changed frames |
| `selftest` | ACK holds TX block one, a second Message gets `Busy` without losing ownership, and a third allocation exhausts TX block two cleanly |
| `pool` | dequeue is held while 16 separately IDLE-delimited ADUs arrive; exactly the first eight survive FIFO order and the other eight report RX backpressure |
| `stress` | repeated full-duplex standard/custom requests and exact echoes over every important size, with DWT/IRQ accounting and zero unexpected failures |
| `paced` | same precomputed traffic at a target average 300 frames/s, with achieved cadence and DWT/IRQ accounting |
| `crc_benchmark` | eight equal input lengths, nine samples of eight calls, live DWT cycles and an independent PC checksum oracle |
| `framing` | records, without asserting, whether one frame in one write, one frame split into two writes 1 ms apart, two frames in one write, and an orphan half followed 50 ms later by a whole different frame come back exactly, three repeats at 8, 32, 128 and maximum body bytes; the default endpoint is expected to lose the split and glued shapes, and only the framed endpoint's stale-frame watchdog (`poll(now_ms)`) lets the whole frame survive the orphan (the earlier records used a 5 ms split pause, which the watchdog now correctly treats as a dead frame) |

`all` runs vectors, faults, selftest, pool, crc_benchmark, stress and paced;
`framing` is run explicitly by `run_framing.py`.
Statistics are captured
while the STATS request owns exactly one RX block and before its response owns
a TX block, so the runner requires `rx_in_use=1` and `tx_in_use=0` at that
observation point.

## Build and one run

The local Cube scaffold is the same checked H7S project used by the UART and
COBS silicon tests. Build products stay under its gitignored
`out/modbus-hardware/` directory.

```powershell
$env:MODBUS_HW_BAUD = '115200'
$env:MODBUS_HW_OPT = '-Os'       # accepted: -Os, -O2, -O3
$env:MODBUS_HW_LTO = '0'         # accepted: 0 or 1
$env:MODBUS_HW_CRC_POLICY = 'bitwise' # aliases bitwise/table or the nine named policies
$env:MODBUS_HW_FRAMER = '0'      # 1 builds the framing-policy endpoint (peer needs --framer)
$env:MODBUS_HW_CXXFLAGS_EXTRA = '' # extra C++ flags for controlled experiments, e.g. -falign-loops=8
& 'C:\Program Files\Git\bin\bash.exe' `
  'src/modbus/rtu/tests/hardware/h7s/build.sh'

& 'C:\ST\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe' `
  -c port=SWD sn=<STLINK_SERIAL> mode=UR reset=HWrst freq=4000 `
  -w 'stm32_cube_test/h7s_cobs_test/out/modbus-hardware/modbus_hardware_bench.elf' `
  -v -rst

python -B src/modbus/rtu/tests/hardware/h7s/modbus_hardware.py COM6 `
  --baud 115200 --crc-policy bitwise --suite all --seconds 5
```

## Audited matrix

The default matrix rebuilds, flashes, verifies and tests 115200 and 1M baud,
runs an extended stress at 1M, and finally restores and smoke-checks a verified
115200 image. Higher rates remain accepted through `-BaudRates`, but they are
transport-boundary probes rather than part of the default acceptance matrix.
At 3M one PC `write()` was observed as two UART-IDLE candidates, but this
harness did not timestamp the pause and cannot attribute its source or judge
it against t1.5.

```powershell
& 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' `
  -NoProfile -ExecutionPolicy Bypass `
  -File 'src/modbus/rtu/tests/hardware/h7s/run_matrix.ps1' `
  -Port COM6 `
  -StLinkSerial <STLINK_SERIAL> `
  -StressSeconds 5 `
  -ExtendedSeconds 15 `
  -Output 'src/modbus/rtu/tests/hardware/h7s/results_new.jsonl'
```

The runner refuses to append into an existing result file.

To run both built-ins in one verified A/B matrix from PowerShell:

```powershell
& .\modbus\rtu\tests\hardware\h7s\run_matrix.ps1 `
  -Port COM6 `
  -StLinkSerial <STLINK_SERIAL> `
  -BaudRates @(1000000) `
  -CrcPolicies @('bitwise', 'table') `
  -StressSeconds 5 `
  -ExtendedSeconds 15 `
  -Output 'modbus\rtu\tests\hardware\h7s\results_crc_policy_new.jsonl'
```

The firmware reports its compiled policy ID in `HELLO`; the Python runner
checks it against `--crc-policy` before running any suite and writes the policy
name into every JSONL record. This prevents two accidental flashes of the same
image from being accepted as an A/B comparison. Unless `-LeaveAtLastBaud` is
given, the matrix restores and smoke-tests `Bitwise` at 115200.

## Historical audited results

The following measurements predate the nine-policy benchmark and its
precomputed/fixed-rate host traffic. Keep those comparisons within their own
recorded workloads; use [CRC_BENCHMARK.md](CRC_BENCHMARK.md) for the new matrix.

Both accepted rates passed vectors, four corruption/recovery cases, TX
backpressure, deterministic RX exhaustion, and stress. The runner then
rebuilt, flashed, verified, and smoke-tested the 115200 image left on the
board.

| Baud / duration | Exact stress ADUs | Function-data bytes | Data MiB/s | Measured CPU |
|---:|---:|---:|---:|---:|
| 115200 / 5 s | 316 | 26,948 | 0.0051 | 0.151% |
| 1M / 5 s | 2,463 | 212,772 | 0.0406 | 1.205% |
| 1M / 15 s | 7,319 | 632,449 | 0.0402 | 1.207% |

Every stress record has zero CRC rejection, RTU allocation failure, stream
gap, refused/failed send, UART overrun/error/restart, and pool rejection or
exhaustion. The intentional suites separately produced exactly:

- four CRC errors for corruption in address, function, data and CRC, followed
  by four immediate valid recoveries;
- one `SendResult::Busy` while preserving the caller's Message and one TX pool
  exhaustion while two owners were live;
- eight retained FIFO RX packets and eight clean allocation failures from a
  16-frame physical-burst flood into `Pool<8,2>`.

The final 115200 image is `22,592 B text`, `12 B data`, and `6,832 B BSS`.
The 1M image differs by four text bytes. The observed target was NUCLEO-H7S3L8
Rev Y, device ID `0x485`, ST-Link V3J17M11, 3.26 V, and a 600 MHz core.

### CRC policy A/B, 2026-09-05

The fresh `-Os`, no-LTO comparison used the same board, 1M line rate, UART
configuration, `Pool<8,2>`, Python oracle and traffic for both template
instantiations. Both images passed all 31 vectors, four independent corrupted
ADUs with immediate recovery, the TX backpressure self-test, the deterministic
16-into-8 RX pool test, 5-second stress, and 15-second stress. Every unexpected
RTU, UART, ownership and pool failure counter remained zero.

| Policy / duration | Frames | Data bytes | Data MiB/s | Integrated CPU | RTU RX avg/max cycles | Packet/TX avg/max cycles |
|---|---:|---:|---:|---:|---:|---:|
| `crc::Bitwise` / 5 s | 2,452 | 211,821 | 0.0404 | 1.190% | 6,066 / 17,224 | 6,910 / 18,027 |
| `crc::Table` / 5 s | 2,485 | 214,674 | 0.0409 | 0.402% | 1,128 / 2,853 | 1,976 / 3,948 |
| `crc::Bitwise` / 15 s | 7,313 | 631,912 | 0.0402 | 1.197% | 6,070 / 17,224 | 6,911 / 18,029 |
| `crc::Table` / 15 s | 7,444 | 643,196 | 0.0409 | 0.406% | 1,128 / 2,854 | 1,976 / 3,980 |

On this workload Table reduced measured integrated CPU by 66.1% and average
`receive_adu()` cost by 81.4%. Throughput changed only 1.8% because the serial
link and host request/response cadence bound the test. These are observed
end-to-end results, not a general cycle guarantee for other MCUs or traffic.

At 1M, the linked `Bitwise` image is `22,620 B text`; the `Table` image is
`23,120 B text`. Both have `12 B data` and `6,832 B BSS`. The lookup itself is
exactly 512 read-only bytes, while removing the bitwise loop saves 12 bytes,
so the net image cost is 500 text bytes and zero RAM. The matrix finally
restored, verified and smoke-tested the 115200 `Bitwise` image.

After the final constructor-constraint hardening, both 1M images were rebuilt
from the final tree and converted with `arm-none-eabi-objcopy -O binary`. Their
load images were byte-identical to the tested A/B artifacts: Bitwise SHA-256
`44359B33F28624B6A1CA1028187AA8EE63D713481EB4DAE562917576396AE9CC`, Table
SHA-256 `360078B330A477B4F164D62BA532281E2D4E1DB242B7942870828FDFA93EE892`.
ELF container hashes are not used for this claim because rebuild metadata does
not belong to the MCU load image.

### General CRC library regression, 2026-09-05

After moving the algorithms and wire codecs into `src/crc/Crc.h`, the complete
`-Os`, no-LTO matrix was repeated from the final refactored tree. The JSONL has
23 passing records and no failed record. Both policies passed vectors, all four
intentional corruption cases and immediate recoveries, backpressure, the
deterministic 16-into-8 pool test, 5-second stress at both 115200 and 1M, and
15-second stress at 1M. Every unexpected RTU, UART, ownership and pool counter
remained zero.

| Policy / 1M duration | Frames | Data bytes | Data MiB/s | Integrated CPU | RTU RX avg/max cycles | Packet/TX avg/max cycles |
|---|---:|---:|---:|---:|---:|---:|
| `crc::Crc16Bitwise` / 5 s | 2,464 | 213,024 | 0.0406 | 1.188% | 6,010 / 16,997 | 6,865 / 17,822 |
| `crc::Crc16Table` / 5 s | 2,479 | 214,009 | 0.0408 | 0.415% | 1,188 / 3,078 | 2,088 / 4,254 |
| `crc::Crc16Bitwise` / 15 s | 7,337 | 634,317 | 0.0403 | 1.192% | 6,011 / 16,997 | 6,865 / 17,823 |
| `crc::Crc16Table` / 15 s | 7,411 | 640,343 | 0.0407 | 0.419% | 1,189 / 3,079 | 2,088 / 4,264 |

On this workload the table policy reduced measured integrated CPU by 64.9%
and average `receive_adu()` cost by 80.2%. The refactored 1M images are
`22,728 B text` for Bitwise and `23,228 B text` for Table; both remain
`12 B data` and `6,832 B BSS`. The 500-byte net text difference is the private
512-byte class table minus code no longer needed by the table loop. The matrix
then restored, verified, and smoke-tested the default 115200 Bitwise image
(`22,724 B text`).

### Optimization cross-check

The hardware build accepts only the explicit `-Os`, `-O2`, or `-O3` values.
LTO can be enabled separately. Generated `syscalls.c` and `sysmem.c` remain
ordinary function-section objects so `--gc-sections` can discard unused heap
hooks; every accepted image is rejected if `_sbrk`, `malloc`, `free`,
`operator new`, or `operator delete` survives the final link.

At 1M, the separate `-O2` image passed 1,473 stress ADUs / 127,182 data bytes
in 3 seconds at 1.145% measured CPU. The `-O3 + LTO` image passed 2,468 ADUs /
213,058 bytes in 5 seconds and 4,917 ADUs / 425,097 bytes in 10 seconds at
1.082% and 1.087% measured CPU respectively. Those are observed harness
measurements, not a generic speed guarantee; `-O3 + LTO` used 26,112 B text
versus 22,596 B for the final 1M `-Os` image.

### Why 3M is recorded separately

The same 31-vector suite completed once at 3M, then a repeated run failed at
the 132-byte ADU. The board reported `26` candidate bursts for 24 valid ADUs:
the failed single PC write became two independently CRC-invalid bursts
(`crc_errors=2`) with zero UART errors, overruns, RTU gaps, allocation failures,
or pool failures.

That is direct evidence that the current IDLE adapter produced two boundaries
inside one host write. It is not evidence that identifies the component which
created the pause, nor does it show whether the pause was below or above the
Modbus t1.5 threshold. Streaming COBS tolerates such fragmentation; this
constrained burst adapter does not. Therefore 3M is not claimed as a reliable
full-size result, while the complete 1M matrix is. Any adapter with a different
boundary contract remains outside Endpoint and supplies only complete
candidates to `receive_adu()` — or the endpoint is given the framing policy
measured next.

## Framing policy at high baud, 2026-09-05

The same bridge, the same `Uart<256,4>` and the same CRC16 Bitwise format,
with the endpoint built twice: `framing::None` (the default, one burst
candidate per `receive_adu()`) and the optional framing policy (`consume()`
on every chunk, every function length-prefixed). `run_framing.py` built,
inspected and flashed each of the eight images, drove `smoke`, `framing`
and `vectors` against it, and restored the board's original flash with
programmer verification and a byte-exact read-back:

```powershell
python -B src/modbus/rtu/tests/hardware/h7s/run_framing.py --port COM6 `
  --serial 002A001F3033510135393935 `
  --output src/modbus/rtu/tests/hardware/h7s/results_framing_2026-09-05.jsonl
python -B src/modbus/rtu/tests/hardware/h7s/verify_framing.py `
  src/modbus/rtu/tests/hardware/h7s/results_framing_2026-09-05.jsonl `
  --check-doc src/modbus/rtu/tests/hardware/h7s/README.md
```

### RTU frame boundaries on the H7S ST-Link bridge: default burst framing versus the framing policy

| Baud | Endpoint | single-write echoes | split-write echoes | two frames in one write | smoke | vectors suite |
|---:|---|---:|---:|---:|---|---|
| 1000000 | framing::None (burst candidate) | 12/12 | 0/12 | 3/12 | passed | passed |
| 1000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | passed | passed |
| 3000000 | framing::None (burst candidate) | 12/12 | 0/12 | 0/12 | passed | FAILED at vector 24 (128 data bytes), no echo |
| 3000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | passed | passed |
| 6000000 | framing::None (burst candidate) | 4/12 | 0/12 | 0/12 | FAILED no HELLO response | FAILED at vector 7 (31 data bytes), no echo |
| 6000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | passed | passed |
| 10000000 | framing::None (burst candidate) | 5/12 | 0/12 | 0/12 | passed | FAILED at vector 8 (31 data bytes), no echo |
| 10000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | passed | passed |

Counts are exact echoes out of twelve attempts (three per body size). The
default endpoint behaves as the 2026-09-02 probe predicted: a frame the host
sends in two writes is never one candidate (`split` 0/12 at every baud), two
frames in one write are one CRC-failing candidate unless the bridge happens
to pause between them (`glued` 3/12 at 1M, then 0/12), and from 6M the bridge
splits even single writes (`single` 4/12 and 5/12; at 6M the 21-byte smoke
request itself got no answer). Its STATS at 6M counted 122 candidates for
36 data frames plus control traffic, 114 CRC errors and 3 too-short
candidates, with zero UART errors or overruns: the losses are frame-boundary
losses, not line errors.

The framed endpoint delivered every shape at every baud with zero CRC errors
(49 candidates, 49 frames received in each `framing` snapshot) and passed the
full 31-vector suite at 1M, 3M, 6M and 10M. The splitting is visible in its
counters and harmless: the RX callback ran 33, 74 and 58 times for the 32
frames of the vector suite at 3M, 6M and 10M. This is the first RTU result
above 1M on this bridge that is a complete-load result rather than a probe.

What it does not show: standard-function framing (the harness protocol is
length-prefixed on every function; the standard table is verified on the host
against the specification's worked examples) and Modbus t1.5/t3.5 timing,
which the policy does not implement. The framed endpoint's CPU was measured
afterwards next to COBS and the default endpoint, at 1M and up to 10M
(`doc/PROTOCOL_COMPARISON.md`).

### Stale frames: an orphan half must not take the next frame with it

A frame whose sender stops mid-frame would otherwise stay in flight in the
framed endpoint, holding its RX block and gluing itself to the next frame,
which then fails CRC and is lost with it. The `framing` suite gained a fourth
shape for it — the first half of a frame, 50 ms of silence, then a whole
different frame — and the split shape's pause was set to 1 ms, a bridge-like
split (with a 5 ms pause the half is, correctly, a dead frame).

The first cure was a 5 ms watchdog inside the endpoint
([record](results_framing_stale_2026-09-05.jsonl), kept as evidence of that
step):

| Baud | Endpoint | single-write echoes | split-write echoes | two frames in one write | orphan half then a whole frame | smoke | vectors suite |
|---:|---|---:|---:|---:|---:|---|---|
| 1000000 | framing::None (burst candidate) | 12/12 | 0/12 | 2/12 | 12/12 | passed | passed |
| 1000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |

It was replaced the next day: with DMA reception the software sees
silence for a whole chunk's transfer time while the line is still busy
(294 ms for a 256-byte chunk at 9600 baud), so a fixed limit inside the
endpoint kills every frame that spans two chunks at low baud. The rule now
lives in `modbus::rtu::UartAdapter`, which knows the driver's chunk size and
whether a chunk ended by IDLE or by transfer-complete, and the endpoint
exposes only `expire_incomplete()`. The harness runs through the adapter
(`bind()` for the gap and transport binding, the adapter's `on_rx()` under
the `rtu_receive` timing scope, `prepare()` before and `finish()` after the
driver's `proceed()`), both endpoints rerun at 1M
([record](results_framing_adapter_2026-09-06.jsonl), then again after the
lifecycle review — line rate read from the handle, transactional `bind()`,
12-bit characters, deadline judged after the driver has delivered —
[record](results_framing_lifecycle_2026-09-06.jsonl), and a third time with
the deadline asking the driver's `rx_progress()` whether DMA is already
taking the remainder before it expires a frame —
[record](results_framing_progress_2026-09-06.jsonl), and once more after the
repository migration, the adapter now living in `src/adapters/rtu/` and the
harness including it from there —
[record](results_framing_layout_2026-09-06.jsonl); `verify_framing.py`):

### RTU frame boundaries on the H7S ST-Link bridge: default burst framing versus the framing policy

| Baud | Endpoint | single-write echoes | split-write echoes | two frames in one write | orphan half then a whole frame | smoke | vectors suite |
|---:|---|---:|---:|---:|---:|---|---|
| 1000000 | framing::None (burst candidate) | 12/12 | 0/12 | 3/12 | 12/12 | passed | passed |
| 1000000 | framing policy (length-prefixed) | 12/12 | 12/12 | 12/12 | 12/12 | passed | passed |

The framed endpoint echoed the whole frame after every orphan (12/12) with
zero CRC errors; without any expiry the same firmware echoed 0/12 there, each
orphan costing one CRC failure and the frame behind it. The default endpoint
is unaffected by orphans by construction: the half is a separate burst that
fails CRC on its own. The low-baud, multi-chunk behaviour of the adapter's
rule is proven on the host against the fake HAL (`test_uart_integration`,
9600 baud, 700-byte private ADU across three chunks), which this 1M record
cannot exercise: a 256-byte ADU here is one chunk.
