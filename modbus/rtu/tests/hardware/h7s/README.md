<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# NUCLEO-H7S3L8 Modbus RTU + UART hardware verification

Status: audited on real silicon after the protocol-independent CRC extraction,
including CRC-policy A/B, 2026-09-05.

Raw evidence:

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
    <-> modbus::rtu::Endpoint<Pool<8, 2>, crc::Bitwise|crc::Table>
```

The board does not use the C++ CRC implementation to generate PC requests.
The Python side has an independent 256-entry CRC oracle, checks the canonical
`01 03 00 00 00 0A C5 CD` vector, rejects every single-bit mutation of that
ADU, and round-trips every legal function-data length `0..252` before opening
the serial port.

## Physical framing scope

This is the same pragmatic v1 boundary documented by the library: one
continuous `Uart<256,N>` ReceiveToIdle burst is one candidate RTU ADU. The
runner waits for a response between ordinary requests and inserts explicit
silence between the pool-flood requests. UART IDLE is roughly one character,
earlier than the Modbus t1.5 invalid-frame threshold, so this adapter requires
an uninterrupted peer burst and does not claim strict t1.5/t3.5
interoperability.

The exact 256-byte case ends through DMA transfer-complete; short cases end
through UART IDLE. CRC decides whether each candidate is published.

## Board protocol

Every non-control valid ADU is rebuilt and echoed with identical address,
function and function data. The library calculates the response CRC and sends
one contiguous DMA span.

Harness control also uses normal RTU packets:

```text
address = F7
function = 41
data = "MRTU" | command:u8 | token:u32-le | optional argument:u32-le
```

Control responses preserve address/function, set bit 7 of the command byte,
and repeat the token. `HELLO`, `STATS`, `RESET_METRICS`, `HOLD_PACKETS`, and
`BACKPRESSURE_SELFTEST` therefore exercise the same CRC, Packet, Message,
Pool, `send()`, UART borrow and `poll()` paths as ordinary traffic.

## Suites

| Suite | Real path and required outcome |
|---|---|
| `smoke` | custom function with an embedded zero echoes exactly; all counters and owners settle |
| `vectors` | data sizes 0, 1, 2, 31, 32, 63, 64, 127, 128, 251 and 252 across zero/alternating/random patterns; includes exact 255- and 256-byte ADUs |
| `faults` | corruption in address, function, data and CRC is dropped; a valid custom ADU after every bad candidate recovers immediately |
| `selftest` | ACK holds TX block one, a second Message gets `Busy` without losing ownership, and a third allocation exhausts TX block two cleanly |
| `pool` | dequeue is held while 16 separately IDLE-delimited ADUs arrive; exactly the first eight survive FIFO order and the other eight report RX backpressure |
| `stress` | repeated full-duplex standard/custom requests and exact echoes over every important size, with DWT/IRQ accounting and zero unexpected failures |

`all` runs vectors, faults, selftest, pool and stress. Statistics are captured
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
$env:MODBUS_HW_CRC_POLICY = 'bitwise' # accepted: bitwise or table
& 'C:\Program Files\Git\bin\bash.exe' `
  'modbus/rtu/tests/hardware/h7s/build.sh'

& 'C:\ST\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe' `
  -c port=SWD sn=<STLINK_SERIAL> mode=UR reset=HWrst freq=4000 `
  -w 'stm32_cube_test/h7s_cobs_test/out/modbus-hardware/modbus_hardware_bench.elf' `
  -v -rst

python -B modbus/rtu/tests/hardware/h7s/modbus_hardware.py COM6 `
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
  -File 'modbus/rtu/tests/hardware/h7s/run_matrix.ps1' `
  -Port COM6 `
  -StLinkSerial <STLINK_SERIAL> `
  -StressSeconds 5 `
  -ExtendedSeconds 15 `
  -Output 'modbus/rtu/tests/hardware/h7s/results_new.jsonl'
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

## Audited result

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

After moving the algorithms and wire codecs into `crc/Crc.h`, the complete
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
candidates to `receive_adu()`.
