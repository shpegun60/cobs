<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Extension contracts and follow-up audit — 2026-09-12

Baseline: `3d177cf3b47c22715803c9bec26f621d69d60cb7`. This slice fixes
compile-time admission of custom storage and RTU framers. It does not
change the wire format, fields, ownership, algorithms, UART or adapters.
The earlier CRC/reader/RTOS fixes and their live-board evidence remain in
[CONTRACT_HARDENING_2026-09-12.md](CONTRACT_HARDENING_2026-09-12.md);
those hardware runs are not newly measured results for this slice.

## Reproduced failures and fixes

`wire::ByteStorage` originally tested only const lvalue arguments. All three
protocols issue computed RX-size expressions, return computed RX pointers,
and release mutable TX descriptors. An overload taking one of those other
argument forms could be deleted, return the wrong type or be
`noexcept(false)` while the concept still accepted the storage. The local
reproducer selected unchecked overloads in COBS, RTU and TCP; an actual
throw entered `std::terminate` in each endpoint.

The concept now checks const lvalues, mutable lvalues and temporaries for
all four operations. Normal value-taking methods and const-reference
implementations remain valid. No forwarding layer, fields or runtime
checks were introduced. This enforces the syntax/exception contract; it
does not validate an allocator's behavioral guarantees such as alignment
or the physical size of a grant. See [STORAGE.md](STORAGE.md).

`framing::Policy` originally checked only `layout(const Direction,
const uint8_t)`. Actual RX calls pass `Framer::rx` and either a const
candidate byte or a mutable prefix byte; TX passes
`framing::opposite(Framer::rx)` and a const byte. An unchecked mutable-byte
overload reproduced the same termination. All three actual expressions
are now checked, including conversion of a custom `rx` type. The
`opposite` lookup is qualified exactly like the endpoint's call, so ADL
cannot substitute another function during the check.

The original reproducers now fail at the intended compile-time boundaries.
Built-in Heap, Pool and Standard framers were not affected by the failures.
CRC semantics remain entirely user-controlled.

## Regression tests

[extension_checks.h](../src/wire/tests/extension_checks.h), called by
`test_contracts.cpp`, checks throwing, deleted and wrong-result overloads,
custom direction conversions, ADL, and rejection through the bound Storage
concept for all three protocols. Positive tests execute const-reference
storage through COBS/RTU/TCP and const-reference/nonthrowing-overloaded
framers through both stream RX, complete-candidate RX and TX. The combined
reader/CRC/storage/framer suite now executes **156 runtime checks** per
build, in addition to its static assertions.

The next independent round added
[test_length_domains.cpp](../src/wire/tests/test_length_domains.cpp):

- Every MBAP Length value `0..65535` over six CRC/data-limit configurations,
  including zero payload capacity and the largest encodable TCP payload.
- Every nonzero Protocol ID `1..65535` over the same six configurations;
  rejection must happen before allocation.
- Every two-byte private RTU count `0..65535` over four configurations,
  including zero capacity and maximum ADU bounds with NoCrc/CRC64.
- Exact physical allocation requests and known-remainder OOM skipping.
  All spans refer to actual backing arrays; no fabricated huge spans are used.

That is **786,426 TCP headers plus 262,144 RTU count declarations** and
**2,560,991 checks per build**. It passed under WSL ASan/UBSan and O3/LTO,
and MSVC x64/x86. This checks declaration/allocation/skip behavior; CRC
arithmetic and successful payload delivery retain their separate suites.

For the subsequent live repeat, the unchanged declaration cases were extracted
into [length_checks.h](../src/wire/tests/length_checks.h), shared by that host
main and the TCP MCU harness. Its real 1,024-byte backing array is consumed in
bounded chunks, rather than placing a 65,536-byte constant in the board's
64-KiB flash region. The count remains 2,560,991 checks. The custom-storage
positive tests similarly accept a backing Memory parameter: Heap on the host,
Pool in the static-only FreeRTOS image. Neither adaptation changes production
headers or substitutes a fake protocol implementation.

## Validation performed

- WSL wire, COBS, RTU, TCP, CRC, UART and adapter suites passed, including
  sanitized and release configurations provided by their runners.
- Qt adapter event-loop tests passed: 264 checks plus 22 journal/injection
  checks. The QtSerialBus harness compiled; no COM port was opened.
- MSVC x64/x86 runner passed, including the new length-domain suite.
- Strict GCC O3/LTO, altered enum/char ABI and shared-table linkage passed.
  NoCrc/Bitwise emitted no lookup table; two selected Table translation
  units linked one 512-byte table.
- The shared ARM guard passed: 96 scalar, 96 protocol and 48 COBS objects.
  TCP passed 72 default objects plus its Bitwise/Table positive controls.
- The extended contract test compiled with exceptions disabled for M0,
  M0+, M3, M4, M7, M23, M33 and M55. This is compile-only coverage.
- A separate UART fake-HAL experiment used 24 new random seeds (25..48):
  480,000 event/fault operations, 21,728 accepted sends and exactly 21,728
  terminal verdicts, with no model ownership violations in either
  ASan/UBSan or O3/LTO. This is a model test, not hardware execution.

### Code generation before/after

[extension_codegen.cpp](../src/wire/tests/extension_codegen.cpp) exposes
COBS, framed RTU and TCP consume/send/reclaim paths plus their object,
storage, Message and Packet sizes. GCC Arm 14.3.1 produced **18 byte-identical
object pairs**: M0/M4/M7 × Os/O2/O3 × Heap/Pool. The entire objects, not
just text sizes, were compared with SHA-256. Both sides used the same
fixture bytes apart from comments, with baseline versus corrected headers.

Common flags: `-std=c++20 -c -fno-exceptions -fno-rtti -ffunction-sections
-fdata-sections -mthumb -Isrc -Ilibs/delegate`, plus the selected `-mcpu`,
optimization and `-DPROBE_HEAP=1` for Heap. No debug information was emitted.
Local before/after objects remain under `src/wire/tests/out/extension-*`.
This demonstrates no code/layout cost in these instantiations, not execution
on every ARM target or a new physical CPU-load measurement.

## Re-run

Run `sh src/wire/tests/run.sh` for both new test groups, and
`powershell -File src/wire/tests/check_msvc.ps1` for both MSVC architectures.
Other suite commands remain documented in [BUILD.md](BUILD.md) and CLAUDE.md.
Local logs for this slice are `src/wire/tests/out/extension-*.log`;
the supplementary UART sequence logs are under `src/uart/tests/host/out/`.

No additional confirmed defect was found in the follow-up round. This is a
bounded audit result, not a guarantee against all possible failures. The
initial audit above was host/compile-only. The subsequent user-requested
[live H7S repeat](HARDWARE_EXTENSIONS_2026-09-12.md) records board execution
of these fixes separately, including the shared extension and exhaustive
length-domain bodies. The external ST-Link programming issue from the
earlier report has not been claimed fixed.
