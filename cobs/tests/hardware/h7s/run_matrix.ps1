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
    [int[]]$BaudRates = @(115200, 1000000, 3000000, 6000000, 10000000),

    [ValidateRange(1, 3600)]
    [int]$StressSeconds = 10,

    [ValidateRange(0, 3600)]
    [int]$ExtendedSeconds = 30,

    [ValidateSet('none', 'bitwise', 'table')]
    [string]$Crc = 'bitwise',

    [int]$MaxPayload = 0,

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
$repo = (Resolve-Path -LiteralPath (Join-Path $here '..\..\..\..')).Path
$buildScript = Join-Path $here 'build.sh'
$runner = Join-Path $here 'cobs_hardware.py'
$elf = Join-Path $repo `
    'stm32_cube_test\h7s_cobs_test\out\cobs-hardware\cobs_hardware_bench.elf'

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
$minimumBaud = $matrix[0]
$maximumBaud = $matrix[-1]
$previousBaud = $env:COBS_HW_BAUD
$previousCrc = $env:COBS_HW_CRC
$previousPayload = $env:COBS_HW_MAX_PAYLOAD
$env:COBS_HW_CRC = [string]@{none=0; bitwise=1; table=2}[$Crc]
if ($MaxPayload -eq 0) { $MaxPayload = if ($Crc -eq 'none') { 255 } else { 253 } }
if ($MaxPayload -lt 192 -or $MaxPayload -gt 65535 - $(if ($Crc -eq 'none') { 0 } else { 2 })) {
    throw 'Invalid hardware payload ceiling'
}
$env:COBS_HW_MAX_PAYLOAD = [string]$MaxPayload

function Assert-NativeSuccess([string]$Operation) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation failed with exit code $LASTEXITCODE"
    }
}

function Build-And-Flash([int]$Baud) {
    Write-Host "`n=== BUILD + FLASH $Baud baud ==="
    $env:COBS_HW_BAUD = [string]$Baud
    & $GitBash $buildScript
    Assert-NativeSuccess "ARM build at $Baud baud"

    & $CubeProgrammer `
        -c port=SWD "sn=$StLinkSerial" mode=UR reset=HWrst freq=4000 `
        -w $elf -v -rst
    Assert-NativeSuccess "flash/verify at $Baud baud"
}

function Run-Suite([int]$Baud, [string]$Suite, [int]$Seconds,
                   [int]$Window) {
    $arguments = @(
        '-B', $runner, $Port,
        '--baud', [string]$Baud,
        '--crc', $Crc, '--max-payload', [string]$MaxPayload,
        '--suite', $Suite,
        '--output', $Output
    )
    if ($Suite -eq 'all' -or $Suite -eq 'stress') {
        $arguments += @('--seconds', [string]$Seconds, '--window', [string]$Window)
    }
    & $Python @arguments
    Assert-NativeSuccess "$Suite suite at $Baud baud"
}

Push-Location $repo
try {
    foreach ($baud in $matrix) {
        Build-And-Flash $baud
        Run-Suite $baud 'all' $StressSeconds 4
        if ($baud -eq $minimumBaud -or $baud -eq $maximumBaud) {
            Run-Suite $baud 'gap' $StressSeconds 4
        }
    }

    if ($ExtendedSeconds -gt 0) {
        Write-Host "`n=== EXTENDED $maximumBaud baud / window 7 ==="
        Run-Suite $maximumBaud 'stress' $ExtendedSeconds 7
    }

    if (-not $LeaveAtLastBaud) {
        Build-And-Flash 115200
        Run-Suite 115200 'smoke' 1 1
    }

    Write-Host "`n=== MATRIX PASS ==="
    Write-Host "Results: $Output"
} finally {
    $env:COBS_HW_BAUD = $previousBaud
    $env:COBS_HW_CRC = $previousCrc
    $env:COBS_HW_MAX_PAYLOAD = $previousPayload
    Pop-Location
}
