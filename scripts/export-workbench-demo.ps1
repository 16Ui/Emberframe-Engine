param([string]$OutputDirectory,[ValidateRange(5,60)][int]$Fps=15,[ValidateRange(1,60)][int]$Repeat=60,[string]$InputDirectory)
$ErrorActionPreference='Stop'
$engineRoot=Split-Path -Parent $PSScriptRoot
if(-not $OutputDirectory){$OutputDirectory=Join-Path $engineRoot ('output\demo-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
$outputRoot=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $outputRoot){throw 'Choose a new output directory; prior evidence will not be overwritten.'}
New-Item -ItemType Directory -Path $outputRoot | Out-Null
$encoder=Join-Path $engineRoot 'bin\Release\emberframe_demo_video.exe'
if(-not (Test-Path -LiteralPath $encoder)){
    & (Join-Path $PSScriptRoot 'build-windows.ps1') -Config Release -Target emberframe_demo_video
    if($LASTEXITCODE -ne 0){throw 'Video helper build failed.'}
}
$frames=if($InputDirectory){[IO.Path]::GetFullPath($InputDirectory)}else{Join-Path $outputRoot 'frames'}
if(-not $InputDirectory){
    New-Item -ItemType Directory -Path $frames | Out-Null
    $exe=Join-Path $engineRoot 'bin\Release\emberframe_workbench.exe'
    $cases=@(
        @{name='PBR-materials';preset=0;args=@('--energy')},
        @{name='Deferred-clustered-lights';preset=3;args=@('--path','deferred','--culling','2')},
        @{name='MSM-shadows';preset=2;args=@('--shadow','5')},
        @{name='SSGI-GTAO-TAA';preset=1;args=@('--path','deferred','--gi','2','--ao','2','--taa','--svgf')},
        @{name='RSM-indirect';preset=2;args=@('--gi','3')},
        @{name='LPV-propagation';preset=2;args=@('--gi','4')},
        @{name='Voxel-cone-tracing';preset=2;args=@('--gi','5')},
        @{name='Disney-NPR';preset=0;args=@('--shading','2','--outline','--hatching','--filter','3')}
    )
    $index=0;$timeline=@()
    foreach($case in $cases){
        $path=Join-Path $frames ('{0:D3}-{1}.bmp' -f $index,$case.name)
        $arguments=@('--gpu-hidden','--no-ui','--frames','16','--width','960','--height','540','--preset',"$($case.preset)",'--output',$path)+$case.args
        $log=Join-Path $outputRoot ($case.name+'.log')
        & $exe @arguments *> $log
        if($LASTEXITCODE -ne 0){throw "Capture failed: $($case.name)"}
        if(Select-String -LiteralPath $log -Pattern 'Validation Error|VUID-' -Quiet){throw "Validation failed: $($case.name); captures retained."}
        $timeline+=@{name=$case.name;seconds=$index*$Repeat/$Fps;file=$path;arguments=$arguments};$index++
    }
    $timeline | ConvertTo-Json -Depth 5 | Out-File -LiteralPath (Join-Path $outputRoot 'timeline.json') -Encoding utf8
}
& $encoder $frames (Join-Path $outputRoot 'EmberFrame-demo.mp4') "$Fps" "$Repeat"
if($LASTEXITCODE -ne 0){throw 'MP4 encode/decode verification failed; raw captures retained.'}
Write-Output "Demo: $outputRoot. This is a fixed-frame GPU showcase, not real-time FPS evidence."
