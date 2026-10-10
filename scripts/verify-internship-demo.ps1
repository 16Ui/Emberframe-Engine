param([switch]$SkipBuild,[string]$OutputDirectory)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repoRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(Get-Process emberframe_workbench -ErrorAction SilentlyContinue){throw 'Save and close EmberFrame first; verification never closes user windows.'}
if(-not $OutputDirectory){$OutputDirectory=Join-Path $repoRoot ('output/verification/internship-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
$audit=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $audit){throw 'Choose a NEW evidence directory.'}
New-Item -ItemType Directory -Path $audit | Out-Null
Push-Location $repoRoot
try {
    if(-not $SkipBuild){
        cmake --build build --config Release --target emberframe_workbench emberframe_lab_tests emberframe_gpu_tests --parallel 3
        if($LASTEXITCODE){throw 'Build failed; old binaries are not verified.'}
    }
    $runtime=& ./scripts/configure-vulkan-runtime.ps1
    $exe=Join-Path $repoRoot 'bin/Release/emberframe_workbench.exe'
    $hash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    & ./bin/Release/emberframe_lab_tests.exe all *> (Join-Path $audit 'cpu.log')
    if($LASTEXITCODE){throw 'CPU checks failed.'}
    $cpu=Select-String -LiteralPath (Join-Path $audit 'cpu.log') -Pattern '^RESULT passed=(\d+) failed=0$'
    if(-not $cpu){throw 'CPU summary missing.'}
    & ./bin/Release/emberframe_gpu_tests.exe ./bin/Release/lab_shaders (Join-Path $audit 'gpu-ui.bmp') *> (Join-Path $audit 'gpu.log')
    if($LASTEXITCODE -or (Select-String -LiteralPath (Join-Path $audit 'gpu.log') -Pattern '^FAIL |VUID-|Validation Error' -Quiet)){throw 'Real GPU checks failed.'}
    $gpuCount=@(Select-String -LiteralPath (Join-Path $audit 'gpu.log') -Pattern '^PASS ').Count
    $cases=[Collections.Generic.List[object]]::new()
    function Run-Case([string]$Name,[string[]]$Arguments){
        $log=Join-Path $audit ($Name+'.log');$timer=[Diagnostics.Stopwatch]::StartNew()
        & $exe @Arguments *> $log
        $exit=$LASTEXITCODE;$timer.Stop()
        $validationError=Select-String -LiteralPath $log -Pattern 'VUID-|Validation Error' -Quiet
        if($exit -ne 0 -or $validationError){throw "$Name failed; see $log"}
        $metrics=Get-Content -LiteralPath (Join-Path $repoRoot 'output/last-run.json') -Raw | ConvertFrom-Json
        $cases.Add([pscustomobject]@{name=$Name;exit=$exit;seconds=$timer.Elapsed.TotalSeconds;metrics=$metrics;validationErrors=$false})
        Write-Output "PASS $Name"
    }
    foreach($id in @('materials','interior','many-objects')){
        Run-Case "showcase-$id" @('--gpu-hidden','--no-ui','--showcase',$id,'--width','1280','--height','720','--frames','40','--output',(Join-Path $audit "showcase-$id.bmp"),'--profile-output',(Join-Path $audit "profile-$id"),'--graph-output',(Join-Path $audit "graph-$id"))
    }
    Run-Case 'moving-object' @('--gpu-hidden','--no-ui','--showcase','materials','--object-replay','--taa','--svgf','--gi','2','--samples','8','--width','640','--height','360','--frames','80','--output',(Join-Path $audit 'moving-object.bmp'),'--profile-output',(Join-Path $audit 'profile-motion'),'--graph-output',(Join-Path $audit 'graph-motion'))
    foreach($mode in @('cached','uncached')){
        $arguments=@('--gpu-hidden','--no-ui','--showcase','many-objects','--ao','0','--no-bloom','--width','640','--height','360','--benchmark-frames','48','--benchmark-output',(Join-Path $audit "benchmark-$mode"),'--profile-output',(Join-Path $audit "profile-$mode"))
        if($mode -eq 'uncached'){$arguments+='--uncached-bounds'}
        Run-Case "bounds-$mode" $arguments
        $arguments=@('--gpu-hidden','--no-ui','--showcase','many-objects','--ao','0','--no-bloom','--width','640','--height','360','--frames','40','--output',(Join-Path $audit "bounds-$mode.bmp"))
        if($mode -eq 'uncached'){$arguments+='--uncached-bounds'}
        Run-Case "bounds-image-$mode" $arguments
    }
    $sameImage=(Get-FileHash -LiteralPath (Join-Path $audit 'bounds-cached.pfm')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $audit 'bounds-uncached.pfm')).Hash
    if(-not $sameImage){throw 'Cached/uncached HDR outputs differ.'}
    function Summarize-Profiles([string]$Name){
        $directory=Join-Path $audit "profile-$Name"
        $index=Get-Content -LiteralPath (Join-Path $directory 'index.json') -Raw | ConvertFrom-Json
        $profiles=@($index.profiles | ForEach-Object {Get-Content -LiteralPath (Join-Path $directory $_) -Raw | ConvertFrom-Json})
        if($profiles.Count -ne 48 -or @($profiles | Where-Object {-not $_.completed}).Count){throw 'Completed-frame profile count mismatch.'}
        $values=@($profiles.cpu_ms.geometry | Sort-Object)
        $gpuValues=@($profiles.gpu_ms | Sort-Object)
        [pscustomobject]@{samples=$profiles.Count;geometryMedianMs=$values[[int]($values.Count/2)];gpuMedianMs=$gpuValues[[int]($gpuValues.Count/2)];lastSerial=$profiles[-1].serial;passNames=$profiles[-1].passes.name}
    }
    $cached=Summarize-Profiles 'cached';$uncached=Summarize-Profiles 'uncached'
    $report=[ordered]@{
        schema='emberframe.internship-demo-verification.v1';time=(Get-Date -Format o);executableSha256=$hash
        cpuPassed=[int]$cpu.Matches[0].Groups[1].Value;gpuPassed=$gpuCount;runtime=$runtime
        cases=$cases;boundsImageIdentical=$sameImage;cached=$cached;uncached=$uncached
        geometrySpeedup=($uncached.geometryMedianMs/$cached.geometryMedianMs)
        scope='Real Vulkan single-machine verification; GPU timings include instrumentation. CPU geometry is a subset of command recording. No cross-machine FPS guarantee, no automated OS file-picker or mouse-layout review.'
    }
    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $audit 'summary.json') -Encoding utf8
    $revision=& git -c "safe.directory=$($repoRoot.Replace('\','/'))" -C $repoRoot rev-parse HEAD
    if($LASTEXITCODE){throw 'Cannot record source revision.'}
    $trackedChanges=& git -c "safe.directory=$($repoRoot.Replace('\','/'))" -C $repoRoot status --porcelain --untracked-files=no
    if($LASTEXITCODE){throw 'Cannot record source checkout state.'}
    [ordered]@{schemaVersion=1;gitRevision=$revision;executableSha256=$hash;configuration='Release';architecture='x64';
        buildInvoked=(-not $SkipBuild);trackedCheckoutDirty=[bool]$trackedChanges;createdUtc=[DateTime]::UtcNow.ToString('o');
        verification='real-vulkan-and-cpu';cpuPassed=$report.cpuPassed;gpuPassed=$gpuCount;
        sourceIdentity='Base revision plus uncommitted working-tree changes; not a clean committed release.'} |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $audit 'build-metadata.json') -Encoding utf8
    Write-Output "RESULT CPU=$($report.cpuPassed) GPU=$gpuCount cases=$($cases.Count); evidence=$audit"
}finally {Pop-Location}
