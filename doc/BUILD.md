# Building COBS on Windows (MinGW)

The project is a qmake-based Qt Widgets application. It can be built either from Qt Creator or from the command line.

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

## Running the executable

Outside Qt Creator the exe needs the Qt runtime DLLs. Either keep `C:\Qt\6.10.1\mingw_64\bin` on `PATH` when launching it, or make the build self-contained once:

```powershell
C:\Qt\6.10.1\mingw_64\bin\windeployqt.exe build\cli\release\COBS.exe
```

## Adding files to the project

New sources, headers, and `.ui` forms must be registered in `COBS.pro` (`SOURCES`, `HEADERS`, `FORMS`). After editing the `.pro` file, re-run `qmake` before `mingw32-make`.

## Cleaning

`mingw32-make clean` removes objects; deleting the whole `build/cli/` directory is the reliable full reset.
