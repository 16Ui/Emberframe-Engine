param([switch]$SkipBuild,[string]$OutputDirectory)
$ErrorActionPreference='Stop'
$engineRoot=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $engineRoot 'bin\Release\emberframe_workbench.exe'
# 诊断会写当前工程的 last-run.json；不要与用户仍在操作的工作台争用。
if(Get-Process emberframe_workbench -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq $exe}){
    throw 'Save and close the current EmberFrame window before display verification.'
}
$out=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $engineRoot ('output\editor-display-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
if(Test-Path -LiteralPath $out){throw 'Choose a new output directory; existing evidence is preserved.'}
New-Item -ItemType Directory -Path $out | Out-Null
$runtime=& (Join-Path $PSScriptRoot 'configure-vulkan-runtime.ps1')
Push-Location $engineRoot
try {
    if(-not $SkipBuild){
        foreach($target in @('emberframe_workbench','emberframe_gpu_tests','emberframe_lab_tests')){
            & (Join-Path $PSScriptRoot 'build-windows.ps1') -Config Release -Target $target
            if($LASTEXITCODE -ne 0){throw "Build failed: $target"}
        }
    }
    $records=[Collections.Generic.List[object]]::new()
    function Invoke-DisplayCase {
        param([string]$Name,[string[]]$Arguments,[switch]$Native,[int]$Width,[int]$Height)
        $log=Join-Path $out ($Name+'.log')
        $capture=Join-Path $out ($Name+'.bmp')
        & $exe --gpu-hidden --frames 12 --resume-demo pbr --screenshot $capture @Arguments 2>&1 | Tee-Object -FilePath $log | Out-Null
        $exitCode=$LASTEXITCODE
        if($exitCode -ne 0){throw "Display case failed: $Name; see $log"}
        if(Select-String -LiteralPath $log -Pattern 'Validation Error|VUID-' -Quiet){throw "Vulkan validation failed: $Name"}
        if(-not (Test-Path -LiteralPath $capture)){throw "No screenshot: $Name"}
        $metrics=Get-Content -LiteralPath (Join-Path $engineRoot 'output\last-run.json') -Raw | ConvertFrom-Json
        if($Native -and ($metrics.renderWidth -ne $metrics.viewportWidth -or $metrics.renderHeight -ne $metrics.viewportHeight)){throw "Native physical resolution mismatch: $Name"}
        if($Width -and ($metrics.renderWidth -ne $Width -or $metrics.renderHeight -ne $Height)){throw "Fixed render resolution mismatch: $Name"}
        if($metrics.fontRasterScale -ne $metrics.displayScale){throw "Font raster DPI mismatch: $Name"}
        if($runtime.validation_configured -and -not (Select-String -LiteralPath $log -Pattern 'VK_LAYER_KHRONOS_validation: enabled' -Quiet)){throw "Validation layer not enabled: $Name"}
        $records.Add([pscustomobject]@{name=$Name;exit=0;screenshot=$capture;metrics=$metrics})
        Write-Output "PASS display $Name"
    }
    Invoke-DisplayCase -Name 'native-window-lifecycle' -Arguments @('--verify-window','--fit-viewport') -Native
    Invoke-DisplayCase -Name 'compact' -Arguments @('--window-width','800','--window-height','600','--fit-viewport') -Native
    Invoke-DisplayCase -Name 'minimum' -Arguments @('--window-width','640','--window-height','480','--fit-viewport') -Native
    Invoke-DisplayCase -Name 'maximized' -Arguments @('--maximized','--fit-viewport') -Native
    Invoke-DisplayCase -Name 'fixed-640x360' -Arguments @('--width','640','--height','360') -Width 640 -Height 360
    Invoke-DisplayCase -Name 'gpu-2560x1440' -Arguments @('--no-ui','--width','2560','--height','1440','--output',(Join-Path $out 'scene-2560x1440.bmp')) -Width 2560 -Height 1440
    foreach($panel in 1..3){Invoke-DisplayCase -Name "panel-$panel" -Arguments @('--ui-panel',"$panel",'--fit-viewport') -Native}
    $cpuLog=Join-Path $out 'algorithm-tests.log'
    & (Join-Path $engineRoot 'bin\Release\emberframe_lab_tests.exe') all 2>&1 | Tee-Object -FilePath $cpuLog | Out-Null
    if($LASTEXITCODE -ne 0){throw "Algorithm regression failed: $cpuLog"}
    $gpuLog=Join-Path $out 'gpu-integration.log'
    & (Join-Path $engineRoot 'bin\Release\emberframe_gpu_tests.exe') (Join-Path $engineRoot 'bin\Release\lab_shaders') (Join-Path $out 'gpu-integration.bmp') 2>&1 | Tee-Object -FilePath $gpuLog | Out-Null
    if($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $gpuLog -Pattern 'Validation Error|VUID-' -Quiet)){throw "GPU regression failed: $gpuLog"}
    $records | ConvertTo-Json -Depth 8 | Out-File -LiteralPath (Join-Path $out 'results.json') -Encoding utf8
    [pscustomobject]@{date=(Get-Date -Format o);programSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash;runtime=$runtime;gpuAndHostPassCount=@(Select-String -LiteralPath $gpuLog -Pattern '^PASS ').Count} | ConvertTo-Json -Depth 5 | Out-File -LiteralPath (Join-Path $out 'environment.json') -Encoding utf8
    Write-Output "PASS algorithms and real GPU integration. Evidence: $out"
}finally{Pop-Location}
