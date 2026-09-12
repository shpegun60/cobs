<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# COBS / RTU API parity on the H7S with real FreeRTOS

This correctness harness runs the ordinary application loop:

```cpp
(void)uart::FreeRtosWake::wait(adapter);
adapter.proceed();
```

It uses the actual UART/DMA driver, HAL, `Uart<256,4>`, Pool storage and
FreeRTOS V10.6.2, not the host fake kernel. One communication task owns the
driver, endpoint, Packets and Messages. The ISR only notifies the task;
protocol processing and buffer reclamation run in task context.

## Dependencies and configuration

Use the existing ignored `stm32_cube_test/h7s_cobs_test` Cube scaffold and
STM32Cube H7RS V1.3.0's separately installed `Middlewares/Third_Party/FreeRTOS/Source`.
`build.sh` accepts `ARM_TOOLS`, `H7S_CUBE_PROJECT`, and `FREERTOS_SOURCE` overrides.
The guarded Windows runner records and verifies the default local installation
paths printed in its source; changing them requires matching the builder too.
Nothing is installed, vendored or modified in the Cube or kernel directories.

- Cortex-M7, 600 MHz, I/D caches and UART FIFO enabled, normal DMA.
- GCC ARM_CM7/r0p1 kernel port; 1 kHz SysTick drives HAL and FreeRTOS time.
- Static task/idle stacks and TCBs, no heap implementation. Build rejects
  allocator symbols and the runner refuses a load image above 64 KiB.
- USART3, RX DMA and TX DMA IRQs use HAL priority 6, below the kernel's
  syscall ceiling of 5. Kernel priority assertions and stack checks stay on.
- Protocol selector 0 = COBS, 1 = burst RTU, 2 = framed RTU.
- CRC selector 0 = NoCrc, 1 = CRC16 Bitwise, 2 = CRC16 Table.
- RTU uses private function 0x41 with a BE16 body-length prefix. Framed TX
  owns that prefix; burst TX writes it explicitly. This is not a test of
  standard function semantics (the separate Qt matrix tests those).

## Run and independently verify

Close every other owner of the ST-Link and COM port first. The runner backs
up the current 64 KiB boot region **before** flashing, verifies every download,
and restores the fresh backup in `finally`, followed by full read-back/hash
comparison. Test firmware stays inside that boot region; option bytes and
external flash are untouched. Runtime failures are not automatically retried.

```powershell
python -B src/adapters/tests/hardware/h7s/parity/run.py --port COM6 --serial <STLINK_SERIAL> --output <new_directory>
python -B src/adapters/tests/hardware/h7s/parity/verify.py <new_directory> --local-images
python -B src/adapters/tests/hardware/h7s/parity/test_verify.py <new_directory>
```

Default matrix: COBS with all three policies, burst RTU with CRC16 Bitwise,
framed RTU with all three policies; each at 115200 and 1M (14 images).
`--bauds 115200` intentionally selects the smaller seven-image matrix and
is recorded as such. Existing protocol runners cover all nine CRC variants
and COBS/framed RTU through 10M; this harness isolates the new API/task path.

Each image exercises 8 boundary/vector echoes (including empty and maximum),
32 further echoes, real RX/TX notification wakes, idle/fallback sleeps,
invalid CRC and recovery where enabled, and refusal to rebind/detach while
TX is borrowed. Stream modes additionally hold one Packet while detaching a
partial RX frame, check Pool availability 2 -> 3 -> 4, rebind and recover.
Framed RTU also drops an orphan before a new frame arrives 25 ms later,
proving the 5 ms adapter deadline bounds the default 50 ms wait.

The separate 20-check local Endpoint test executes on the MCU but not through
UART. It covers `Invalid/Unbound/Busy/Failed/Sent`, byte-identical prepared
retry, endian writers/readers, bounds/strong guarantee, shared Packet lifetime,
TX completion polling and complete Pool reclamation.

The control envelope is `B6 50 52 54 <command>`; replies contain 24 LE32 words
whose layout is declared next to `status()` in `parity_bench.cpp`. Raw TX/RX
hex is retained for every exchange. `verify.py` separately decodes COBS/RTU
and CRC, requires the exact case plan and all device counters, verifies
source provenance, and with `--local-images` re-reads ELF/bin/build/flash/backup
and external-kernel bytes. Its mutation test rejects damaged cases even when
they carry a syntactically valid re-encoded status frame.

## Recorded result

[Complete receipt](results_final_2026-09-12/session.json),
[raw exchanges](results_final_2026-09-12/results.jsonl): **14 images, 704
exchanges, 560 exact echoes, 280 MCU-local checks**, zero device assertion,
ISR-context or task-context failures. Original boot firmware restored and
verified. `test_verify.py` passes 67 mutation/oracle checks.

The [initial stopped receipt](results_2026-09-12/session.json) is deliberately
retained as **incomplete**, not re-labelled successful. Its first image's
device assertions and exchanges were correct; the new host verifier had
incorrectly required `uart.restarts == 0`, although `Uart::init()` calls
`receiveRestart()` once. It also needed to decode the zero in a partial COBS
prefix before checking those payload bytes. Both oracle assumptions were
corrected; firmware/library bytes were unchanged, and the full matrix was
repeated from its first image in a new directory. The original receipt still
fails the completed-matrix gate. Its first ELF/bin and source manifest are
byte-identical to the successful repeat's corresponding first image. See
the [full report](../../../../../../doc/HARDWARE_API_PARITY_2026-09-12.md).

This is an instrumented correctness run, not a CPU-utilization benchmark or
a guarantee for other kernel versions, task priorities, clocks or MCU ports.
