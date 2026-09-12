<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# Build integration and verification

[Documentation](README.md) · [Examples](EXAMPLES.md) · [Qt](QT.md) · [FreeRTOS](FREERTOS.md) · [Testing](TESTING.md)

<!-- toc -->

Contents

- [Embed the libraries in your application](#embed-the-libraries-in-your-application)
- [Documentation cookbook checks](#documentation-cookbook-checks)
- [Toolchain](#toolchain)
- [Qt Creator](#qt-creator)
- [Command line](#command-line)
- [Reusable COBS qmake fragment](#reusable-cobs-qmake-fragment)
- [COBS verification](#cobs-verification)
- [Shared storage and integrity verification](#shared-storage-and-integrity-verification)
- [CRC and Modbus RTU verification](#crc-and-modbus-rtu-verification)
- [Integration examples and evidence regressions](#integration-examples-and-evidence-regressions)
- [UART regression matrix](#uart-regression-matrix)
- [COBS + UART hardware integration matrix](#cobs--uart-hardware-integration-matrix)
- [Modbus TCP core and MCU validation](#modbus-tcp-core-and-mcu-validation)
- [Running the executable](#running-the-executable)
- [Adding files to the project](#adding-files-to-the-project)
- [Cleaning](#cleaning)

<!-- /toc -->

The repository has separate GUI, downstream-consumer and cookbook targets.
The original two COBS targets have different jobs:

- `COBS.pro` is the Qt Widgets host scaffold and compiles the real non-template
  COBS codec through `src/cobs/cobs.pri`;
- `src/cobs/tests/qmake_consumer/consumer.pro` is the application-shaped proof. It
  includes only `Cobs.h`, instantiates `Endpoint` with both `Heap` and `Pool`,
  binds delegates, sends, receives, polls, observes `Stats`, and executes.

The second target is the stronger public-API integration proof; the GUI does
not need test logic in `main.cpp` merely to instantiate templates.

The complete navigation is [the documentation index](README.md). Current
reference and historical validation remain separated by boundary:

- `ARCHITECTURE.md` — components, public API, ownership, and lifetimes;
- `PROTOCOL.md` — normative wire format and framing behavior;
- `STORAGE.md` — checked storage extension contract and custom strategies;
- `COBS_ENGINE.md` — historical v1 rationale and the unchanged codec overlap proof.
- `SHARED_POLICIES_VALIDATION.md` — fresh shared-storage/CRC migration evidence.
- [HARDWARE_REGRESSION_2026-09-07.md](HARDWARE_REGRESSION_2026-09-07.md) — repeatable
  live fault coverage, restored firmware, Qt/VCP failures and control repeats.
- [QT_USB_TIMEOUT_DIAGNOSIS.md](QT_USB_TIMEOUT_DIAGNOSIS.md) — controlled Qt
  fragment-loss reproduction, USB receive deadline, trace and regression tests.
- [QT_CLIENT_RECOVERY.md](QT_CLIENT_RECOVERY.md) — desktop RX/TX ordering,
  bounded write failure, cancellation/retry guards and live interop follow-up.

## Embed the libraries in your application

You do not need the repository Qt GUI, tests or hardware harness in firmware.
All includes below are relative to the `src` root; preserve sibling module
folders instead of copying a single public header away from its dependencies.

| Component | Include root(s) | Sources to compile |
|---|---|---|
| COBS | `src`, `libs/delegate` | `src/cobs/Encoder.cpp`, `src/cobs/Decoder.cpp`, once each |
| RTU / TCP | `src`, `libs/delegate` | header-only |
| wire / CRC | `src` | header-only |
| STM32 UART | `src`, `libs/delegate`, `libs/spsc`, `libs/spsc/src`, your HAL/CMSIS/main.h | one TU with UART callback implementation, or the documented alternate callback mode |
| Qt serial adapters | protocol roots plus Qt Core/SerialPort | header-only adapter; link Qt modules |
| FreeRTOS wake | UART plus the real kernel/port and FreeRTOSConfig.h | header-only wake; application links its configured kernel normally |

CMake integration fragment (replace `/path/to/cobs` with your checkout):

```cmake
set(COMM_ROOT /path/to/cobs)
target_compile_features(app PRIVATE cxx_std_20)
target_include_directories(app PRIVATE
    ${COMM_ROOT}/src
    ${COMM_ROOT}/libs/delegate)
# Only if COBS is used:
target_sources(app PRIVATE
    ${COMM_ROOT}/src/cobs/Encoder.cpp
    ${COMM_ROOT}/src/cobs/Decoder.cpp)
# Only if the STM32 UART is used; HAL/CMSIS/board includes come from your target:
target_include_directories(app PRIVATE
    ${COMM_ROOT}/libs/spsc
    ${COMM_ROOT}/libs/spsc/src)
# For a Qt serial app (use Network instead for QTcpSocket):
find_package(Qt6 REQUIRED COMPONENTS Core SerialPort)
target_link_libraries(app PRIVATE Qt6::Core Qt6::SerialPort)
```

Remove the optional sections your application does not use. Public includes
then read `#include "cobs/Cobs.h"`, `"modbus/rtu/Rtu.h"`,
`"modbus/tcp/Tcp.h"` and `"uart/Uart.h"`. CRC/storage dependencies of a
protocol are included by its public headers, not manually copied into it.

For qmake use `src/cobs/cobs.pri`, `src/modbus/rtu/rtu.pri`,
`src/modbus/tcp/tcp.pri`, and `src/adapters/qt/qt.pri` only as applicable.
These fragments add their sibling includes and dependencies. The examples
under `doc/examples/qt` have complete downstream `.pro` files.

## Documentation cookbook checks

```sh
python -B doc/check_docs.py
sh doc/examples/build.sh
sh doc/examples/qt/build.sh
# Optional, with the recorded H7RS HAL/Cube/FreeRTOS checkout and ARM compiler:
sh doc/examples/check_freertos_arm.sh
```

The last command compiles both task-entry variants against real FreeRTOS/HAL
headers, without DOC_HOST. It does not link, flash, or execute firmware. Set
`ARM_TOOLS`, `FREERTOS_SOURCE` and `H7S_CUBE_PROJECT` for other local locations.
The host runner supports `CXX`; Qt supports `QMAKE`, `MAKE` and its kit variables.
See [Examples](EXAMPLES.md) for every program/mode and [Testing](TESTING.md)
for full regression and live evidence rather than example-only checks.

## Toolchain

| Tool | Path |
|------|------|
| qmake (Qt 6.10.1 MinGW 64-bit) | `C:\Qt\6.10.1\mingw_64\bin\qmake.exe` |
| mingw32-make / g++ (MinGW 13.1.0) | `C:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe` |

Both `bin` directories must be on `PATH` for a command-line build.

## Qt Creator

Open `COBS.pro` with the kit **Desktop Qt 6.10.1 MinGW 64-bit**. Build output goes to `build/Desktop_Qt_6_10_1_MinGW_64_bit_Debug/`.

## Command line

Git Bash:

```bash
export PATH="/c/Qt/6.10.1/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH"
mkdir -p build/cli && cd build/cli
qmake ../../COBS.pro          # add CONFIG+=debug for a debug build
mingw32-make -j
```

PowerShell:

```powershell
$env:PATH = "C:\Qt\6.10.1\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;$env:PATH"
New-Item -ItemType Directory -Force build\cli; Set-Location build\cli
qmake ..\..\COBS.pro          # add CONFIG+=debug for a debug build
mingw32-make -j
```

A plain `qmake` produces a **release** build: `build/cli/release/COBS.exe`. With `CONFIG+=debug` the output goes to `build/cli/debug/`.

The `build/cli/` directory is used so command-line builds never collide with Qt Creator's build directory.

## Reusable COBS qmake fragment

A downstream qmake target consumes the library with one line:

```qmake
include(path/to/cobs/cobs.pri)
```

The guarded fragment enables C++20, adds the COBS and delegate include paths,
registers every library header, and compiles `Decoder.cpp` and `Encoder.cpp`
exactly once. In this repository it finds `libs/delegate` automatically. An
external layout may override the dependency path before including the file:

```qmake
COBS_DELEGATE_DIR = path/to/tiny_delegate
include(path/to/cobs/cobs.pri)
```

Run the checked consumer from Git Bash:

```bash
export PATH="/c/Qt/6.10.1/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH"
sh src/cobs/tests/qmake_consumer/run.sh
```

The default out-of-tree result is
`build/cobs-consumer/bin/cobs_pri_consumer.exe`. `QMAKE`, `MAKE`, and
`COBS_QMAKE_BUILD_DIR` may override the tools or output directory.

## COBS verification

MinGW host suite, including eight independent COBS/shared-header smoke checks, seven
expected compile-fail contracts with diagnostic validation, and the
`-DNDEBUG` storage guarantees:

```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:$PATH"
sh src/cobs/tests/run.sh
```

WSL ASan+UBSan run (do not run it concurrently with the MinGW command because
both intentionally reuse `src/cobs/tests/out`):

```powershell
wsl -e sh -lc 'cd /mnt/c/Users/admin/Documents/my_workspace/Qt/COBS && CXX=g++ sh src/cobs/tests/run.sh'
```

Cortex-M compile-only layout assertions with the recorded CubeIDE toolchain:

```bash
sh src/cobs/tests/check_arm_layout.sh
sh src/wire/tests/check_arm_hotpath.sh
sh src/wire/tests/check_arm_codegen_matrix.sh
```

The scalar hot-path guard compiles both little- and big-endian Cortex-M7
objects. It rejects runtime endian branches, helper calls, needless native
swaps, or loss of the expected `REV16`/`REV` opposite-order operations.
The larger matrix adds M0/M0+/M3/M4/M7/M23/M33/M55, `-Os/-O2/-O3`, strict
alignment and protocol/COBS translation units. On cores that cannot safely
perform unaligned scalar accesses it requires inline byte loads/stores rather
than an out-of-line `memcpy` helper.

The shared host scalar oracle and strict GCC consumer/LTO proof are:

```bash
sh src/wire/tests/run.sh
MATRIX_TAG=gcc13 CXX=/c/Qt/Tools/mingw1310_64/bin/g++.exe \
  sh src/wire/tests/check_gcc_matrix.sh
```

Repeat the second command with a distinct tag for every installed GCC. It
enables strict alignment, aliasing, bounds, null and format diagnostics and
also proves protocol bytes under `-fshort-enums -funsigned-char`. Both commands
compile the COBS/Modbus API parity contract, including identical reader
function identity and the deliberately different COBS-stream/RTU-ADU boundary.
`run.sh` additionally executes `test_endpoint_parity`: one public ownership,
failure/retry, storage-exhaustion and read/write lifecycle for COBS, burst RTU
and both framed RTU directions, plus a regression lock for owned RTU prefixes.
It runs in the sanitized build and separately under `-O3 -DNDEBUG -flto`.
`src/adapters/tests/run.sh` adds the equivalent UART lifecycle through both
adapters and the same FreeRTOS wake, on host HAL/kernel fakes. See
[`API_PARITY.md`](API_PARITY.md) for the exact common contract.

## Shared storage and integrity verification

```bash
sh src/wire/tests/run.sh
sh src/wire/tests/check_shared_crc.sh   # ELF objects only: run under WSL or with arm-none-eabi (see below)
python -B src/wire/tests/verify_hardware_migration.py
```

`check_shared_crc.sh` reads symbol sizes from `nm -S`, which COFF objects do
not carry, so on a Windows host it refuses MinGW with an explicit message.
Run it under WSL, or with the ARM toolchain:

```bash
CXX=arm-none-eabi-g++ NM=arm-none-eabi-nm CXXFLAGS="-mthumb -mcpu=cortex-m7 -mfloat-abi=soft" sh src/wire/tests/check_shared_crc.sh
```

MSVC has a separate native x64/x86 runner (no sanitizer claim):

```powershell
powershell -ExecutionPolicy Bypass -File src/wire/tests/check_msvc.ps1
```

The shared suites include raw Pool checks under NDEBUG, real protocol Geometry,
a protocol-blind custom memory used through both endpoints, under/overgrants,
original descriptor return and equal-width Bitwise/Table type identity.
`check_shared_crc.sh` links two translation units: both protocols together emit
one 512-byte CRC16 Table, or no lookup bytes for Bitwise/NoCrc.

The ELF host suite also interposes `malloc/free` around the real `wire::Heap`
to force COBS, burst RTU and framed RTU construction/growth/RX failures and
verify retained Packet data, retries and balanced ownership, with sanitizers
and separately under `-O3 -DNDEBUG -flto`. That linker-specific test is explicitly
skipped on MinGW. The MSVC runner compiles with `/WX`; reflected/forward CRC8
Table are additionally checked on all 65,536 two-byte inputs. See the
[live Heap OOM correction and audit](../src/wire/tests/hardware/h7s/heap_crc/recovery/README.md).

COBS also tests explicit legacy NoCrc vectors and its CRC16/253 default,
all built-in policies, stateful/sum/custom-width policies, corruption, empty and
maximum frames, and the exact inverse of physical TX geometry.

## CRC and Modbus RTU verification

The protocol-independent CRC module and header-only RTU endpoint have separate
host suites:

```bash
sh src/crc/tests/run.sh
sh src/modbus/rtu/tests/run.sh
```

The CRC suite checks named CRC8/16/32/64 vectors, 20,000 independent random
oracles for both Bitwise and Table implementations, little/big-endian and
truncated integer codecs, stateful custom policies, and `NoCrc` under
ASan+UBSan and `-O3 -DNDEBUG`. The RTU suite adds compile-fail contracts,
policy-derived `0/1/2/3/4/8`-byte geometry, configurable ADU ceilings through 65535,
stateful fake-hardware injection, storage ownership, and fuzzing.

The Cortex-M guards are:

```bash
sh src/crc/tests/check_arm_codegen.sh
sh src/modbus/rtu/tests/check_arm_crc_codegen.sh
sh src/modbus/rtu/tests/check_arm_layout.sh
```

They prove that all CRC8/16/32/64 Bitwise and Table calculation loops are
helper-call-free, unused Table types emit no lookup object, and each selected
Table specialization emits exactly one private read-only table of the expected
size at `-Os/-O2/-O3` on both CPU byte orders. Every codec path is
branch/call-free under default and strict alignment, default RTU emits no
table, `NoCrc` folds away, and empty policies add no Endpoint RAM.

The downstream qmake and fake-UART integrations are:

```bash
export PATH="/c/Qt/6.10.1/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH"
sh src/modbus/rtu/tests/qmake_consumer/run.sh
sh src/adapters/tests/run.sh
```

## Integration examples and evidence regressions

From the repository root, with a C++20 compiler on `PATH`:

```bash
sh doc/examples/build.sh
python -B src/wire/tests/hardware/h7s/test_comparison.py
```

Run the integration examples and `src/modbus/rtu/tests/run.sh` on both
Linux/WSL GCC and MinGW, sequentially: each script reuses its own `out/`
directory. The Linux RTU run keeps `-Wsign-conversion -Werror` and
ASan+UBSan enabled; no warning suppression is needed for the reference
model. Its packed-bit tests cover every byte value, unaligned coil writes
and preservation of neighbouring bits. The examples use `g_endpoint` for
their global objects to avoid POSIX `link()` and template-parameter shadowing.

The Python regression suite rejects UART row protocol/policy/baud mismatches,
HELLO baud mismatches and swapped core-group policies. Its positive controls
are the original committed comparison records; negative tests mutate copies
in memory, never the raw evidence. It isolates source provenance in those
tests, so also run `verify_comparison.py <record> --check-doc
doc/PROTOCOL_COMPARISON.md` to verify real source identities and published
rows. Neither command opens a serial port or flashes the board.

The [real-FreeRTOS H7S harness](../src/adapters/tests/hardware/h7s/parity/README.md)
additionally validates the ordinary `wait(adapter); adapter.proceed();` loop
with the installed STM32Cube kernel, not fake headers. Its guarded runner
requires exclusive board/COM access and a new output directory. See the
[API parity hardware report](HARDWARE_API_PARITY_2026-09-12.md) for exact
recorded sources, restoration receipts, scope and offline verification commands.

## UART regression matrix

The current UART ownership, callback, recovery, and performance contracts are
recorded in `UART_PARANOID_AUDIT.md`. Its host interleaving suite and STM32
portability/probe matrix are:

```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:$PATH"
sh src/uart/tests/host/run.sh
sh src/uart/tests/port/build.sh
```

## COBS + UART hardware integration matrix

The real-silicon NUCLEO-H7S3L8 harness, independent PC codec, exact negative
tests, DWT accounting, baud sweep, raw JSONL evidence, and reproduction steps
live in `src/cobs/tests/hardware/h7s/README.md`.

The one-command Windows runner builds and verifies a fresh image at
115200/1M/3M/6M/10M, executes the complete COBS suite at each rate, performs
physical gap/recovery tests, runs the extended 10M stress, and restores a
smoke-checked 115200 image:

```powershell
& 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' `
  -NoProfile -ExecutionPolicy Bypass `
  -File 'src/cobs/tests/hardware/h7s/run_matrix.ps1' `
  -Port COM6 -StLinkSerial <STLINK_SERIAL> `
  -Output 'src/cobs/tests/hardware/h7s/results_new.jsonl'
```

## Modbus TCP core and MCU validation

The [2026-09-12 cross-stack audit](PARANOID_AUDIT_2026-09-12.md) records the
fresh complete host/ARM/live verification and scoped UART/Qt corrections.
Its live receipts include full source identities and restored-flash checks;
older green records are not substitutes for testing changed source bytes.

TCP is header-only: include `modbus/tcp/Tcp.h` with `-I src -I libs/delegate`,
or include `src/modbus/tcp/tcp.pri` from qmake. No network stack is linked.

```sh
sh src/modbus/tcp/tests/run.sh
sh src/modbus/tcp/tests/check_arm.sh
sh src/modbus/tcp/tests/qmake_consumer/run.sh
```

The host runner checks seven independent headers, nine intentionally rejected
contracts, all nine built-in integrity policies over Heap/Pool, custom memory
faults, stateful policy injection, arbitrary stream cuts and size extremes.
Run under WSL for ASan/UBSan; MinGW has no sanitizer runtime. Both run O3/LTO.
`src/wire/tests/check_msvc.ps1` also includes TCP on x64 and x86 with `/WX`.
`src/wire/tests/run.sh` additionally exercises 324 COBS/RTU/TCP combinations
against the same useful-data limit contract, under sanitizers and O3/LTO.
See [payload-limit migration](PAYLOAD_LIMITS.md).
The ARM guard compiles 72 NoCrc probes (eight cores, three optimizations,
little/big/strict modes) plus Bitwise/Table controls.

The [H7S runner](../src/modbus/tcp/tests/hardware/h7s/README.md) builds six images,
backs up boot flash, tests exact MBAP ADUs through UART, and restores/read-backs
the original image. It does not start or verify Ethernet/TCP/IP.

## Running the executable

Outside Qt Creator the exe needs the Qt runtime DLLs. Either keep `C:\Qt\6.10.1\mingw_64\bin` on `PATH` when launching it, or make the build self-contained once:

```powershell
C:\Qt\6.10.1\mingw_64\bin\windeployqt.exe build\cli\release\COBS.exe
```

## Adding files to the project

GUI sources, headers, and `.ui` forms are registered in `COBS.pro`. COBS
library sources and headers are registered once in `src/cobs/cobs.pri`. After
editing either list, re-run qmake before `mingw32-make`.

## Cleaning

`mingw32-make clean` removes objects; deleting the whole `build/cli/` directory is the reliable full reset.
