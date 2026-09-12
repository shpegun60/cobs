<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Modbus TCP core on NUCLEO-H7S3L8, carried over UART

**Latest source-tree evidence:** [extension-contract receipt](results_extensions_2026-09-12/session.json)
and [full H7S repeat report](../../../../../../doc/HARDWARE_EXTENSIONS_2026-09-12.md).
All six images passed: 24,850 core checks, 36 real OOM checks, 318 local RTU
checks, 1,974 exact UART exchanges and 38 fail-closed trials. Schema 3 adds
**2,560,991 exhaustive length-domain checks in each image**, using the same
[body as the host](../../../../../wire/tests/length_checks.h): every 16-bit
MBAP Length, every nonzero Protocol ID and every private BE16 RTU count over
the selected CRC/data limits. These are MCU-local declaration/skip checks,
not million-frame UART or CRC-arithmetic tests. All flashes verified on
attempt one, and the original boot image was restored and read back.

Earlier evidence: see [cross-stack audit repeat](#cross-stack-audit-repeat).
The [payload-limit API repeat](#payload-limit-api-repeat) records the preceding
API migration before the subsequent UART ownership correction.
The first session below used the earlier ADU-limit parameter and is preserved
as historical evidence, not attributed to the current Format units.

Session: **2026-09-12, 15:35:57 UTC**.
[Raw record](results_2026-09-12/session.json) stores the exact transmitted chunks,
received bytes, MCU self-test replies, source/image/log SHA-256 values and
restoration evidence. All six images passed. No Ethernet or TCP/IP stack was
started. These are MBAP/ownership/MCU/UART tests, **not socket interoperability**.

## Coverage and results

STM32H7S3, Cortex-M7 at 600 MHz, I/D-cache enabled; ST-Link VCP COM6,
`Uart<256, 4>` at 115200 baud. The live endpoint's ADU ceiling is 1024 bytes
to exercise both standard-sized ADUs and explicit private oversized frames.
MCU-local shared core cases separately use the standard 260-byte ceiling.

| Image | Storage | CRC policy | Core checks | OOM checks | Exact UART exchanges | Live rejection trials |
|---|---|---|---:|---:|---:|---:|
| 0-0 | Pool<8,2> | NoCrc | 4094 | 2 | 329 | 4 |
| 1-0 | Heap | NoCrc | 4094 | 14 | 329 | 4 |
| 0-1 | Pool<8,2> | CRC16 Bitwise | 4129 | 2 | 329 | 6 |
| 0-2 | Pool<8,2> | CRC16 Table | 4129 | 2 | 329 | 6 |
| 1-2 | Heap | CRC16 Table | 4129 | 14 | 329 | 6 |
| 0-3 | Pool<8,2> | CRC32 Table | 4115 | 2 | 329 | 6 |

Total: **24,690 MCU core checks, 36 MCU OOM checks, 1,974 exact UART exchanges,
32 live fail-closed trials**, zero failed checks. These categories are different
units and are intentionally not added into a single inflated test count.

Each image exchanges:

- 21 payload lengths, including empty, common boundaries and maximum payload,
  with zero, ascending and deterministic mixed-byte patterns;
- all 256 function values, also exercising all unit values and transaction IDs;
- nine deliberate frame splits, including the MBAP prefix and final byte;
- a train of three coalesced ADUs.

The Python oracle builds BE MBAP directly using `struct`, computes CRC16 with
an independent bit loop and CRC32 with Python's `zlib`. It compares every byte;
the parser's own output is not its oracle. Explicit CRC cases cover the private
extension, not standard TCP interoperability.

Live rejection trials start with a successful baseline echo, then inject an
invalid Protocol ID, zero/one Length, or oversized Length followed by a valid
ADU. CRC images also receive a flipped trailer and a plausible but corrupt
Length. No echo is allowed after the fault. An MCU reset establishes a new
test stream, and its fresh boot reply is required. No runtime trial is retried
or silently discarded. All flash operations succeeded on their first attempt.

The MCU-local [shared core body](../../core_cases.h) also tests every length
within the default capacity, byte-at-a-time assembly, all cuts of a small ADU,
retained Packet copies, queued packets across gap/reset, active TX borrows,
Failed retry immutability and corruption bits. Pool exhaustion is real.
Heap images exhaust the actual newlib malloc arena, verify refused creation,
same-message failed growth and RX OOM, release pressure, skip the known tail,
then grow/send/receive successfully. Allocation failure is not simulated there.

The test-only [heap backing arena](../../../../../wire/tests/hardware/h7s/heap_crc/heap.cpp)
is 128 KiB of AXI SRAM, so Heap and Pool TX are DMA-readable. The real malloc/free
implementation and production UART/cache paths are unchanged. This is not a
claim that arbitrary MCU memory is DMA-accessible.

## Exact inputs and restoration

The record starts from `ef44ea36e35b2641e831c705dd556d005b7a9f67` plus the recorded
**uncommitted TCP implementation/harness bytes**. Do not attribute this new TCP
coverage to that older commit alone. Source hashes identify the actual inputs.
The six binaries are 29,240..30,952 bytes; complete ELF/BIN/MAP/DIS/NM files and
programmer logs are retained in the ignored local session named in the record.

The entire 64 KiB boot flash was backed up before any image write. After the
matrix it was restored and freshly read back. Both SHA-256 values are:

```text
a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456
```

## Reproduce and recheck

Requirements: the existing ignored Cube project, GNU Arm 14.3.1, CubeProgrammer,
Python with pyserial, exclusive access to the board and COM port. The runner
never regenerates Cube files, refuses to overwrite evidence, builds before
touching flash and restores in `finally`, including when a trial fails.

```powershell
python src/modbus/tcp/tests/hardware/h7s/run.py `
  --port COM6 --serial 002A001F3033510135393935 `
  --output src/modbus/tcp/tests/hardware/h7s/results_new/session.json

python src/modbus/tcp/tests/hardware/h7s/verify.py `
  src/modbus/tcp/tests/hardware/h7s/results_audit_2026-09-12/session.json --local
```

Without `--local`, verification checks the complete raw-byte plan, replies,
restoration consistency and record shape without requiring local build files.
With `--local`, it additionally checks all recorded artifacts, programmer logs
and source bytes against files still on this machine. A later source change
will intentionally fail that exact-tree mode; use the recorded bytes to
reproduce the measured tree, not a timestamp or branch-name inference.

## Other platforms and remaining boundary

Host: 74,056 core checks and 18,284 advanced checks per configuration,
ASan/UBSan + O3/LTO under WSL GCC; MinGW 13, MSVC x64/x86 `/WX`; qmake consumer.
All nine CRC policies (NoCrc and CRC8/16/32/64 Bitwise/Table), stateful wrapping
sum, under/overgrants, exact owner release, random chunks and the maximum
65541-byte ADU are covered on the host. CRC8/64 TCP paths and maximum 65541-byte
frames were not run on this board in this session.

ARM: 72 default-core objects across M0/M0+/M3/M4/M7/M23/M33/M55,
Os/O2/O3, little/big/strict access modes, plus CRC16 Bitwise/Table controls.
NoCrc objects contain no CRC engine/table, HAL, heap or floating-point helper
dependency. Bitwise emits no table; Table emits its 512-byte lookup.
Inspected `transaction_id()` code: M7 little-endian uses `ldrh` + `rev16`,
big-endian needs no swap, M0/strict mode uses byte loads. These are compile/
disassembly checks, not execution on all eight cores or a benchmark.

Unchanged COBS, RTU, CRC, shared storage, adapter and UART host suites plus
integration examples passed as regressions. This slice adds no network socket
adapter, partial-write handling, connection scheduler, Ethernet hardware test,
client/server transaction engine or TCP/IP conformance claim.

## Payload-limit API repeat

The [current final receipt](results_payload_limits_final_2026-09-12/session.json)
started at **2026-09-12 16:10:06 UTC**, after the RTU/TCP Format argument became
useful data size. It is separate from both the earlier ADU-limit session and
the [first passing data-limit run](results_payload_limits_2026-09-12/session.json).
The repeat also captures final explanatory header/verifier changes in its
source hashes. No old record was edited to claim it measured new source bytes.

Every live TCP image now uses **1024 useful data bytes**. The hello reply
explicitly reports the resulting ADU maximum, proving the limit's unit:

| Image | Data capacity | Derived TCP ADU | TCP core checks | OOM checks | RTU data checks | UART exchanges | Live rejections |
|---|---:|---:|---:|---:|---:|---:|---:|
| Pool / NoCrc | 1024 | 1032 | 4094 | 2 | 53 | 329 | 5 |
| Heap / NoCrc | 1024 | 1032 | 4094 | 14 | 53 | 329 | 5 |
| Pool / CRC16 Bitwise | 1024 | 1034 | 4161 | 2 | 53 | 329 | 7 |
| Pool / CRC16 Table | 1024 | 1034 | 4161 | 2 | 53 | 329 | 7 |
| Heap / CRC16 Table | 1024 | 1034 | 4161 | 14 | 53 | 329 | 7 |
| Pool / CRC32 Table | 1024 | 1036 | 4179 | 2 | 53 | 329 | 7 |

Totals for this final receipt only: **24,850 TCP core checks, 36 OOM checks,
318 RTU data checks, 1,974 byte-exact UART exchanges, 38 live rejection trials**.
All passed. The added live rejection advertises precisely one data byte over
the configured capacity; a valid frame after it is still not accepted until
the next test stream. The maximum positive case sends all 1024 data bytes.

RTU checks execute on the MCU over Heap/Pool and the selected policy. They
compare transmitted bytes against an independent address/function/data/CRC
oracle for data sizes 0/1/7/252/255/256/1024, receive each complete ADU back,
and reject a 1025-byte data hint/extra append. **These RTU checks are local
MCU loopbacks, not a new RTU UART matrix**; UART exchanges here carry TCP ADUs.
The existing RTU UART harness keeps its physical benchmark budgets explicitly.

Current host results: 75,016 TCP core, 18,284 advanced, 1,620 TCP data-limit
checks; additionally 2,916 shared checks across 324 COBS/RTU/TCP combinations.
The nine TCP and eleven RTU compile-fail boundaries reject overflow and
contract violations. Full RTU/shared/COBS/CRC/UART/adapter regressions and
integration examples passed, along with MSVC x64/x86, MinGW, qmake and the
ARM guards described in [payload limits](../../../../../../doc/PAYLOAD_LIMITS.md).
The receipt verifier rejects 16 intentionally corrupted original-schema
records and 20 data-limit-schema corruptions; it accepts both complete controls.

All six final image flashes succeeded on attempt one. Binaries were
30,892..33,240 bytes. The complete 64 KiB original boot image was restored;
backup and fresh read-back again matched
`a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456`.
No network stack, client transaction manager or custom MBAP framer was added.

## Cross-stack audit repeat

The later [contract-fix receipt](results_contracts_2026-09-12/session.json)
repeats all six images after the shared CRC-call and enum-reader fixes:
24,850 TCP checks, 36 OOM checks, 318 local RTU checks, 1,974 exact UART
exchanges and 38 live rejection trials, all passed. All flashes succeeded
on their first attempts; the fresh boot backup was restored and read back.
The [follow-up report](../../../../../../doc/CONTRACT_HARDENING_2026-09-12.md)
also covers the new FreeRTOS waits, COBS/RTU matrix and ARM evidence.

The [fresh record](results_audit_2026-09-12/session.json) started at
**2026-09-12 17:10:34 UTC**, after the UART DMA-ownership fixes described in
the [cross-stack audit](../../../../../../doc/PARANOID_AUDIT_2026-09-12.md).
All six images passed with the same data-limit coverage and counts in the
table immediately above: **24,850 core + 36 OOM + 318 local RTU checks**,
**1,974 byte-exact UART exchanges**, **38 live fail-closed trials**. The
1024-byte limit still means useful data, not ADU size. Pool/Heap and the
explicit private CRC extensions use unchanged protocol/storage semantics.

All six flashes succeeded on attempt one; binaries are **31,000..33,348
bytes**. The original 64-KiB boot image was restored and its actual backup
and fresh read-back files were rehashed, both matching the SHA-256 above.
The verifier's `--local` mode passes for this record, including the final
UART source and all retained ELF/BIN/MAP/DIS/NM artifacts and log hashes.
Earlier successful receipts remain intact and identify their earlier bytes;
they must not be relabelled as tests of the fixed UART. This repeat adds no
Ethernet/socket or CPU-performance claim.
