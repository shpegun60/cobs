# Author: shpegun60
# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^COM[0-9]+$')][string]$Port,
    [Parameter(Mandatory = $true)][string]$StLinkSerial,
    [int[]]$BaudRates = @(115200, 1000000, 3000000, 6000000, 10000000),
    [ValidateSet('none', 'bitwise', 'table')][string[]]$Policies = @('none', 'bitwise', 'table'),
    [ValidateSet(253, 1024)][int[]]$MaxPayloads = @(253, 1024),
    [ValidateRange(1, 60)][int]$Seconds = 2,
    [ValidateRange(1, 20)][int]$Repeats = 2,
    [string[]]$Cases = @(),
    [Parameter(Mandatory = $true)][string]$Output,
    [string]$CubeProgrammer = 'C:\ST\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe',
    [string]$GitBash = 'C:\Program Files\Git\bin\bash.exe',
    [string]$Python = 'python'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..\..')).Path
if (-not [IO.Path]::IsPathRooted($Output)) { $Output = Join-Path $repo $Output }
$Output = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $Output) { throw "Refusing to overwrite/append an existing session: $Output" }
foreach ($baud in $BaudRates) {
    if ($baud -notin @(115200, 1000000, 3000000, 6000000, 10000000)) { throw "Unsupported baud $baud" }
}
if ($BaudRates.Count -eq 0 -or $Policies.Count -eq 0 -or $MaxPayloads.Count -eq 0) { throw 'Empty matrix' }
foreach ($exe in @($CubeProgrammer, $GitBash, $Python)) {
    if (-not (Get-Command $exe -ErrorAction SilentlyContinue)) { throw "Missing tool $exe" }
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$session = Join-Path $repo "cobs\tests\out\performance-$stamp"
if (Test-Path -LiteralPath $session) { throw "Existing session directory $session" }
[void](New-Item -ItemType Directory -Path $session)
$backup = Join-Path $session 'before.bin'
$elf = Join-Path $repo 'stm32_cube_test\h7s_cobs_test\out\cobs-hardware\cobs_hardware_bench.elf'
$previousBaud = $env:COBS_HW_BAUD
$previousCrc = $env:COBS_HW_CRC
$previousPayload = $env:COBS_HW_MAX_PAYLOAD
$backedUp = $false
$mutated = $false
$completed = $false

function Check-Exit([string]$Operation) {
    if ($LASTEXITCODE -ne 0) { throw "$Operation failed ($LASTEXITCODE)" }
}
Push-Location $repo
try {
    # This harness only programs the board's 64 KiB internal boot flash.
    & $CubeProgrammer -c port=SWD "sn=$StLinkSerial" mode=UR reset=HWrst freq=4000 `
        -u 0x08000000 0x10000 $backup -rst 2>&1 | Tee-Object -FilePath (Join-Path $session 'backup.log')
    Check-Exit 'backup'
    if ((Get-Item -LiteralPath $backup).Length -ne 65536) { throw 'Backup size mismatch' }
    $backedUp = $true
    foreach ($maximum in $MaxPayloads) {
        foreach ($baud in $BaudRates) {
            # Rotate the policy order across bauds; all policies see the same corpus.
            $offset = [array]::IndexOf($BaudRates, $baud) % $Policies.Count
            for ($index = 0; $index -lt $Policies.Count; ++$index) {
                $policy = $Policies[($index + $offset) % $Policies.Count]
                $tag = "$policy-$maximum-$baud"
                $env:COBS_HW_BAUD = [string]$baud
                $env:COBS_HW_CRC = [string]@{none=0; bitwise=1; table=2}[$policy]
                $env:COBS_HW_MAX_PAYLOAD = [string]$maximum
                Write-Host "BUILD $tag"
                & $GitBash (Join-Path $PSScriptRoot 'build.sh') *> (Join-Path $session "$tag-build.log")
                Check-Exit "build $tag"
                Copy-Item -LiteralPath $elf -Destination (Join-Path $session "$tag.elf")
                $mutated = $true
                & $CubeProgrammer -c port=SWD "sn=$StLinkSerial" mode=UR reset=HWrst freq=4000 `
                    -w $elf -v -rst *> (Join-Path $session "$tag-flash.log")
                Check-Exit "flash/verify $tag"
                $arguments = @('-B', (Join-Path $PSScriptRoot 'cobs_performance.py'), $Port,
                    '--baud', [string]$baud, '--crc', $policy, '--max-payload', [string]$maximum,
                    '--seconds', [string]$Seconds, '--repeats', [string]$Repeats,
                    '--stlink-serial', $StLinkSerial, '--output', $Output)
                foreach ($case in $Cases) { $arguments += @('--case', $case) }
                & $Python @arguments 2>&1 | Tee-Object -FilePath (Join-Path $session "$tag-measure.log")
                Check-Exit "measurement $tag"
            }
        }
    }
    $completed = $true
} finally {
    try {
        if ($backedUp -and $mutated) {
            Write-Host 'Restoring and verifying the original board firmware...'
            & $CubeProgrammer -c port=SWD "sn=$StLinkSerial" mode=UR reset=HWrst freq=4000 `
                -w $backup 0x08000000 -v -rst 2>&1 | Tee-Object -FilePath (Join-Path $session 'restore.log')
            Check-Exit 'original firmware restore'
            $receipt = [ordered]@{
                completed = $completed; restored_and_verified = $true
                stlink_serial = $StLinkSerial; backup_bytes = 65536
                backup_sha256 = (Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash.ToLowerInvariant()
                results_sha256 = if (Test-Path -LiteralPath $Output) {
                    (Get-FileHash -LiteralPath $Output -Algorithm SHA256).Hash.ToLowerInvariant()
                } else { $null }
                session = $session; finished_utc = (Get-Date).ToUniversalTime().ToString('o')
            }
            # Serialization is generated test output, not a source-file edit.
            $receipt | ConvertTo-Json | Set-Content -LiteralPath "$Output.session.json" -Encoding UTF8
        }
    } finally {
        $env:COBS_HW_BAUD = $previousBaud
        $env:COBS_HW_CRC = $previousCrc
        $env:COBS_HW_MAX_PAYLOAD = $previousPayload
        Pop-Location
    }
}
Write-Host "PERFORMANCE MATRIX PASS: $Output"
