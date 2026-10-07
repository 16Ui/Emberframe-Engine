param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$engineRoot=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $engineRoot 'bin\Release\emberframe_workbench.exe'
if(Get-Process emberframe_workbench -ErrorAction SilentlyContinue){throw 'Please save and close EmberFrame before verification.'}
Push-Location $engineRoot
try{
    if(-not $SkipBuild){
        cmake --build build --config Release --target emberframe_workbench emberframe_lab_tests emberframe_gpu_tests --parallel
        if($LASTEXITCODE -ne 0){throw 'Editor build failed.'}
    }
    $out=Join-Path $engineRoot ('output\editor-assets-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
    if(Test-Path -LiteralPath $out){throw 'Evidence directory exists; retry with a new timestamp.'}
    New-Item -ItemType Directory -Path $out | Out-Null
    $runtime=& (Join-Path $PSScriptRoot 'configure-vulkan-runtime.ps1')
    $log=Join-Path $out 'algorithm-tests.log'
    & (Join-Path $engineRoot 'bin\Release\emberframe_lab_tests.exe') all 2>&1 | Tee-Object -FilePath $log | Out-Null
    if($LASTEXITCODE -ne 0){throw "Algorithm/editor tests failed: $log"}
    $records=[Collections.Generic.List[object]]::new()
    foreach($route in 'forward','deferred'){
        $gpuLog=Join-Path $out "$route.log"
        & $exe --gpu-hidden --preset 1 --verify-editor --width 800 --height 450 --path $route --frames 40 --no-bloom --ui-panel 1 --output (Join-Path $out "$route.bmp") --screenshot (Join-Path $out "$route-ui.bmp") 2>&1 |
            Tee-Object -FilePath $gpuLog | Out-Null
        if($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $gpuLog -Pattern 'Validation Error|VUID-' -Quiet)){throw "Editor GPU case failed: $gpuLog"}
        if($runtime.validation_configured -and -not(Select-String -LiteralPath $gpuLog -Pattern 'VK_LAYER_KHRONOS_validation: enabled' -Quiet)){throw 'Configured validation layer was not loaded.'}
        if(@(Select-String -LiteralPath $gpuLog -Pattern '^PASS editor ').Count -ne 3){throw 'Editor action diagnostic did not complete.'}
        if(-not(Select-String -LiteralPath $gpuLog -Pattern '^PASS GPU continuous editor model/light dragging: 6 frames, zero static upload bytes' -Quiet)){throw 'Continuous editor dragging did not reuse resident assets.'}
        $metrics=Get-Content -LiteralPath (Join-Path $engineRoot 'output\last-run.json') -Raw | ConvertFrom-Json
        if($metrics.actualPath -notin @(0,1)){throw 'Editor verification used CPU fallback.'}
        $records.Add([pscustomobject]@{route=$route;metrics=$metrics;log=$gpuLog;screenshot=(Join-Path $out "$route-ui.bmp")})
    }
    [pscustomobject]@{date=(Get-Date -Format o);programSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash;runtime=$runtime;tests=@(Select-String -LiteralPath $log -Pattern '^PASS ').Count;cases=$records} |
        ConvertTo-Json -Depth 8 | Out-File -LiteralPath (Join-Path $out 'results.json') -Encoding utf8
    Write-Output "PASS editor model/texture/light/gizmo/undo/persistence integration. Evidence: $out"
}finally{Pop-Location}
