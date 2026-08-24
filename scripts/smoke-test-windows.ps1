param(
    [ValidateRange(1, 60)]
    [int]$Seconds = 8,

    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $repoRoot "bin\$Config\chapter_6.exe"
$logDirectory = Join-Path $repoRoot "build"
$stdoutLog = Join-Path $logDirectory "chapter6-smoke.stdout.log"
$stderrLog = Join-Path $logDirectory "chapter6-smoke.stderr.log"

if (-not (Test-Path -LiteralPath $executable)) {
    throw "Executable not found: $executable. Build the project first."
}

New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$process = Start-Process -FilePath $executable `
    -WorkingDirectory (Split-Path $executable) `
    -PassThru `
    -WindowStyle Hidden `
    -RedirectStandardOutput $stdoutLog `
    -RedirectStandardError $stderrLog

Start-Sleep -Seconds $Seconds
$process.Refresh()

if ($process.HasExited) {
    $stderrText = if (Test-Path -LiteralPath $stderrLog) {
        Get-Content -LiteralPath $stderrLog -Raw
    } else {
        ""
    }
    throw "chapter_6 exited early with code $($process.ExitCode).`n$stderrText"
}

Stop-Process -Id $process.Id
Wait-Process -Id $process.Id -ErrorAction SilentlyContinue
Write-Host "Smoke test passed: chapter_6 remained active for $Seconds seconds."
