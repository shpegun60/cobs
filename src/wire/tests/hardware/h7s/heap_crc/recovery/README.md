<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Heap OOM correction and live recovery, 2026-09-12

Latest full repeat: [extension-contract receipt](results_extensions_2026-09-12/session.json).
All 12 refusal/recovery cases, 42 exact frames and the intentionally unchanged
global-new abort control passed again. Original flash was restored and read
back; [cross-stack coverage](../../../../../../../doc/HARDWARE_EXTENSIONS_2026-09-12.md)
also includes the fresh DMA, FreeRTOS, COBS/RTU and TCP sessions.

**Fixed for `wire::Heap`: actual exhaustion now returns failure to COBS/RTU
and both resume after memory is released.** Global C++ allocation operators
are deliberately unchanged. The direct `new(std::nothrow)` negative control
still enters the nano runtime's real `abort` and `_exit(1)`.

## Production change

Only [shared Heap storage](../../../../../Storage.h) changes allocation
backend: `std::malloc` paired with `std::free`, with no new state, public
policy types, protocol fields or runtime dispatch. COBS, RTU and UART
production code are unchanged. The later CRC8 compile-time cleanup is described
below; CRC16 behavior is unaffected. Existing `Endpoint<Memory, Format, ...>`,
Message/Packet ownership and wire formats remain intact.

RX accepts fundamental alignment up to `alignof(std::max_align_t)` and asks
malloc for `max(bytes, Geometry::alignment)`. TX asks for `max(bytes, 1)`
but returns the original exact `granted == bytes`, including zero. Oversize
requests fail before malloc. This avoids malloc(0)'s implementation-defined
result and preserves RX alignment even on weak-alignment runtimes: the
allocation is large enough for the requested fundamental alignment.
See [C23 draft N3096, 7.24.3](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3096.pdf)
for allocation-size/alignment and null-on-failure semantics, and the
[C++ allocation contract](https://eel.is/c++draft/c.malloc) for the separation
from global new/delete. No object destructor or Packet lifetime was removed:
Heap allocates raw storage; protocol code still constructs/destroys its own
headers and owns their lifetimes.

Compatibility note: custom global new/delete overrides and new_handler no
longer control `wire::Heap`. Configure the C allocator or provide a custom
Storage when those hooks were previously used. DMA-accessible TX placement,
execution-context rules and arbitrary allocator latency are separate from
this OOM correction; the generated production linker was not modified.

## Live results

Source: [raw stages, exact wire bytes, source/image hashes and restoration](results_2026-09-12/session.json).

| Cases | What was forced | Result |
|---|---|---|
| `C`, `R`, `F` | TX construction OOM: COBS, burst RTU, framed RTU | Invalid Message returned; all three endpoints subsequently send/receive |
| `c`, `r`, `f` | Three consecutive RX allocations fail while an earlier Packet is retained | Exactly three allocation failures; no new packet/CRC error; retained data intact; next frame accepted after freeing |
| `G`, `g`, `J` | `reserve`, `append_bytes`, `append_native`, `append_be`, `append_le` each need a failed allocation | All return false; size/capacity preserved; the same Message grows on retry and emits the intact prefix plus tail |
| `Z` | Zero, one and 128-byte RX/TX requests, available/exhausted/released heap | Correct alignment and exact grants; null/empty on failure; allocation works again |
| `M`, `P` | Direct malloc and Pool controls with the heap full | malloc returns null; Pool works; Heap recovers after releasing held allocations |
| `N` | Direct global nothrow new with the heap full | Expected original `abort -> _exit(1)`; proves no hidden global replacement |

**12 positive refusal/recovery scenarios, 42 exact wire frames, one unchanged
global-new negative control.** Every nonfatal case returns to the command
loop and answers a fresh `H` request. Each recovery checks that malloc's live
allocated-byte total (`mallinfo().uordblks`) returns to its pre-case value;
this is a byte-balance check, not an allocation count. No unexpected abort, packet
corruption, ownership imbalance or unrecovered endpoint was recorded.

The growth test checks the original `41 42` prefix plus a 32-byte tail after
retrying the same live Message. It verifies actual encoded bytes with an
independent host CRC/COBS oracle, not just size/capacity counters. The RX
test holds two references to the old Packet during exhaustion and validates
error counters and the first good post-OOM frame. Framed RTU uses private
function `0x41` and a BE16 length prefix; burst RTU receives a complete ADU.
This is endpoint execution on the MCU, with a capturing Sender and raw frame
hex reported over UART, not a fresh DMA-throughput or CPU-percentage benchmark.

The test reuses the frozen [original fault generator](../oom/bench.cpp) and
[128 KiB AXI SRAM heap backing](../heap.cpp), so the failure mechanism is
unchanged: real malloc allocations fill the arena, down through one-byte
requests. malloc/free are not replaced or forced to fail by a stub. Linker
abort/exit observers report then call the original functions. A reset starts
each case with a fresh allocator state. No runtime case is retried.

The [linked path excerpt](results_2026-09-12/allocator_path.dis) proves that
live-executed Heap RX/TX probes call malloc and free, directly or through
the compiler's Heap helper. The verifier rejects any C++ new/delete or abort
dependency in those paths. It separately checks the still-original global
new -> abort path and both forwarding observers.

Original 65,536-byte boot flash was restored and independently read back:
`a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456`.
The diagnostic image is 29,076 bytes; ELF SHA-256:
`511335837523e6119e26f97c9a960fe093e649aee34220610d74b26731e1f586`.

## Host validation and reproduction

The host storage suite now covers tiny requests and the maximum fundamental
alignment boundary. Its new `test_heap_oom.cpp` interposes malloc/free at the
ELF linker boundary around the real Heap: construction failure, all five
growth entries, RX errors, successful retries, exact bytes and balanced
ownership for COBS, burst RTU and framed RTU. The paranoid follow-up additionally
holds an old Packet and its copy through RX OOM and checks their data after
recovery. It passes with ASan+UBSan and separately at `-O3/-DNDEBUG/-flto`:
55 allocator calls, zero live blocks at completion in each build.

The complete wire suite also passes on MinGW; its CRT import indirection is
not the ELF wrapping mechanism, so that specific interposition test is
explicitly skipped there. COBS, RTU, UART and adapter host suites pass under
WSL after the production change, including their sanitizer/release variants.
Six recovery-verifier tests reject missing recovery, silent hangs, corrupted
wire bytes, failed MCU checks, aborts in corrected paths and wrong ELF calls.

```powershell
& 'C:/Program Files/Git/bin/bash.exe' src/wire/tests/hardware/h7s/heap_crc/recovery/build.sh
python -B src/wire/tests/hardware/h7s/heap_crc/recovery/test_verify.py
python -B src/wire/tests/hardware/h7s/heap_crc/recovery/run.py `
  --port COM6 --serial 002A001F3033510135393935 `
  --output path/to/a-new-recovery-record
python -B src/wire/tests/hardware/h7s/heap_crc/recovery/verify.py `
  src/wire/tests/hardware/h7s/heap_crc/recovery/results_2026-09-12 --local-images
```

The [pre-fix OOM receipt](../oom/results_2026-09-12/session.json) and
[earlier performance tables](../../../../../../../doc/HEAP_AND_HARDWARE_CRC.md)
remain historical evidence about their recorded sources. They were not
rewritten or silently relabeled as measurements of the new Heap backend.

## Pre-commit paranoid repeat

The [fresh independent board session](results_audit_2026-09-12/session.json)
repeated all 13 cases without a runtime retry: 12 positive refusal/recovery
cases, 42 exact wire frames, and the intentionally unchanged global-new abort
control. A new original-flash backup was taken; restoration and independent
read-back again matched the SHA-256 above. The production header and measured
recovery harness did not change between the two live sessions.

The follow-up host/compile checks also passed:

- WSL ASan/UBSan and optimized release suites: wire, CRC, COBS, burst/framed
  RTU, UART and adapters; compiled/running integration documentation examples.
- MinGW 13 wire suite and strict `-O3/-flto` consumer/API matrix; real COBS
  and RTU qmake consumers; Qt adapter and reference-server host tests.
- MSVC x64/x86 shared-storage, CRC, protocol parity, COBS and RTU suites,
  now with `/WX` (warnings are errors).
- ARM M0/M0+/M3/M4/M7/M23/M33/M55 endian/alignment/codegen matrix:
  96 scalar, 96 protocol and 48 COBS objects, plus CRC table/layout guards.
- STM32 UART F1/G4/H7RS portability builds; hardware-CRC M7 builds at
  `-Os/-O2/-O3`, default/strict alignment. The CRC guard now requires `REV`
  and word/byte writes and rejects helper calls in the default build.
- All 19 negative-oracle tests across performance, original OOM and recovery
  verifiers; full local ELF/transfer-log checks on the saved performance and
  OOM receipts. All five performance tables (35 rows) were also recomputed
  directly from raw MCU timing/telemetry without importing the report helpers.

These are bounded regression checks, not a claim of worst-case allocator
latency, exhaustive MCU-family coverage or a new post-fix throughput result.

MSVC exposed one pre-existing build-hygiene issue in reflected CRC8 Table:
warning C4333 on `value >> 8`, although integral promotion makes that
expression well-defined. The 8-bit Table case now uses an explicit
`if constexpr (width == 8)` lookup-only step for either direction. No runtime
branch, policy state, checksum parameters or wire format changed. A new host
test exhausts all 65,536 two-byte inputs in both directions with nonzero
initial/final XOR values against independent bit-level oracles. Wider CRC
calculations are in the unchanged compile-time branches.

The [final board session after that cleanup](results_final_2026-09-12/session.json)
retains its own source hashes and repeats the complete OOM/recovery plan;
it is not silently substituted into either earlier record. The final ELF
remains byte-identical to the first recovery ELF (SHA-256 above), as expected
because the changed CRC8-only branch is not instantiated by this CRC16 image.
