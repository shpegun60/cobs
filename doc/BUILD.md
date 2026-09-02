<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# Building and verifying COBS on Windows (MinGW)

The repository has two qmake targets with different jobs:

- `COBS.pro` is the Qt Widgets host scaffold and compiles the real non-template
  COBS codec through `cobs/cobs.pri`;
- `cobs/tests/qmake_consumer/consumer.pro` is the application-shaped proof. It
  includes only `Cobs.h`, instantiates `Endpoint` with both `Heap` and `Pool`,
  binds delegates, sends, receives, polls, observes `Stats`, and executes.

The second target is the stronger public-API integration proof; the GUI does
not need test logic in `main.cpp` merely to instantiate templates.

Current COBS documentation is split by boundary:

- `ARCHITECTURE.md` — components, public API, ownership, and lifetimes;
- `PROTOCOL.md` — normative wire format and framing behavior;
- `STORAGE.md` — checked storage extension contract and custom strategies;
- `COBS_ENGINE.md` — detailed rationale, state traces, and overlap proof.

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
sh cobs/tests/qmake_consumer/run.sh
```

The default out-of-tree result is
`build/cobs-consumer/bin/cobs_pri_consumer.exe`. `QMAKE`, `MAKE`, and
`COBS_QMAKE_BUILD_DIR` may override the tools or output directory.

## COBS verification

MinGW host suite, including six independent COBS/shared-header smoke checks, seven
expected compile-fail contracts with diagnostic validation, and the
`-DNDEBUG` storage guarantees:

```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:$PATH"
sh cobs/tests/run.sh
```

WSL ASan+UBSan run (do not run it concurrently with the MinGW command because
both intentionally reuse `cobs/tests/out`):

```powershell
wsl -e sh -lc 'cd /mnt/c/Users/admin/Documents/my_workspace/Qt/COBS && CXX=g++ sh cobs/tests/run.sh'
```

Cortex-M compile-only layout assertions with the recorded CubeIDE toolchain:

```bash
sh cobs/tests/check_arm_layout.sh
sh wire/tests/check_arm_hotpath.sh
sh wire/tests/check_arm_codegen_matrix.sh
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
sh wire/tests/run.sh
MATRIX_TAG=gcc13 CXX=/c/Qt/Tools/mingw1310_64/bin/g++.exe \
  sh wire/tests/check_gcc_matrix.sh
```

Repeat the second command with a distinct tag for every installed GCC. It
enables strict alignment, aliasing, bounds, null and format diagnostics and
also proves protocol bytes under `-fshort-enums -funsigned-char`.

## UART regression matrix

The current UART ownership, callback, recovery, and performance contracts are
recorded in `UART_PARANOID_AUDIT.md`. Its host interleaving suite and STM32
portability/probe matrix are:

```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:$PATH"
sh uart/tests/host/run.sh
sh uart/tests/port/build.sh
```

## COBS + UART hardware integration matrix

The real-silicon NUCLEO-H7S3L8 harness, independent PC codec, exact negative
tests, DWT accounting, baud sweep, raw JSONL evidence, and reproduction steps
live in `cobs/tests/hardware/h7s/README.md`.

The one-command Windows runner builds and verifies a fresh image at
115200/1M/3M/6M/10M, executes the complete COBS suite at each rate, performs
physical gap/recovery tests, runs the extended 10M stress, and restores a
smoke-checked 115200 image:

```powershell
& 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' `
  -NoProfile -ExecutionPolicy Bypass `
  -File 'cobs/tests/hardware/h7s/run_matrix.ps1' `
  -Port COM6 -StLinkSerial <STLINK_SERIAL> `
  -Output 'cobs/tests/hardware/h7s/results_new.jsonl'
```

## Running the executable

Outside Qt Creator the exe needs the Qt runtime DLLs. Either keep `C:\Qt\6.10.1\mingw_64\bin` on `PATH` when launching it, or make the build self-contained once:

```powershell
C:\Qt\6.10.1\mingw_64\bin\windeployqt.exe build\cli\release\COBS.exe
```

## Adding files to the project

GUI sources, headers, and `.ui` forms are registered in `COBS.pro`. COBS
library sources and headers are registered once in `cobs/cobs.pri`. After
editing either list, re-run qmake before `mingw32-make`.

## Cleaning

`mingw32-make clean` removes objects; deleting the whole `build/cli/` directory is the reliable full reset.
