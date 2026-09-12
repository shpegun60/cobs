# Author: shpegun60
# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [string]$DeveloperCommand = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat'
)
$ErrorActionPreference = 'Stop'
$msvcOriginalPath = $env:Path
# VsDevCmd's discovery can invoke vswhere by name even when the installer
# directory was not inherited in PATH (e.g. a plain Codex/PowerShell session).
$msvcInstaller = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
if (Test-Path (Join-Path $msvcInstaller 'vswhere.exe')) {
    $env:Path = "$msvcInstaller;$env:Path"
}
$repo = $PSScriptRoot
while (-not (Test-Path (Join-Path $repo 'COBS.pro'))) { $repo = Split-Path $repo -Parent }
$src = Join-Path $repo 'src'
$cases = @(
    @{name='crc'; sources=@('crc/tests/test_crc.cpp')},
    @{name='storage'; sources=@('wire/tests/test_storage.cpp')},
    @{name='parity'; sources=@('wire/tests/test_api_parity.cpp')},
    @{name='contracts'; sources=@('wire/tests/test_contracts.cpp', 'cobs/Decoder.cpp', 'cobs/Encoder.cpp')},
    @{name='endpoint_parity'; sources=@('wire/tests/test_endpoint_parity.cpp', 'cobs/Decoder.cpp', 'cobs/Encoder.cpp')},
    @{name='payload_limits'; sources=@('wire/tests/test_payload_limits.cpp', 'cobs/Decoder.cpp', 'cobs/Encoder.cpp')},
    @{name='custom_memory'; sources=@('wire/tests/test_protocol_storage.cpp', 'cobs/Decoder.cpp', 'cobs/Encoder.cpp')},
    @{name='cobs_crc'; sources=@('cobs/tests/test_crc.cpp', 'cobs/Decoder.cpp', 'cobs/Encoder.cpp')},
    @{name='cobs_layout'; sources=@('cobs/tests/test_layout.cpp')},
    @{name='rtu_geometry'; sources=@('modbus/rtu/tests/test_crc_geometry.cpp')},
    @{name='rtu_framing'; sources=@('modbus/rtu/tests/test_framing.cpp')},
    @{name='rtu_stream'; sources=@('modbus/rtu/tests/test_stream.cpp')},
    @{name='rtu_stream_fuzz'; sources=@('modbus/rtu/tests/test_stream_fuzz.cpp')},
    @{name='rtu_layout'; sources=@('modbus/rtu/tests/test_layout.cpp')},
    @{name='tcp_core'; sources=@('modbus/tcp/tests/test_core.cpp')},
    @{name='tcp_advanced'; sources=@('modbus/tcp/tests/test_advanced.cpp')},
    @{name='tcp_data_limits'; sources=@('modbus/tcp/tests/test_data_limits.cpp')}
)
Push-Location $src
try {
    foreach ($arch in @('x64', 'x86')) {
        $out = Join-Path $PSScriptRoot "out/msvc-$arch"
        New-Item -ItemType Directory -Force $out | Out-Null
        foreach ($case in $cases) {
            $sources = $case.sources -join ' '
            $exe = Join-Path $out ($case.name + '.exe')
            $command = "call `"$DeveloperCommand`" -arch=$arch -host_arch=x64 > nul && " +
                "cl /nologo /std:c++20 /Zc:__cplusplus /permissive- /EHsc /utf-8 /O2 /DNDEBUG /WX " +
                "/I. /Icobs /I`"$repo\libs\delegate`" $sources /Fo`"$out/`" /Fe`"$exe`" && `"$exe`""
            & $env:ComSpec /d /c $command
            if ($LASTEXITCODE -ne 0) { throw "MSVC $arch $($case.name) failed" }
        }
    }
    Write-Host 'MSVC x64/x86: CRC, shared storage, protocol parity/custom memory, COBS CRC/layout, RTU geometry/framing/stream/layout, TCP core/advanced passed'
} finally { Pop-Location; $env:Path = $msvcOriginalPath }
