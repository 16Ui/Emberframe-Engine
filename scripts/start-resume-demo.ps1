param([ValidateSet('pbr','kc','lights','shadows','temporal','geometry')][string]$Demo='pbr',[switch]$Rebuild)
$ErrorActionPreference='Stop'
$demoRoot=Split-Path -Parent $PSScriptRoot
$runtime=& (Join-Path $PSScriptRoot 'configure-vulkan-runtime.ps1')
$demoExe=Join-Path $demoRoot 'bin\Release\emberframe_workbench.exe'
if($Rebuild -or -not(Test-Path -LiteralPath $demoExe)){
    & (Join-Path $PSScriptRoot 'build-windows.ps1') -Config Release -Target emberframe_workbench
    if($LASTEXITCODE -ne 0){throw 'Demo build failed.'}
}
Push-Location $demoRoot
try { & $demoExe --resume-demo $Demo --fit-viewport; if($LASTEXITCODE -ne 0){throw 'Demo failed; see output/workbench.log.'} }
finally {Pop-Location}
