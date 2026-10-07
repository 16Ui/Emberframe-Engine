param(
    [switch]$SkipBuild,
    [switch]$SkipDemoPack,
    [string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$engineRoot=Split-Path -Parent $PSScriptRoot
if(Get-Process emberframe_workbench -ErrorAction SilentlyContinue){throw '请先保存并关闭 EmberFrame；检查不会关闭用户窗口。'}
$auditRoot=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $engineRoot ('output\verification\demo-ready-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
if(Test-Path -LiteralPath $auditRoot){throw '证据目录已存在，请选择新目录，避免覆盖旧报告。'}
New-Item -ItemType Directory -Path $auditRoot | Out-Null
$shellPath=(Get-Process -Id $PID).Path
Push-Location $engineRoot
try {
    if(-not $SkipBuild){
        foreach($targetName in @('emberframe_workbench','emberframe_lab_tests','emberframe_gpu_tests','emberframe_lighting_tests','emberframe_demo_video')){
            & $shellPath -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-windows.ps1') -Config Release -Target $targetName *> (Join-Path $auditRoot ('build-'+$targetName+'.log'))
            if($LASTEXITCODE -ne 0){throw ('编译失败：'+$targetName+'；不会检查旧程序。')}
        }
    }
    $exe=Join-Path $engineRoot 'bin\Release\emberframe_workbench.exe'
    $programHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    # Shader 是独立运行资产；仅比较 exe 哈希不能证明渲染版本一致。
    $shaderHashes=@(Get-ChildItem -LiteralPath (Join-Path $engineRoot 'bin\Release\lab_shaders') -Filter '*.spv' -File | Sort-Object Name | ForEach-Object {
        [pscustomobject]@{name=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
    })
    $cases=@(
        @{name='models';file='verify-model-library.ps1';args=@('-OutputDirectory',(Join-Path $auditRoot 'models'))},
        @{name='rendering';file='verify-workbench.ps1';args=@('-Gpu','-SkipBuild','-OutputDirectory',(Join-Path $auditRoot 'rendering'))},
        @{name='display';file='verify-editor-display.ps1';args=@('-SkipBuild','-OutputDirectory',(Join-Path $auditRoot 'display'))},
        @{name='colored-room';file='verify-colored-room.ps1';args=@('-SkipBuild','-OutputDirectory',(Join-Path $auditRoot 'colored-room'))},
        @{name='editor';file='verify-editor-assets.ps1';args=@('-SkipBuild')}
    )
    if(-not $SkipDemoPack){$cases+=@{name='demo-pack';file='export-resume-demo.ps1';args=@('-SkipBuild','-OutputDirectory',(Join-Path $auditRoot 'demo-pack'),'-SequenceFrames','48','-BenchmarkFrames','30','-Width','640','-Height','360')}}
    $records=[Collections.Generic.List[object]]::new()
    foreach($case in $cases){
        Write-Output ('START '+$case.name)
        $timer=[Diagnostics.Stopwatch]::StartNew()
        $log=Join-Path $auditRoot ($case.name+'.log')
        $arguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot $case.file))+$case.args
        # 每个检查使用独立 PowerShell 子进程，明确取得退出码；所有 GPU 检查串行，
        # 防止争抢 last-run.json / 窗口状态，也不会把空的 LASTEXITCODE 当作失败。
        & $shellPath @arguments *> $log
        $code=$LASTEXITCODE
        $timer.Stop()
        $records.Add([pscustomobject]@{name=$case.name;passed=($code -eq 0);exit=$code;seconds=$timer.Elapsed.TotalSeconds;log=$log})
        @{schema='emberframe.demonstration-verification.v1';program=$exe;sha256=$programHash;shaders=$shaderHashes;time=(Get-Date -Format o);cases=$records;manual_ui_review_required=$true;scope='Rendering, representative local models, DPI/windowing, editor assets, demo captures and GPU timings'} | ConvertTo-Json -Depth 8 | Out-File -LiteralPath (Join-Path $auditRoot 'summary.json') -Encoding utf8
        Write-Output ((@('FAIL','PASS')[[int]($code -eq 0)])+' '+$case.name+' ('+[Math]::Round($timer.Elapsed.TotalSeconds,1)+'s)')
    }
    $failed=@($records | Where-Object {-not $_.passed})
    Write-Output ('Demonstration suites: '+($records.Count-$failed.Count)+'/'+$records.Count+' passed. Evidence: '+$auditRoot)
    if($failed.Count){exit 1}
    exit 0
} finally {Pop-Location}
