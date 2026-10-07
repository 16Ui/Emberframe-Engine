param([switch]$Rebuild, [switch]$Test, [Parameter(ValueFromRemainingArguments=$true)][string[]]$Arguments)
$ErrorActionPreference = 'Stop'
$engineRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $engineRoot 'bin\Release\emberframe_workbench.exe'
Push-Location $engineRoot
try {
    if ($Rebuild -or -not (Test-Path -LiteralPath $executable)) {
        & (Join-Path $PSScriptRoot 'build-windows.ps1') -Config Release -Target emberframe_workbench
        if ($LASTEXITCODE -ne 0) { throw 'Workbench build failed; will not start a stale binary.' }
    }
    if ($Test) {
        & (Join-Path $PSScriptRoot 'build-windows.ps1') -Config Release -Target emberframe_lab_tests
        if ($LASTEXITCODE -ne 0) { throw 'Algorithm checks build failed.' }
        & (Join-Path $engineRoot 'bin\Release\emberframe_lab_tests.exe')
        if ($LASTEXITCODE -ne 0) { throw 'Algorithm checks failed.' }
    }
    & $executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Workbench exited with code $LASTEXITCODE." }
} finally { Pop-Location }
