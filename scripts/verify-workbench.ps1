param([switch]$Gpu, [switch]$SkipBuild, [string]$OutputDirectory)
$ErrorActionPreference='Stop'
$engineRoot=Split-Path -Parent $PSScriptRoot
$runtime=& (Join-Path $PSScriptRoot 'configure-vulkan-runtime.ps1')
Push-Location $engineRoot
try {
$out=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $engineRoot 'output\verification'}
if($OutputDirectory -and (Test-Path -LiteralPath $out)){throw 'Choose a new verification directory; previous evidence is preserved.'}
New-Item -ItemType Directory -Force -Path $out | Out-Null
if(-not $SkipBuild){
    & (Join-Path $PSScriptRoot 'build-windows.ps1') -Config Release -Target emberframe_workbench
    if($LASTEXITCODE -ne 0){throw 'Workbench build failed; verification will not reuse a stale binary.'}
    & (Join-Path $PSScriptRoot 'build-windows.ps1') -Config Release -Target emberframe_lab_tests
    if($LASTEXITCODE -ne 0){throw 'CPU tests build failed.'}
    if($Gpu){
        & (Join-Path $PSScriptRoot 'build-windows.ps1') -Config Release -Target emberframe_gpu_tests
        if($LASTEXITCODE -ne 0){throw 'GPU tests build failed.'}
    }
}
$exe=Join-Path $engineRoot 'bin\Release\emberframe_workbench.exe'
$checks=Join-Path $engineRoot 'bin\Release\emberframe_lab_tests.exe'
$records=[Collections.Generic.List[object]]::new()
function Invoke-Case {
    param([string]$Name,[string]$Program,[string[]]$Arguments)
    $timer=[Diagnostics.Stopwatch]::StartNew()
    $log=Join-Path $out ($Name+'.log')
    & $Program @Arguments 2>&1 | Tee-Object -FilePath $log
    $exitCode=$LASTEXITCODE
    if(Select-String -LiteralPath $log -Pattern 'Validation Error|VUID-' -Quiet){$exitCode=1}
    $usesVulkan=($Name -eq 'gpu-integration' -or $Arguments -contains '--gpu-hidden' -or $Arguments -contains '--smoke-test' -or $Arguments -contains '--compare')
    $layerEnabled=if($usesVulkan){[bool](Select-String -LiteralPath $log -Pattern 'VK_LAYER_KHRONOS_validation: enabled' -Quiet)}else{$null}
    if($usesVulkan -and $runtime.validation_configured -and -not $layerEnabled){$exitCode=1}
    $timer.Stop()
    $records.Add([PSCustomObject]@{name=$Name;exit=$exitCode;seconds=$timer.Elapsed.TotalSeconds;log=$log;validation_layer_enabled=$layerEnabled})
}
Invoke-Case -Name 'algorithm-tests' -Program $checks -Arguments @('all')
foreach($mode in 0..6){
    Invoke-Case -Name "shadow-$mode" -Program $exe -Arguments @('--headless','--preset','2','--width','128','--height','72','--shadow',"$mode",'--output',(Join-Path $out "shadow-$mode.bmp"))
}
foreach($mode in 0..6){
    Invoke-Case -Name "gi-$mode" -Program $exe -Arguments @('--headless','--preset','1','--width','96','--height','64','--samples','4','--gi',"$mode",'--output',(Join-Path $out "gi-$mode.bmp"))
}
foreach($mode in 1..2){
    Invoke-Case -Name "ao-$mode" -Program $exe -Arguments @('--headless','--preset','1','--width','128','--height','72','--ao',"$mode",'--output',(Join-Path $out "ao-$mode.bmp"))
}
Invoke-Case -Name 'path-reference' -Program $exe -Arguments @('--headless','--preset','1','--path','trace','--width','96','--height','64','--samples','8','--output',(Join-Path $out 'path-reference.bmp'))
foreach($diffuse in 1..2){Invoke-Case -Name "scene-diffuse-$diffuse" -Program $exe -Arguments @('--headless','--preset','2','--width','96','--height','64','--environment-diffuse',"$diffuse",'--bake-samples','16','--output',(Join-Path $out "scene-diffuse-$diffuse.bmp"))}
Invoke-Case -Name 'scene-sdf-reference' -Program $exe -Arguments @('--headless','--preset','2','--width','96','--height','64','--sdf-shadows','--sdf-resolution','16','--output',(Join-Path $out 'scene-sdf-reference.bmp'))
Invoke-Case -Name 'scene-octree-path' -Program $exe -Arguments @('--headless','--preset','1','--path','trace','--spatial','1','--width','64','--height','40','--samples','2','--output',(Join-Path $out 'scene-octree-path.bmp'))
foreach($topic in @(11,12,13,30)){
    Invoke-Case -Name "topic-$topic" -Program $exe -Arguments @('--headless','--topic',"$topic",'--width','160','--height','96','--samples','4','--output',(Join-Path $out "topic-$topic.bmp"))
}
foreach($mode in @('taa','denoise','svgf')){
    Invoke-Case -Name "temporal-$mode" -Program $exe -Arguments @('--headless','--preset','1','--path','trace','--width','96','--height','64','--samples','1','--frames','4',"--$mode",'--output',(Join-Path $out "temporal-$mode.bmp"))
}
if($Gpu){
    foreach($path in @('forward','deferred')){
        foreach($diffuse in 1..2){Invoke-Case -Name "native-$path-diffuse-$diffuse" -Program $exe -Arguments @('--gpu-hidden','--no-ui','--path',$path,'--frames','12','--preset','2','--width','128','--height','80','--environment-diffuse',"$diffuse",'--bake-samples','16','--output',(Join-Path $out "native-$path-diffuse-$diffuse.bmp"))}
        Invoke-Case -Name "native-$path-sdf" -Program $exe -Arguments @('--gpu-hidden','--no-ui','--path',$path,'--frames','12','--preset','2','--width','128','--height','80','--sdf-shadows','--sdf-resolution','16','--output',(Join-Path $out "native-$path-sdf.bmp"))
    }
    Invoke-Case -Name 'native-voxel-dense-reference' -Program $exe -Arguments @('--gpu-hidden','--no-ui','--frames','8','--preset','2','--width','128','--height','80','--gi','5','--dense-voxels','--output',(Join-Path $out 'native-voxel-dense-reference.bmp'))
    Invoke-Case -Name 'gpu-integration' -Program (Join-Path $engineRoot 'bin\Release\emberframe_gpu_tests.exe') -Arguments @((Join-Path $engineRoot 'bin\Release\lab_shaders'),(Join-Path $out 'gpu-integration.bmp'))
    Invoke-Case -Name 'cpu-gpu-compare' -Program $exe -Arguments @('--compare','--width','320','--height','180')
    foreach($path in @('forward','deferred')){
        Invoke-Case -Name "gpu-$path" -Program $exe -Arguments @('--path',$path,'--smoke-test','--screenshot',(Join-Path $out "gpu-$path.bmp"))
    }
    # 原生编辑器逐页呈现；窗口尺寸与离屏尺寸分开，窄窗口必须能保留可操作检查器。
    foreach($panel in 1..3){
        Invoke-Case -Name "editor-panel-$panel" -Program $exe -Arguments @('--gpu-hidden','--frames','12','--ui-panel',"$panel",'--screenshot',(Join-Path $out "editor-panel-$panel.bmp"))
    }
    Invoke-Case -Name 'editor-compact' -Program $exe -Arguments @('--gpu-hidden','--frames','12','--window-width','800','--window-height','600','--screenshot',(Join-Path $out 'editor-compact.bmp'))
    # --gpu-hidden 是真实显卡，--headless 是 CPU；输出 PFM/BMP 来自 GPU HDR 读回。
    foreach($mode in 0..6){Invoke-Case -Name "native-shadow-$mode" -Program $exe -Arguments @('--gpu-hidden','--no-ui','--frames','8','--preset','2','--width','160','--height','96','--shadow',"$mode",'--output',(Join-Path $out "native-shadow-$mode.bmp"))}
    foreach($mode in 0..6){$giPreset=if($mode -ge 3 -and $mode -le 5){'2'}else{'1'};Invoke-Case -Name "native-gi-$mode" -Program $exe -Arguments @('--gpu-hidden','--no-ui','--frames','8','--preset',$giPreset,'--width','128','--height','80','--samples','4','--gi',"$mode",'--output',(Join-Path $out "native-gi-$mode.bmp"))}
    foreach($mode in 1..2){Invoke-Case -Name "native-ao-$mode" -Program $exe -Arguments @('--gpu-hidden','--no-ui','--frames','8','--preset','1','--width','128','--height','80','--ao',"$mode",'--debug','6','--output',(Join-Path $out "native-ao-$mode.bmp"))}
    foreach($mode in @('taa','denoise','svgf')){Invoke-Case -Name "native-$mode" -Program $exe -Arguments @('--gpu-hidden','--no-ui','--frames','16','--preset','1','--width','128','--height','80','--gi','2',"--$mode",'--output',(Join-Path $out "native-$mode.bmp"))}
    Invoke-Case -Name 'native-disney-npr-filter' -Program $exe -Arguments @('--gpu-hidden','--no-ui','--frames','8','--preset','0','--width','160','--height','96','--shading','2','--outline','--hatching','--filter','3','--output',(Join-Path $out 'native-disney-npr-filter.bmp'))
}
$records | ConvertTo-Json -Depth 5 | Out-File -LiteralPath (Join-Path $out 'results.json') -Encoding utf8
$runtime | ConvertTo-Json -Depth 3 | Out-File -LiteralPath (Join-Path $out 'runtime.json') -Encoding utf8
$failed=@($records | Where-Object {$_.exit -ne 0})
Write-Output ("Verification: {0}/{1} cases passed. Reports: {2}" -f ($records.Count-$failed.Count),$records.Count,$out)
if($failed.Count){exit 1}
} finally { Pop-Location }
