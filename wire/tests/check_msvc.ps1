# Author: shpegun60
# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [string]$DeveloperCommand = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$cases = @(
    @{name='crc'; sources=@('crc/tests/test_crc.cpp')},
    @{name='storage'; sources=@('wire/tests/test_storage.cpp')},
    @{name='parity'; sources=@('wire/tests/test_api_parity.cpp')},
    @{name='custom_memory'; sources=@('wire/tests/test_protocol_storage.cpp', 'cobs/Decoder.cpp', 'cobs/Encoder.cpp')},
    @{name='cobs_crc'; sources=@('cobs/tests/test_crc.cpp', 'cobs/Decoder.cpp', 'cobs/Encoder.cpp')},
    @{name='cobs_layout'; sources=@('cobs/tests/test_layout.cpp')},
    @{name='rtu_geometry'; sources=@('modbus/rtu/tests/test_crc_geometry.cpp')},
    @{name='rtu_layout'; sources=@('modbus/rtu/tests/test_layout.cpp')}
)
Push-Location $repo
try {
    foreach ($arch in @('x64', 'x86')) {
        $out = Join-Path $PSScriptRoot "out/msvc-$arch"
        New-Item -ItemType Directory -Force $out | Out-Null
        foreach ($case in $cases) {
            $sources = $case.sources -join ' '
            $exe = Join-Path $out ($case.name + '.exe')
            $command = "call `"$DeveloperCommand`" -arch=$arch -host_arch=x64 > nul && " +
                "cl /nologo /std:c++20 /Zc:__cplusplus /permissive- /EHsc /utf-8 /O2 /DNDEBUG " +
                "/I. /Icobs /Ilibs/delegate $sources /Fo`"$out/`" /Fe`"$exe`" && `"$exe`""
            & $env:ComSpec /d /c $command
            if ($LASTEXITCODE -ne 0) { throw "MSVC $arch $($case.name) failed" }
        }
    }
    Write-Host 'MSVC x64/x86: CRC, shared storage, protocol parity/custom memory, COBS CRC/layout, RTU geometry/layout passed'
} finally { Pop-Location }
