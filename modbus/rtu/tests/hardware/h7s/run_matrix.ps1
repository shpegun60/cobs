# Author: shpegun60
# SPDX-License-Identifier: MIT

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM[0-9]+$')]
    [string]$Port,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$StLinkSerial,

    [ValidateSet(115200, 1000000, 3000000, 6000000, 10000000)]
    [int[]]$BaudRates = @(115200, 1000000),

    [ValidateRange(1, 3600)]
    [int]$StressSeconds = 5,

    [ValidateRange(0, 3600)]
    [int]$ExtendedSeconds = 15,

    [string]$Output,

    [string]$CubeProgrammer =
        'C:\ST\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe',

    [string]$GitBash = 'C:\Program Files\Git\bin\bash.exe',

    [string]$Python = 'python',

    [switch]$LeaveAtLastBaud
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
$repo = (Resolve-Path -LiteralPath (Join-Path $here '..\..\..\..\..')).Path
$buildScript = Join-Path $here 'build.sh'
$runner = Join-Path $here 'modbus_hardware.py'
$elf = Join-Path $repo `
    'stm32_cube_test\h7s_cobs_test\out\modbus-hardware\modbus_hardware_bench.elf'

if (-not (Test-Path -LiteralPath $CubeProgrammer -PathType Leaf)) {
    throw "STM32CubeProgrammer not found: $CubeProgrammer"
}
if (-not (Test-Path -LiteralPath $GitBash -PathType Leaf)) {
    throw "Git Bash not found: $GitBash"
}
if (-not (Get-Command $Python -ErrorAction SilentlyContinue)) {
    throw "Python command not found: $Python"
}

if ([string]::IsNullOrWhiteSpace($Output)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $Output = Join-Path $here "results_$stamp.jsonl"
} elseif (-not [System.IO.Path]::IsPathRooted($Output)) {
    $Output = Join-Path $repo $Output
}
$Output = [System.IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $Output) {
    throw "Refusing to append a new audit to existing results: $Output"
}

$matrix = @($BaudRates | Sort-Object -Unique)
if ($matrix.Count -eq 0) {
    throw 'BaudRates must not be empty'
}
$maximumBaud = $matrix[-1]
$previousBaud = $env:MODBUS_HW_BAUD

function Assert-NativeSuccess([string]$Operation) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation failed with exit code $LASTEXITCODE"
    }
}

function Build-And-Flash([int]$Baud) {
    Write-Host "`n=== BUILD + FLASH $Baud baud ==="
    $env:MODBUS_HW_BAUD = [string]$Baud
    & $GitBash $buildScript
    Assert-NativeSuccess "ARM build at $Baud baud"

    & $CubeProgrammer `
        -c port=SWD "sn=$StLinkSerial" mode=UR reset=HWrst freq=4000 `
        -w $elf -v -rst
    Assert-NativeSuccess "flash/verify at $Baud baud"
}

function Run-Suite([int]$Baud, [string]$Suite, [int]$Seconds) {
    $arguments = @(
        '-B', $runner, $Port,
        '--baud', [string]$Baud,
        '--suite', $Suite,
        '--output', $Output
    )
    if ($Suite -eq 'all' -or $Suite -eq 'stress') {
        $arguments += @('--seconds', [string]$Seconds)
    }
    & $Python @arguments
    Assert-NativeSuccess "$Suite suite at $Baud baud"
}

Push-Location $repo
try {
    foreach ($baud in $matrix) {
        Build-And-Flash $baud
        Run-Suite $baud 'all' $StressSeconds
    }

    if ($ExtendedSeconds -gt 0) {
        Write-Host "`n=== EXTENDED $maximumBaud baud ==="
        Run-Suite $maximumBaud 'stress' $ExtendedSeconds
    }

    if (-not $LeaveAtLastBaud) {
        Build-And-Flash 115200
        Run-Suite 115200 'smoke' 1
    }

    Write-Host "`n=== MODBUS RTU MATRIX PASS ==="
    Write-Host "Results: $Output"
} finally {
    $env:MODBUS_HW_BAUD = $previousBaud
    Pop-Location
}
