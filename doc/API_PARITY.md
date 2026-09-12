# COBS / Modbus RTU / Modbus TCP: end-user API parity

This contract describes the shared application vocabulary, not a common
protocol implementation. Codecs, framing, packet envelopes and diagnostics
remain protocol-specific. See [integration](INTEGRATION.md),
[storage](STORAGE.md), [COBS wire format](PROTOCOL.md) and
[RTU usage](../src/modbus/README.md).

## TCP extension (2026-09-12)

[`modbus::tcp::Endpoint<Memory, Format>`](../src/modbus/tcp/README.md) preserves
the same Message writers (scalar and span), Packet ownership, exact shared
`read_*` functions, `wire::SendResult`, delegates, bind/send/poll lifecycle,
storage and stateful CRC injection. It adds no parallel memory API.
The COBS/RTU adapter-specific sections below remain specific to those adapters;
this slice adds no TCP socket adapter or TCP-specific FreeRTOS glue.

| Protocol fact | RTU | TCP |
|---|---|---|
| Factory metadata | `address, function[, hint]` | `transaction_id, unit_id, function[, hint]` |
| Format | `Format<Crc = Crc16Bitwise, MaxData = 252>` | `Format<Crc = NoCrc, MaxData = 252>` |
| Default function data | 252 bytes | 252 bytes |
| Stream framing | optional function-layout policy | always MBAP Length, no custom framer |
| CRC | standard CRC16 by default | absent in standard TCP; opt-in CRC is private |
| Service length | private RTU owned prefixes stay in data | MBAP Length never enters data |
| Gap recovery | protocol/framer candidate rules | failed until explicit reset at a known new stream |
| Packet metadata | `address/function` | `transaction_id/unit_id/function` |

Both Packet variants expose `data/size/pdu/adu`. TCP hides the optional trailer
from data/PDU and includes it in ADU. It does not inherit RTU's candidate-only
`receive_adu()` or its timing-expiry methods: a byte stream has different
boundary guarantees. `assembling`, `rx_failed`, `reset_rx` expose those facts.
Packet, Message and layout internals stay protocol-specific. Bitwise/Table
of equal width share TCP Storage/Message/Packet types, just as in RTU/COBS.

`src/modbus/tcp/tests/test_advanced.cpp` checks exact reader/result identity,
type parity, all six scalar/span writer forms, metadata and ownership behavior;
the common MCU/host suite checks send failures, immutable retries and borrows.

## Common application surface

| Concern | All three protocol cores (adapter rows apply to COBS/RTU) |
|---|---|
| Memory | `wire::Heap`, `wire::Pool<Rx, Tx>`, or one custom `Memory::For<Geometry>` |
| Integrity | the same `crc::` policies, including user-provided stateful calculators |
| Endpoint configuration | `Endpoint<Memory, Format>`; only RTU optionally adds `Framer` |
| Injection | CRC object and/or `std::in_place` storage constructor arguments |
| TX builder | move-only `Message`, `size/capacity/reserve`, `append_native/be/le/bytes` |
| RX handle | shared-ownership `Packet`, `data/size/reset`, cheap copy and move |
| Readers | `read_native/be/le/bytes` in `cobs`, `modbus::rtu`, `modbus::tcp`, or neutral `wire` |
| Transport | identical `Sender`/`BusyQuery` delegate types and `bind/unbind` |
| Send result | one `wire::SendResult`, also exported by all three namespaces and `Endpoint::SendResult` |
| Service | `poll(now_ms)`, `tx_active`, `notify_gap`, `has_packet/pop_packet` |
| Common statistics | RX `frames_received/crc_errors/oversize/allocation_failure`; TX `frames_sent/send_refused_busy/send_failed` |
| STM32 composition | `cobs::UartAdapter` / `modbus::rtu::UartAdapter`: construct, `bind/unbind/bound`, `proceed()` |
| Sleeping FreeRTOS task | the same `uart::FreeRtosWake::wait(adapter)`, wake attached to the driver, not the protocol |

Every endpoint must outlive every Packet and Message it issued. All these
objects belong to one execution context; shared packet references are not
atomic. Transport delegates must not throw or re-enter the endpoint.

The same reader constraints apply in every namespace: integer/floating/byte
fields and scoped enums (`enum class`), but no unscoped enum. Read an integer
and validate it before converting to an unscoped enum; C++20 has no portable
fixed-underlying-enum trait. No runtime enum-range checks or cursor state are
added. A CRC policy's checksum is passed to `store` as a const lvalue in all
three protocols, matching the `crc::Policy` exception contract.

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
  publication in all three protocols, not raw transport candidates or application
  calls to `pop_packet()`. Protocol-specific error counts are not interchangeable.

None of the three endpoints currently uses `now_ms` internally. RTU's incomplete-frame
deadline belongs to its transport adapter; `Endpoint::poll()` alone does not
expire RTU fragments. The COBS adapter has no timer state and always returns
`no_deadline` from `deadline_in_ms()`, so both use the same FreeRTOS wait loop.

The ordinary application loop needs neither a timestamp nor a deadline query:

```cpp
(void)uart::FreeRtosWake::wait(adapter);
adapter.proceed();
while (auto packet = endpoint.pop_packet()) { handle(packet); }
```

The STM32 adapter includes the configured `main.h` for its HAL clock. It
reads a fresh `HAL_GetTick()` when `proceed()` is called, after waking;
the FreeRTOS wake layer itself has no HAL dependency. `wait(adapter)` chooses
the shorter of the adapter's current deadline and its default 50 ms fallback.
`wait(adapter, 20u)` selects a custom fallback. COBS's constant no-deadline
query compiles to the fallback without a HAL clock read.

For custom schedulers or deterministic tests, `proceed(now_ms)` and
`deadline_in_ms(now_ms)` remain available. Use a single monotonic millisecond
clock domain consistently; do not mix a synthetic timestamp with automatic
HAL-time calls. The raw `FreeRtosWake::wait(milliseconds)` also remains valid.
These conveniences add no object state, virtual dispatch, or allocation.

## Deliberate differences: do not hide these with aliases

1. COBS creates `make_message(hint)`; RTU creates
   `make_message(address, function, hint)`. RTU Packet additionally exposes
   `address()`, `function()`, `pdu()` and `adu()`.
2. COBS accepts arbitrary cuts via `consume()`. RTU gains `consume()` only
   with a framing policy; the default `receive_adu()` needs a complete candidate.
   Merely renaming that call would not make it a stream receiver.
3. All three `Format<CRC, N>` spellings now limit useful `data()` bytes.
   Headers, CRC and encoding overhead are added automatically. With CRC16
   and `N=512`, all three expose 512 data bytes; RTU/TCP derive ADUs of
   516/522 bytes. Defaults remain 253 COBS payload bytes, standard RTU
   CRC16 with 252 data / 256 ADU and standard TCP NoCrc with 252 data / 260 ADU.
   See [migration and exact units](PAYLOAD_LIMITS.md).
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

### Initial host slice, 2026-09-12

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
At that initial host checkpoint the board and previous JSON/JSONL evidence
were not modified. The subsequent live follow-up is recorded separately below.

### Live follow-up and platform-clock facade, 2026-09-12

See [the hardware report](HARDWARE_API_PARITY_2026-09-12.md) and its exact
source/image receipts. It includes the 33-image protocol regression, targeted
UART faults, high-baud framing, and **14 real FreeRTOS V10.6.2 images** running
the two-line loop above on the H7S. The latter passed 704 exchanges, 560 exact
echoes and 280 MCU-local ownership checks; no fake kernel runs on that board.

The updated host adapter suite passes 105 RTU checks (including implicit HAL
time and wraparound), 24 wake checks and 170 common adapter checks. All seven
examples and the real F1/G4/H7RS compile/layout matrix also pass. Historical
evidence and earlier-stage counts above remain stage-specific, not claims
that an old binary contained the latest facade.
