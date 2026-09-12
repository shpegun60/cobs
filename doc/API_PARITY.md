# COBS / Modbus RTU: end-user API parity

This contract describes the shared application vocabulary, not a common
protocol implementation. Codecs, framing, packet envelopes and diagnostics
remain protocol-specific. See [integration](INTEGRATION.md),
[storage](STORAGE.md), [COBS wire format](PROTOCOL.md) and
[RTU usage](../src/modbus/README.md).

## Common application surface

| Concern | Both protocols |
|---|---|
| Memory | `wire::Heap`, `wire::Pool<Rx, Tx>`, or one custom `Memory::For<Geometry>` |
| Integrity | the same `crc::` policies, including user-provided stateful calculators |
| Endpoint configuration | `Endpoint<Memory, Format>`; RTU optionally adds `Framer` |
| Injection | CRC object and/or `std::in_place` storage constructor arguments |
| TX builder | move-only `Message`, `size/capacity/reserve`, `append_native/be/le/bytes` |
| RX handle | shared-ownership `Packet`, `data/size/reset`, cheap copy and move |
| Readers | `read_native/be/le/bytes` in `cobs`, `modbus::rtu`, or neutral `wire` |
| Transport | identical `Sender`/`BusyQuery` delegate types and `bind/unbind` |
| Send result | one `wire::SendResult`, also exported by both namespaces and `Endpoint::SendResult` |
| Service | `poll(now_ms)`, `tx_active`, `notify_gap`, `has_packet/pop_packet` |
| Common statistics | RX `frames_received/crc_errors/oversize/allocation_failure`; TX `frames_sent/send_refused_busy/send_failed` |
| STM32 composition | `cobs::UartAdapter` / `modbus::rtu::UartAdapter`: construct, `bind/unbind/bound`, `proceed(now_ms)` |
| Sleeping FreeRTOS task | the same `uart::FreeRtosWake`, attached to the driver, not the protocol |

Both endpoints must outlive every Packet and Message they issued. All these
objects belong to one execution context; shared packet references are not
atomic. Transport delegates must not throw or re-enter the endpoint.

## Ownership and failure semantics

- `make_message(..., capacity_hint)` reserves capacity, not application data.
  Check the resulting handle and every append/reserve result. An unsuccessful
  append does not poison the message; ignoring it could send incomplete data.
- A refused append/reserve leaves the previous size, capacity and contents
  intact. Storage overgrants never raise the protocol's payload limit, and the
  exact original grant is returned to its issuing storage instance.
- `Sent` means transport acceptance, not receipt by the peer. The message is
  emptied; the endpoint owns the block until the transport reports idle and
  `poll()` reclaims it. `tx_active()` stays true until that reclamation.
- `Busy`/`Unbound` retain the message in its current building/prepared state.
  `Failed` retains a prepared, read-only frame for a byte-identical retry.
  Empty or foreign-owner messages are `Invalid`. Framed RTU also rejects data
  inconsistent with its function layout; that check precedes transport checks
  and can fill an owned prefix while leaving the message in Building state.
- A Packet copy retains the same immutable data after the original is reset.
  `notify_gap()` does not erase already completed queued packets.
- `stats()` returns a snapshot. `frames_received` counts successful queue
  publication in both protocols, not raw transport candidates or application
  calls to `pop_packet()`. Protocol-specific error counts are not interchangeable.

Neither endpoint currently uses `now_ms` internally. RTU's incomplete-frame
deadline belongs to its transport adapter; `Endpoint::poll()` alone does not
expire RTU fragments. The COBS adapter has no timer state and always returns
`no_deadline` from `deadline_in_ms()`, so both use the same FreeRTOS wait loop.

## Deliberate differences: do not hide these with aliases

1. COBS creates `make_message(hint)`; RTU creates
   `make_message(address, function, hint)`. RTU Packet additionally exposes
   `address()`, `function()`, `pdu()` and `adu()`.
2. COBS accepts arbitrary cuts via `consume()`. RTU gains `consume()` only
   with a framing policy; the default `receive_adu()` needs a complete candidate.
   Merely renaming that call would not make it a stream receiver.
3. `cobs::Format<CRC, N>` limits useful payload, while
   `modbus::rtu::Format<CRC, N>` limits the entire ADU. With CRC16 and `N=512`,
   data limits are 512 and 508 respectively. Defaults remain 253 COBS payload
   bytes and a 256-byte RTU ADU (252 function-data bytes).
4. COBS hides its service length and CRC from `data()/size()`. An RTU private
   `length_prefixed(2)` layout owns the length on TX but keeps it in function
   data: initial Message size is 2, and RX `data()` is `[BE16 length][body]`.
   Read that field explicitly before reading the body. This slice does not
   change RTU data semantics, standard function fields, or either wire format.
5. A COBS gap/detach discards input through the next delimiter; framed RTU
   restarts from its next candidate start. COBS adapter detach uses the normal
   counted `notify_gap()`; RTU detach discards its partial frame uncounted.
   Ready packets survive both. A rebind is not a promise of zero frame loss.

## Source migration in this slice

- Rename COBS `stats().rx.frames_delivered` to `stats().rx.frames_received`.
  This is a real field rename: no duplicate counter, union alias or new state.
- Existing `cobs::SendResult`, `modbus::SendResult`, `modbus::rtu::SendResult`
  and `modbus::read_*` remain valid. Result names now denote the same enum,
  and `modbus::rtu::read_*` directly names the same wire functions as COBS.
- Header-only public changes require consumers to rebuild. Binary/plugin ABI
  compatibility with separately compiled old enum types is not promised.
- Saved benchmark JSON/JSONL and their schema are historical evidence, not
  the C++ API. Their old `frames_delivered` labels and bytes remain unchanged;
  the COBS hardware harness reads the renamed field into the existing schema.

## Verification

- `sh src/wire/tests/run.sh`: compile-time exact type/reader identity;
  shared custom storage; generic public lifecycle under ASan/UBSan where
  available and separately `-O3 -DNDEBUG -flto`. Includes burst RTU, both
  standard stream roles, CRC16 Bitwise/Table/NoCrc, and the owned-prefix lock.
- `sh src/adapters/tests/run.sh`: real UART driver on fake HAL through both
  adapters, including `FreeRtosWake` on a recording kernel fake; transactional
  binding, DMA borrows, detach/destruction, recovery and scheduler calls in
  sanitized and optimized builds. Framed RTU in the COBS adapter fails to compile.
- `sh doc/examples/build.sh`: manual and adapter COBS, RTU, and both FreeRTOS
  compositions compile and execute against host fakes.
- `sh src/uart/tests/port/build.sh`: real F1/G4/H7RS HAL compile probes, including
  both adapters. COBS adapter has two references and a bound flag (12 ARM bytes),
  no timer fields; existing RTU adapter/UART layout locks remain intact.
- Existing COBS/RTU suites, ARM layout guards, Qt consumers and MSVC parity
  probes remain regression gates. These host/compile checks are not live-board
  or real-FreeRTOS runtime measurements. Earlier board evidence remains tied
  to the exact recorded source revisions.

### Slice validation, 2026-09-12

Executed on the updated working tree (not attributed to an earlier commit):

| Gate | Result |
|---|---|
| New Endpoint lifecycle parity | 244 checks, zero failures: WSL GCC ASan/UBSan and O3/LTO; MSVC x64/x86 |
| New UART adapter parity | 170 checks, zero failures in sanitized and O3 builds: COBS Bitwise/Table/NoCrc, burst/framed RTU, shared FreeRTOS wake, full DMA chunk plus split COBS tail |
| Existing adapter regressions | 100 RTU integration checks and 17 FreeRTOS wake checks passed |
| COBS / RTU regression suites | passed, including 20,361 COBS integrity checks, exhaustive codec and RTU stream fuzz gates |
| Examples and strict consumers | all seven integration examples passed under WSL and MinGW; strict GCC 13 O3/LTO consumer matrix passed |
| Qt 6.4.3 / GCC 11.2 consumers | 202 adapter checks and 22 server trace checks passed, no COM port used |
| ARM | F1/G4/H7RS driver/adapter compile matrix, existing disassembly/stack gates and both protocol layout guards passed |
| H7S firmware build | COBS harness rebuilt successfully after the counter rename; no flashing or live measurement in this slice |

The MSVC run used the Visual Studio Installer directory on the process PATH
so `VsDevCmd` can find `vswhere.exe`; it changed no machine-wide settings.
The live board and saved JSON/JSONL evidence were not modified.
