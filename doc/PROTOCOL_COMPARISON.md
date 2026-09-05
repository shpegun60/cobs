<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# COBS versus Modbus RTU: matched hardware comparison

**5 September 2026: RTU's endpoint path is cheaper, while COBS provides
stream framing that RTU requires its caller to supply.** With 252 useful
random bytes, COBS adds about 4,230–4,269 cycles (7.0–7.1 microseconds at
600 MHz) per complete RX/build-TX/release transaction in the endpoint-only
benchmark. That is 12.5% extra work with CRC16 Bitwise, 62.5% with Table,
or 3.01 times the RTU cost without CRC. Ratios differ because checksum cost
can dominate the total; the absolute framing-related difference is similar.

The accepted [raw result](../wire/tests/hardware/h7s/results_comparison_2026-09-05.json)
contains **225 endpoint configurations / 2,025 timing windows**, **168 paired
UART windows / 47,352 exact echoes / 4,721,088 useful bytes per direction**,
and six physical high-baud probes. Endpoint and ordinary paired UART checks
passed. The probes deliberately retain RTU frame-delivery failures instead
of labeling rejection work as successful traffic. All **21 flashed ELFs**
and the restoration of the original boot image were verified. No production
library or existing hardware harness was changed.

Session: 17:28–17:36 UTC / 19:28–19:36 Europe/Warsaw, 5 September 2026.

## Two different questions, two experiments

This comparison concerns **our COBS and Modbus RTU libraries**, not Modbus
TCP, register-server application logic or an arbitrary third-party stack.
There are two distinct measurements:

1. **Endpoint-only hot-path cost on H7S silicon.** Identical useful data,
   identical `wire::Pool<8,2>` policy, identical CRC implementation, explicit
   receive/build/send/release scopes and a synchronous borrow-only transport.
   This isolates the libraries from UART and physical frame delivery.
2. **Actual paired UART echo traffic.** Both existing, unchanged hardware
   harnesses receive the same payload corpus, exact packet count and scheduled
   rate at 115200 and 1M. These rows include the UART driver, interrupts,
   caches, application echo and ownership handling. Separate 3M/6M/10M probes
   test whether the physical link supplies usable complete candidates.

Do not juxtapose the older COBS window-7 stress percentages with Modbus's
older request–reply percentages as if those represented equal useful work.
The previous [COBS matrix](COBS_PERFORMANCE.md) remains valid for its own
workload, but this is a new controlled comparison, not a re-labeling of it.

## Measured results

### Library-only RX + TX + release, random 252-byte payload

These are **live DWT cycles on the MCU**, with UART excluded. Whole-frame
input is the direct library-to-library comparison; the middle column shows
COBS's additional cost when its encoded input arrives in 128-byte fragments.
RTU cannot accept those same arbitrary fragments as if they were whole ADUs.

| CRC | COBS whole cycles | COBS chunk128 cycles | RTU whole cycles | COBS whole / RTU |
|---|---:|---:|---:|---:|
| none | 6333.0 | 6424.0 | 2104.0 | 3.01x |
| bitwise | 38505.8 | 38659.8 | 34237.0 | 1.12x |
| table | 11020.5 | 11171.5 | 6783.5 | 1.62x |

The default Bitwise case is therefore about 64.18 microseconds for COBS
versus 57.06 for RTU, including both RX and TX. Table is about 18.37 versus
11.31 microseconds. This is not a claim that a complete Modbus application,
including framing and register handling, is always faster by the same ratio.

### Actual UART echo, random252, equal scheduled rate

These are **measured instrumented UART + protocol + echo CPU percentages**,
not the endpoint-only model. Within each row, useful input bytes, frame count,
cadence and both repetitions match across protocols and CRC policies.

| Baud | Frames/s | COBS NoCrc % | RTU NoCrc % | COBS Bitwise % | RTU Bitwise % | COBS Table % | RTU Table % |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 115200 | 16 | 0.026 | 0.011 | 0.111 | 0.096 | 0.040 | 0.024 |
| 1000000 | 145 | 0.236 | 0.096 | 1.012 | 0.862 | 0.368 | 0.213 |

At 1M this is roughly **35.08–35.12 kB/s of useful payload** per direction
over the board interval, with approximately **35.4–35.8% wire utilization**.
At 115200 it is about 3.87 kB/s useful payload and 33.9–34.3% wire utilization.
This paired low-duty-cycle comparison must not be confused with the previous
maximum-window COBS stress load. The host's two-second windows actually lasted
2.00004–2.00124 seconds; worst recorded scheduling lateness was 2.231 ms.

### Actual UART echo at 1M, Bitwise, all scenarios

The packet rate is intentionally lower for larger frames, allowing a complete
request/reply pair on both protocols. Comparisons **within** a row are matched;
the CPU percentages between different rows do not represent equal traffic.
The cycles/echo columns help distinguish packet cost from offered load.

| Case | Frames/s | COBS CPU % | RTU CPU % | COBS cycles/echo | RTU cycles/echo |
|---|---:|---:|---:|---:|---:|
| random8 | 300 | 0.226 | 0.208 | 4712.9 | 4338.1 |
| random32 | 300 | 0.398 | 0.362 | 8290.7 | 7535.1 |
| random128 | 281 | 1.074 | 0.913 | 23856.1 | 20294.3 |
| random252 | 145 | 1.012 | 0.862 | 43604.6 | 37118.2 |
| zero252 | 145 | 1.024 | 0.862 | 44117.4 | 37115.4 |
| nonzero252 | 145 | 1.012 | 0.862 | 43602.3 | 37116.6 |
| mixed | 145 | 0.469 | 0.400 | 20208.3 | 17247.6 |

### Physical high-baud framing: RTU has no valid complete-load timing row

The default CRC16 Bitwise policy was probed with three independent requests
at each of 8, 32, 128 and 252 useful bytes. Counts below are exact echoes out
of three, not success probabilities or long-run error-rate estimates.

| Protocol | Baud | 8 B | 32 B | 128 B | 252 B | Total exact echoes |
|---|---:|---:|---:|---:|---:|---:|
| COBS | 3000000 | 3/3 | 3/3 | 3/3 | 3/3 | 12/12 |
| RTU | 3000000 | 3/3 | 3/3 | 3/3 | 2/3 | 11/12 |
| COBS | 6000000 | 3/3 | 3/3 | 3/3 | 3/3 | 12/12 |
| RTU | 6000000 | 3/3 | 2/3 | 0/3 | 0/3 | 5/12 |
| COBS | 10000000 | 3/3 | 3/3 | 3/3 | 3/3 | 12/12 |
| RTU | 10000000 | 3/3 | 1/3 | 0/3 | 0/3 | 4/12 |

At 3M, RTU counted 14 candidates for 12 data requests plus the observing
STATS request, with two CRC errors. At 6M it counted 31 candidates, 24 CRC
errors and one too-short candidate. No UART overrun/error/restart was counted
in either snapshot. These observations support **frame splitting at the
UART-burst-to-ADU boundary**, not CPU saturation or a checksum-algorithm
failure. They do not independently localize every gap to USB versus host
scheduling. At 10M the final STATS request itself timed out; those counters
are explicitly unavailable, while the captured echo bytes remain evidence.

Consequently there is **no accepted equal-load UART CPU comparison at
3M/6M/10M for this RTU adapter and VCP setup**. Do not put the CPU spent
rejecting partial candidates in the same column as successful COBS traffic.
The libraries and UART driver are left unchanged; no length-based or timed
framer was quietly added to make a benchmark pass.

## Why RTU can be cheaper without being a replacement for COBS

`rtu.receive_adu()` receives a **complete, externally framed candidate**.
It checks its envelope/CRC and copies the accepted frame into packet-owned
storage. It does not recover packet boundaries from arbitrary UART chunks.
COBS instead decodes its byte stream, interprets the declared length and
delimiter, reconstructs payload/CRC, and generates an encoded TX frame.
That work provides a capability the RTU receive boundary does not supply.

Both protocols calculate CRC on RX and TX. The algorithm is the same
CRC-16/MODBUS for Bitwise/Table, but the covered bytes are intentionally
protocol-specific: COBS covers useful payload; RTU covers the address,
function and function data. The same useful data adds two input bytes to each
RTU checksum calculation. Table versus Bitwise changes no wire bytes.
NoCrc in RTU, or an ADU larger than 256 bytes, is a **private variant**, not
standard Modbus RTU. The benchmark does not change any library defaults.

## Endpoint-only measurement contract

Target: NUCLEO-H7S3L8 / STM32H7S3L8 rev Y, Cortex-M7 at 600 MHz,
ST-LINK `002A001F3033510135393935`. GNU Arm 14.3.1, C++20, `-Os`, no LTO.
Instruction and data caches are enabled and checked through returned core
registers. Code and CRC lookup live in internal flash. Endpoint/pool objects
are local on the DTCM stack; the input payload and encoded candidate are in
static RAM. This placement is identical in intent for both libraries but
differs from the UART harnesses' static endpoint objects. The results are
warm-cache measurements, not a cold-cache or arbitrary-memory guarantee.

The common sizes are 8, 32, 128 and **252 useful bytes**. 252 is the largest
payload shared by the standard CRC16 RTU ADU and default COBS, without treating
an RTU address/function byte as useful data. Each uses five deterministic
patterns: pseudo-random, all zero, nonzero 1..255, alternating zero/A5 and
254-byte-boundary patterns. COBS receives either the complete encoded frame
or fragments of at most 128 bytes; RTU always receives one whole ADU.

An additional explicitly private wide case uses 1024 useful bytes:
`cobs::Format<Crc,1024>` and `rtu::Format<Crc,1026 + Crc::wire_size>`.
It is a library geometry/performance experiment only, not a 1024-byte
standard Modbus UART interoperability claim.

For each protocol/policy/size/pattern/fragment configuration:

- The firmware produces a candidate once, outside timing. Its **entire wire
  image** is returned and compared against the independent Python COBS/CRC
  oracle, including header, metadata, CRC order and delimiter.
- A warm-up echo precedes each of nine measurement windows.
- Each window performs four transactions for the small geometry or one for
  the 1024-byte case. Interrupts are disabled only around this bounded window,
  then restored to their previous state. Every observed window must stay below
  600,000 cycles (1 ms). UART output occurs after restoration, outside timing.
- The RX scope includes `consume`/`receive_adu` and `pop_packet`. The TX scope
  includes `make_message`, `append_bytes`, `send`, message cleanup and the
  borrow callback. The release scope completes the test transport's borrow,
  calls `poll` and releases the Packet. Both protocols use the same public
  lifecycle; no decoder internals or fake CRC implementations are substituted.
- Between scopes, outside the cycle sums, the firmware checks exact payload,
  RTU metadata, exact echoed wire bytes and zero live owners/queued packets.
  The transport only holds a span until explicit completion; it does not add
  a transmit copy, DMA or UART cost to the endpoint-only number.
- Compiler memory barriers bracket cycle reads. Counts are gross, including
  the small scope boundary overhead; they are not baseline-subtracted and
  are not dynamic instruction counts.

Totals use the **median of nine per-transaction RX+TX+release sums**. A total
is not constructed by adding independently rounded or independently selected
component medians. Component values remain in the raw samples for inspection.
All 225 configurations and all 2,025 windows are retained, not just the fastest.

## Interpreting the baud-equivalent CPU model

Baud does not change the endpoint-only timer: **cycles are measured on the
board; CPU at a specified traffic rate is calculated**. For the main 252-byte
random case, the common reference is the 257-byte CRC16 COBS wire frame:

```text
common_frames_per_second = nominal_baud / (10 * 257)
library_CPU_percent = median_cycles_per_echo * common_frames_per_second
                      / 600000000 * 100
```

Exactly the same packet rate is applied to COBS and RTU, and to every CRC
policy, so useful-byte throughput matches. Using each protocol's individual
maximum wire rate instead would compare different amounts of useful work.
This is a model of equal RX + echoed TX endpoint work, **excluding UART**.
It assumes the caller supplies correctly framed RTU candidates and does not
include t1.5/t3.5 framing, serial turnaround, bus silence or request processing.
It is not evidence of a continuously saturated physical Modbus link at 10M.

### Equal packet-rate CPU model from measured library-only cycles, random 252 bytes

The values below are **calculated from live endpoint cycle measurements**,
not observed UART CPU at these baud rates. For example, 10M corresponds to
3,891.05 complete RX+TX transactions per second at the chosen 257-byte
reference cadence. Supplying whole RTU frames at that rate is an assumption,
not something the VCP framing probe demonstrated.

| Nominal baud | COBS NoCrc % | RTU NoCrc % | COBS Bitwise % | RTU Bitwise % | COBS Table % | RTU Table % |
|---:|---:|---:|---:|---:|---:|---:|
| 115200 | 0.047 | 0.016 | 0.288 | 0.256 | 0.082 | 0.051 |
| 1000000 | 0.411 | 0.136 | 2.497 | 2.220 | 0.715 | 0.440 |
| 3000000 | 1.232 | 0.409 | 7.491 | 6.661 | 2.144 | 1.320 |
| 6000000 | 2.464 | 0.819 | 14.983 | 13.322 | 4.288 | 2.639 |
| 10000000 | 4.107 | 1.364 | 24.971 | 22.203 | 7.147 | 4.399 |

The 2,025 measured windows had a maximum IRQ-off duration of **287.64 us**,
below the 1 ms guard. Repeated hot samples can be identical on this
deterministic, interrupt-free DTCM workload; their spread is not an estimate
of uncertainty across traffic patterns, memory placements or other targets.

## Actual UART measurement contract

The unchanged harnesses are intentional realistic integrations:

| Setting | COBS | RTU |
|---|---|---|
| UART DMA storage | `Uart<128,8>` | `Uart<256,4>` |
| Packet storage | `wire::Pool<8,2>` | `wire::Pool<8,2>` |
| Useful test sizes | 8, 32, 128, 252 | same |
| Useful maximum in firmware | 253 for all tested policies | 252 with CRC16; 254 with NoCrc |
| Packets in flight | 1 | 1 |
| Wire framing | COBS stream + delimiter | whole UART burst passed as candidate |

Both UART DMA configurations provide 1024 bytes total, but their chunk size
and resulting event pattern differ. Thus the UART table is an **integration
comparison**, not an isolation of protocol code alone. The first experiment
exists specifically to separate those effects. No framer was added to UART,
and no old firmware/hardware evidence was changed.

Both protocols see exactly the same precomputed payloads, packet count and
schedule. The rate is selected from the longest wire request/reply pair
across both protocols and all three policies, with 25% line-time margin and
a 300 fps cap. Seven scenarios cover random 8/32/128/252, zero252,
nonzero252 and a mixed size/pattern corpus. Every scenario has two independent
two-second schedule windows; the second reverses scenario order.

The host computes payloads, CRC and encoding before resetting counters.
It transmits one whole reference frame, waits for the exact complete echo,
then follows the next scheduled deadline. Per-frame lateness and total host
time are retained rather than assuming the intended cadence was achieved.
Both reset paths settle for 80 ms. CPU and wire utilization use the same
board observation interval, including that settle and the observing STATS RX.
The JSON also includes a separately labelled CPU normalization to the target
packet rate. This is an average traffic schedule, not strict Modbus timing.

Integrated cycle sum:

```text
USART IRQ + RX DMA IRQ + TX DMA IRQ
+ UART proceedSlow + packet processing + successful TX release
```

The protocol RX sub-counter is already inside `proceedSlow`; adding it again
would double-count. Thread scopes may include IRQ preemption, so their sum
with IRQ scopes can overstate the union of measured work. Empty busy polling
and other uninstrumented application work are excluded. These qualifications
are the same as in the preceding COBS performance document. The endpoint-only
experiment avoids IRQ overlap but deliberately measures a different boundary.

## Reproduce and audit

All commands below are from the repository root. The full run flashes the
explicitly selected board; it saves and restores/verifies all 64 KiB of its
original internal boot flash, including after an exception. External flash
and option bytes are not programmed. Original images, every measured ELF
and build/flash/restore logs remain in an ignored `wire/tests/out/comparison-*`
session directory. The durable result records its location and source/image
fingerprints. Existing ignored Cube scaffolding and pyserial are required.

```powershell
python -B wire/tests/hardware/h7s/test_comparison.py

python -B wire/tests/hardware/h7s/run_comparison.py `
  --port COM6 --serial 002A001F3033510135393935 `
  --output wire/tests/hardware/h7s/results_comparison_NEW.json

python -B wire/tests/hardware/h7s/verify_comparison.py `
  wire/tests/hardware/h7s/results_comparison_NEW.json

# Recheck the recorded session and all published numeric table rows:
python -B wire/tests/hardware/h7s/verify_comparison.py `
  wire/tests/hardware/h7s/results_comparison_2026-09-05.json `
  --check-doc doc/PROTOCOL_COMPARISON.md
```

`--core-only` is an optional shorter run of endpoint-only measurements.
The verifier accepts the same flag for that explicitly reduced result set.
Optional `--nm <arm-none-eabi-nm.exe>` checks all retained ELF identities,
read-only lookup size/placement, flash verification logs and restored backup.
Optional `--check-doc doc/PROTOCOL_COMPARISON.md` checks that every generated
published numeric table row agrees with the selected recorded result.

### Validation assessment: Share with caveats

The purpose of `validate-data` here is to keep equal-work comparisons,
actual UART measurements, endpoint-only timing and rate-model extrapolations
distinct. The evidence supports a comparison on this MCU, compiler, memory
placement and set of hot inputs. It does not certify worst-case execution,
total application CPU, strict Modbus timing or high-baud RTU frame delivery
through every serial bridge. A missing valid high-baud RTU timing result must
remain unavailable, not be filled with CPU spent rejecting partial frames.

Related: [COBS-only full matrix](COBS_PERFORMANCE.md),
[COBS wire format](PROTOCOL.md),
[RTU physical framing scope](../modbus/rtu/tests/hardware/h7s/README.md),
[shared-policy validation](SHARED_POLICIES_VALIDATION.md).
