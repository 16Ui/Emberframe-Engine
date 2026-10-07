param([switch]$SkipBuild,[string]$OutputDirectory)
$ErrorActionPreference='Stop'
$engineRoot=Split-Path -Parent $PSScriptRoot
if(Get-Process emberframe_workbench -ErrorAction SilentlyContinue){throw '请保存并关闭 EmberFrame；检查不会关闭用户窗口。'}
Push-Location $engineRoot
try {
    if(-not $SkipBuild){
        cmake --build build --config Release --target emberframe_workbench emberframe_lab_tests emberframe_gpu_tests --parallel 8
        if($LASTEXITCODE -ne 0){throw '编译失败，不检查旧程序。'}
    }
    $auditRoot=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $engineRoot ('output\verification\rendering-editor-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
    if(Test-Path -LiteralPath $auditRoot){throw '证据目录已存在，不覆盖旧结果。'}
    New-Item -ItemType Directory -Path $auditRoot | Out-Null
    $runtime=& .\scripts\configure-vulkan-runtime.ps1
    $exe=Join-Path $engineRoot 'bin\Release\emberframe_workbench.exe'
    & .\bin\Release\emberframe_lab_tests.exe all *> (Join-Path $auditRoot 'cpu-tests.log')
    if($LASTEXITCODE -ne 0){throw 'CPU / 编辑逻辑回归失败。'}
    $cpuSummary=Select-String -LiteralPath (Join-Path $auditRoot 'cpu-tests.log') -Pattern '^RESULT passed=(\d+) failed=0$'
    if(-not $cpuSummary){throw 'CPU 汇总缺失，不能记录为通过。'}
    $cpuPasses=[int]$cpuSummary.Matches[0].Groups[1].Value
    $gpuLog=Join-Path $auditRoot 'gpu-self-tests.log'
    & .\bin\Release\emberframe_gpu_tests.exe .\bin\Release\lab_shaders (Join-Path $auditRoot 'gpu-self-test-ui.bmp') *> $gpuLog
    if($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $gpuLog -Pattern '^FAIL |VUID-|Validation Error' -Quiet)){throw '真实 GPU 回归失败。'}
    $gpuPasses=@(Select-String -LiteralPath $gpuLog -Pattern '^PASS ').Count
    Write-Output "REGRESSION CPU=$cpuPasses GPU=$gpuPasses"
    $cases=[Collections.Generic.List[object]]::new()
    foreach($preset in 0..3){foreach($gi in 3..5){
        $cases.Add(@{name="scene-$preset-gi-$gi";expectedGi=$gi;args=@('--preset',"$preset",'--gi',"$gi")})
    }}
    foreach($gi in 3..5){$cases.Add(@{name="empty-gi-$gi";expectedGi=$gi;args=@('--new-scene','--gi',"$gi")})}
    foreach($gi in 3..5){$cases.Add(@{name="point-room-gi-$gi";expectedGi=$gi;expectedDebug=8;args=@('--project',(Join-Path $engineRoot 'assets\verification\point-room.ember'),'--gi',"$gi",'--debug','8')})}
    foreach($preset in 0..1){foreach($gi in 1..2){
        $cases.Add(@{name="screen-$preset-gi-$gi";expectedGi=$gi;args=@('--preset',"$preset",'--gi',"$gi",'--samples','32','--svgf')})
        $cases.Add(@{name="screen-raw-$preset-gi-$gi";expectedGi=$gi;args=@('--preset',"$preset",'--gi',"$gi",'--samples','8')})
    }}
    foreach($path in @('forward','deferred')){
        $cases.Add(@{name="editor-$path";args=@('--preset','1','--path',$path,'--verify-editor','--ui-panel','1')})
    }
    $records=[Collections.Generic.List[object]]::new()
    foreach($case in $cases){
        Write-Output ('START '+$case.name)
        $log=Join-Path $auditRoot ($case.name+'.log')
        $image=Join-Path $auditRoot ($case.name+'.bmp')
        $arguments=@('--gpu-hidden','--width','960','--height','540','--window-width','1600','--window-height','900','--frames','40','--no-bloom','--output',$image)+$case.args
        if($case.name -like 'editor-*'){$arguments+=@('--screenshot',(Join-Path $auditRoot ($case.name+'-ui.bmp')))}
        $timer=[Diagnostics.Stopwatch]::StartNew()
        & $exe @arguments *> $log
        $exit=$LASTEXITCODE;$timer.Stop()
        $metrics=$null
        if($exit -eq 0){$metrics=Get-Content output\last-run.json -Raw | ConvertFrom-Json}
        $validationError=Select-String -LiteralPath $log -Pattern 'Validation Error|VUID-' -Quiet
        $validationLoaded=Select-String -LiteralPath $log -Pattern 'VK_LAYER_KHRONOS_validation: enabled' -Quiet
        $passed=$exit -eq 0 -and -not $validationError -and (Test-Path -LiteralPath $image) -and $metrics.actualPath -in @(0,1) -and (-not $runtime.validation_configured -or $validationLoaded)
        if($case.ContainsKey('expectedGi')){$passed=$passed -and $metrics.giMode -eq $case.expectedGi}
        if($case.ContainsKey('expectedDebug')){$passed=$passed -and $metrics.debugView -eq $case.expectedDebug}
        $passed=$passed -and $metrics.renderWidth -eq 960 -and $metrics.renderHeight -eq 540
        if($case.name -like 'editor-*'){$passed=$passed -and (Select-String -LiteralPath $log -Pattern '^PASS object/resource selection isolation' -Quiet)}
        $records.Add([pscustomobject]@{name=$case.name;passed=$passed;exit=$exit;seconds=$timer.Elapsed.TotalSeconds;metrics=$metrics;log=$log;image=$image})
        [pscustomobject]@{schema='emberframe.rendering-editor-verification.v1';time=(Get-Date -Format o);programSha256=(Get-FileHash $exe -Algorithm SHA256).Hash;cpuPassed=$cpuPasses;gpuSelfTestsPassed=$gpuPasses;runtime=$runtime;manualMouseReviewRequired=$true;cases=$records} |
            ConvertTo-Json -Depth 8 | Out-File (Join-Path $auditRoot 'summary.json') -Encoding utf8
        Write-Output ((@('FAIL','PASS')[[int]$passed])+' '+$case.name+' ('+[Math]::Round($timer.Elapsed.TotalSeconds,1)+'s)')
    }
    $failed=@($records | Where-Object {-not $_.passed})
    Write-Output ('RESULT '+($records.Count-$failed.Count)+'/'+$records.Count+'; evidence: '+$auditRoot)
    if($failed.Count){exit 1}
} finally {Pop-Location}
