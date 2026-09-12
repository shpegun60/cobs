<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# H7S: application API parity and the platform-clock FreeRTOS loop

This follows [the API parity slice](API_PARITY.md) and is separate from the
[earlier paranoid-audit hardware regression](HARDWARE_REGRESSION_2026-09-12.md).
The first matrix measures the API slice based on `bf29d30`, committed as
`65d4cf7`. The following runs start at `65d4cf7` plus the platform-clock facade
and real-FreeRTOS harness. Source hashes identify the exact measured bytes;
the base SHA alone is not a claim that it already contained those changes.

NUCLEO-H7S3L8, STM32H7S3 Rev Y, M7 at 600 MHz; ST-Link
`002A001F3033510135393935`, V3J17M11, COM6; STM32CubeProgrammer 2.21.0,
GNU Arm 14.3. The ignored Cube scaffold and optional installed FreeRTOS
sources are not modified. The same 64 KiB original boot image was backed up
fresh for each session and restored/read back; SHA-256:
`a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456`.

## What the application writes now

```cpp
(void)uart::FreeRtosWake::wait(adapter);
adapter.proceed();
while (auto packet = endpoint.pop_packet()) { handle(packet); }
```

No application `std::min`, deadline query, or `now` argument is required.
The STM32 adapter owns the HAL clock. `proceed()` samples it after the wait;
framed RTU's deadline shortens that wait automatically. COBS has no deadline
state. `wait(adapter, 20u)` overrides the default 50 ms fallback. Explicit
clock forms remain for custom loops/tests; the FreeRTOS wake itself remains
independent of HAL. Do not mix HAL and synthetic clock domains in one loop.

## Completed live gates

| Gate | Coverage | Evidence |
|---|---|---|
| Base COBS / RTU matrix | 33 images; 93 COBS + 144 RTU suite records | [receipt](../src/wire/tests/hardware/h7s/results_api_parity_2026-09-12/session.json) |
| Targeted UART / RTU faults | `-Os/-O2/-O3`, 132 trials, 1623 device assertions | [receipt](../src/adapters/tests/hardware/h7s/results_api_parity_2026-09-12/session.json), [raw trials](../src/adapters/tests/hardware/h7s/results_api_parity_2026-09-12/results.jsonl) |
| High-baud framing | 8 images, 24 records; framed 192/192 boundary trials and all vectors | [record](../src/modbus/rtu/tests/hardware/h7s/results_framing_api_parity_2026-09-12.jsonl), [receipt](../src/modbus/rtu/tests/hardware/h7s/results_framing_api_parity_2026-09-12.jsonl.session.json) |
| Real FreeRTOS application loop | 14 images, 704 exchanges, 560 exact echoes, 280 MCU-local checks | [receipt](../src/adapters/tests/hardware/h7s/parity/results_final_2026-09-12/session.json), [raw exchanges](../src/adapters/tests/hardware/h7s/parity/results_final_2026-09-12/results.jsonl), [harness](../src/adapters/tests/hardware/h7s/parity/README.md) |
| QtSerialBus interop | 8 flashed role-runs, 660 reference-model verdicts, zero unexpected outcomes | [record](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_api_parity_2026-09-12.json), [receipt](../src/adapters/qt/tests/hardware/h7s/results_qmodbus_api_parity_2026-09-12.json.session.json) |

The base matrix uses COBS CRC16 Bitwise/Table at 253 bytes and NoCrc at 1024
bytes over 115200/1M/3M/6M/10M; RTU runs all nine policies at 115200/1M.
Vectors, corruption, gaps/BREAK, Pool exhaustion, borrowed TX, recovery and
stress pass the existing independent negative-counter/ownership guards.
The COBS hardware harness now binds through the actual `cobs::UartAdapter`;
its split service calls remain only to preserve timing scopes.

Targeted faults repeat the real DMA progress/lost-publication/DMAR-off cases
from the earlier audit, including the slow 9600-baud multi-chunk transfer,
with no change to production ISR, DMA or cache-maintenance logic.

The burst-RTU control deliberately remains limited: at 6M its smoke and
vectors fail, and at 10M its vectors fail when the VCP splits candidates.
Split/glued inputs are not promised by `framing::None`. Those failures remain
in the record; they are **not** described as passing stream reception.
Framed RTU passes single/split/glued/orphan-recovery trials at every tested
baud through 10M. Configured baud is not proof of continuous VCP throughput.

Qt 6.4.3 interoperability repeats 115200/1M and burst/framed MCU endpoints in
both client/server roles. Of 660 verdicts, 652 are `ok` and 8 are the required
unknown-function timeouts (not retried or counted as successful replies).
The independent verifier confirms every outcome against the reference model.
The USB-aware native Qt server fragment deadline stays at its established
50 ms; this run does not silently alter request retries, baud or wire data.

## Real kernel, not a fake notification API

All 14 images use FreeRTOS **V10.6.2**, STM32Cube H7RS V1.3.0's GCC
ARM_CM7/r0p1 port, a 1 kHz SysTick and statically allocated tasks/stacks.
There is no linked heap implementation; build guards reject allocator
symbols. UART/DMAs use IRQ priority 6 with syscall ceiling 5; kernel priority
assertions and stack checks are enabled. I/D caches remain enabled.
Load images are 26,312..28,028 bytes; every one fits the 64 KiB boot region.

The selected variants are COBS and framed RTU with NoCrc/CRC16 Bitwise/Table,
plus burst RTU with CRC16 Bitwise, each at 115200 and 1M. Real task
notifications, idle-hook execution and timed-out waits increase as expected.
All device assertions, wrong-ISR-context and wrong-task-context counters stay
zero. Exactly one UART RX start is recorded (the initial `init()`), with no
additional recovery restart during this matrix.

The tests cover active-TX detach/rebind refusal, Packet retention across
partial-frame detachment, exact Pool return, invalid CRC/recovery, empty and
maximum frames, and a 25 ms orphan/new-frame interval in framed RTU. The
latter requires servicing the 5 ms deadline before the 50 ms fallback; the
application supplies no timestamp/deadline arithmetic.

The 20 checks per image labelled MCU-local use Endpoint loopback on the
processor, not UART. Actual wire echo counts are reported separately.
`verify.py` re-decodes raw wire bytes and requires all named cases and
device counters. `test_verify.py` passes **67 mutation/oracle checks**, not
merely a saved aggregate pass flag.

The initial one-image run is [retained as incomplete](../src/adapters/tests/hardware/h7s/parity/results_2026-09-12/session.json).
The new verifier incorrectly expected zero RX starts and then tested an
un-decoded COBS partial prefix. Inspection of `Uart::init()` and raw
telemetry identified the oracle mistakes. No firmware/library change was
needed; a fresh full run passed after fixing the verifier. The stopped
receipt is not relabelled or spliced into the successful matrix.
The initial and repeated first ELF/bin bytes and source manifests match
exactly. No programming retry was needed in the complete FreeRTOS matrix.

## Host and code-generation follow-up

- WSL GCC: 105 RTU adapter, 24 wake and 170 shared adapter checks; sanitized
  and optimized adapter builds. All seven executable integration examples
  pass under WSL and MinGW GCC 13. The portable real-FreeRTOS receipt/source
  verifier also passes under WSL; Windows `--local-images` verifies the
  retained binaries, flash/restore logs, backup and installed kernel sources.
- The HAL-clock test starts near `UINT32_MAX`, receives a partial frame,
  crosses wraparound and expires it with only parameterless adapter calls.
- F1/G4/H7RS real-HAL compile probes exercise both explicit and implicit
  clock forms. Layout locks remain COBS adapter 12 ARM bytes, RTU adapter 32,
  `Uart<256,4>` 1696; no state was added for clock convenience.
- The flashed COBS CRC16/115200 `-Os` disassembly has the wait folded to
  `r2=50, r1=1, r0=0; bl ulTaskGenericNotifyTake`; `HAL_GetTick` is read only
  afterwards for the service pass. There is no COBS deadline calculation or
  clock read on its wait path. This is an inspection of that exact M7 image,
  not a claim that every compiler emits identical instructions.

These are correctness/integration measurements, not a new CPU percentage
benchmark and not proof for every possible FreeRTOS configuration or ARM port.

## Recheck without flashing

```text
python -B src/wire/tests/hardware/h7s/verify_fault_matrix.py src/wire/tests/hardware/h7s/results_api_parity_2026-09-12 --local-images
python -B src/adapters/tests/hardware/h7s/verify.py src/adapters/tests/hardware/h7s/results_api_parity_2026-09-12 --local-images
python -B src/modbus/rtu/tests/hardware/h7s/verify_framing.py src/modbus/rtu/tests/hardware/h7s/results_framing_api_parity_2026-09-12.jsonl
python -B src/adapters/tests/hardware/h7s/parity/verify.py src/adapters/tests/hardware/h7s/parity/results_final_2026-09-12 --local-images
python -B src/adapters/tests/hardware/h7s/parity/test_verify.py src/adapters/tests/hardware/h7s/parity/results_final_2026-09-12
python -B src/adapters/qt/tests/hardware/h7s/verify_qmodbus.py src/adapters/qt/tests/hardware/h7s/results_qmodbus_api_parity_2026-09-12.json
```

ELF/bin/build/flash/backup files stay in the ignored local session directories;
receipts contain their hashes. `--local-images` requires those local files;
portable verification explicitly reports when it cannot re-read them.
