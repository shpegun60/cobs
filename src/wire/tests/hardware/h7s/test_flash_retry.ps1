# Author: shpegun60
# SPDX-License-Identifier: MIT
# Execute only the two runner functions, with every external action mocked.
# No compiler, programmer, COM port or filesystem mutation is invoked.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../../../..')).Path
$tests = 0
foreach ($relative in @('src/cobs/tests/hardware/h7s/run_matrix.ps1',
                         'src/modbus/rtu/tests/hardware/h7s/run_matrix.ps1')) {
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $repo $relative), [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0) { throw "PowerShell parse failure: $relative" }
    foreach ($name in @('Assert-NativeSuccess', 'Build-And-Flash')) {
        $node = $ast.Find({ param($n)
            $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name
        }, $true)
        if (-not $node) { throw "Missing function $name in $relative" }
        . ([scriptblock]::Create($node.Extent.Text))
    }
    function Mock-Build { $global:LASTEXITCODE = 0 }
    function Mock-Inspect { $global:LASTEXITCODE = 0 }
    function Mock-Programmer {
        $script:calls += 1
        if ($script:scenario -eq 'success' -or ($script:scenario -eq 'transient' -and $script:calls -eq 2)) {
            'Download verified successfully'
            $global:LASTEXITCODE = 0
        } elseif ($script:scenario -eq 'unverified') {
            'Download ended without positive verification'
            $global:LASTEXITCODE = 0
        } elseif ($script:scenario -eq 'unknown') {
            'Error: unsupported target'
            $global:LASTEXITCODE = 1
        } elseif ($script:scenario -eq 'bad-exit') {
            'Download verified successfully'
            $global:LASTEXITCODE = 1
        } else {
            'Error: failed to download Sector[0]'
            $global:LASTEXITCODE = 1
        }
    }
    function New-Item { }
    function Copy-Item { }
    function Start-Sleep { }
    function Write-Host { }
    function Write-Warning { }
    $GitBash = 'Mock-Build'
    $Python = 'Mock-Inspect'
    $CubeProgrammer = 'Mock-Programmer'
    $elf = 'C:/unused/audit.elf'
    $buildScript = $inspector = 'unused'
    $StLinkSerial = 'unused'
    $Optimization = 'Os'
    $Lto = 0
    foreach ($case in @(@('success', 1, $false), @('transient', 2, $false), @('persistent', 3, $true),
                        @('unknown', 1, $true), @('unverified', 1, $true), @('bad-exit', 1, $true))) {
        $script:scenario = $case[0]
        $script:calls = 0
        $threw = $false
        try { Build-And-Flash 115200 'bitwise' } catch { $threw = $true }
        if ($script:calls -ne $case[1] -or $threw -ne $case[2]) {
            throw "$relative $($case[0]): calls=$script:calls threw=$threw"
        }
        $tests += 1
    }
}
Write-Output "PASS $tests bounded-flash-retry cases; no hardware accessed"
