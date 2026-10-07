param([switch]$SkipBuild,[string]$OutputDirectory)
$ErrorActionPreference='Stop'
$engineRoot=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $engineRoot 'bin\Release\emberframe_workbench.exe'
if(Get-Process emberframe_workbench -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq $exe}){
    throw 'Save and close the current EmberFrame window before verification.'
}
$out=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $engineRoot ('output\colored-room-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
if(Test-Path -LiteralPath $out){throw 'Choose a new evidence directory; previous results are preserved.'}
New-Item -ItemType Directory -Path $out | Out-Null
$runtime=& (Join-Path $PSScriptRoot 'configure-vulkan-runtime.ps1')
Push-Location $engineRoot
try {
    if(-not $SkipBuild){
        foreach($target in @('emberframe_workbench','emberframe_gpu_tests','emberframe_lighting_tests','emberframe_lab_tests')){
            & (Join-Path $PSScriptRoot 'build-windows.ps1') -Config Release -Target $target
            if($LASTEXITCODE -ne 0){throw "Build failed: $target"}
        }
    }
    $algorithmLog=Join-Path $out 'algorithm-tests.log'
    & (Join-Path $engineRoot 'bin\Release\emberframe_lab_tests.exe') all 2>&1 | Tee-Object -FilePath $algorithmLog | Out-Null
    if($LASTEXITCODE -ne 0){throw "Algorithm regression failed: $algorithmLog"}
    $lightingLog=Join-Path $out 'lighting-numerical.log'
    & (Join-Path $engineRoot 'bin\Release\emberframe_lighting_tests.exe') (Join-Path $engineRoot 'bin\Release\lab_shaders\gpu_lighting_check.comp.spv') 2>&1 | Tee-Object -FilePath $lightingLog | Out-Null
    if($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $lightingLog -Pattern '^VALIDATION |VUID-' -Quiet)){throw "Area BRDF numerical regression failed: $lightingLog"}
    $gpuLog=Join-Path $out 'gpu-integration.log'
    & (Join-Path $engineRoot 'bin\Release\emberframe_gpu_tests.exe') (Join-Path $engineRoot 'bin\Release\lab_shaders') (Join-Path $out 'gpu-integration.bmp') 2>&1 | Tee-Object -FilePath $gpuLog | Out-Null
    if($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $gpuLog -Pattern 'Validation Error|VUID-' -Quiet)){throw "GPU regression failed: $gpuLog"}
    if($runtime.validation_configured -and -not (Select-String -LiteralPath $gpuLog -Pattern 'VK_LAYER_KHRONOS_validation: enabled' -Quiet)){throw 'Validation was configured but not enabled.'}
    $records=[Collections.Generic.List[object]]::new()
    foreach($panel in 0..1){
        $log=Join-Path $out "room-ui-$panel.log"
        $capture=Join-Path $out "room-ui-$panel.bmp"
        & $exe --gpu-hidden --preset 1 --frames 24 --ui-panel "$panel" --fit-viewport --shadow 2 --no-bloom --screenshot $capture 2>&1 | Tee-Object -FilePath $log | Out-Null
        if($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $log -Pattern 'Validation Error|VUID-' -Quiet)){throw "Room UI case failed: $log"}
        if(-not(Test-Path -LiteralPath $capture)){throw "Screenshot missing: $capture"}
        $metrics=Get-Content -LiteralPath (Join-Path $engineRoot 'output\last-run.json') -Raw | ConvertFrom-Json
        if($metrics.renderWidth -ne $metrics.viewportWidth -or $metrics.renderHeight -ne $metrics.viewportHeight){throw 'Room viewport is not native physical resolution.'}
        if($metrics.displayScale -ne $metrics.fontRasterScale){throw 'Font atlas does not match display DPI.'}
        $records.Add([pscustomobject]@{panel=$panel;screenshot=$capture;metrics=$metrics})
    }
    [pscustomobject]@{date=(Get-Date -Format o);programSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash;runtime=$runtime;algorithmPassCount=@(Select-String -LiteralPath $algorithmLog -Pattern '^PASS ').Count;gpuAndHostPassCount=@(Select-String -LiteralPath $gpuLog -Pattern '^PASS ').Count;ui=$records} |
        ConvertTo-Json -Depth 8 | Out-File -LiteralPath (Join-Path $out 'results.json') -Encoding utf8
    Write-Output "PASS colored room and full algorithm/GPU regression. Evidence: $out"
}finally{Pop-Location}
