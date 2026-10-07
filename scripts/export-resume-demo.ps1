param(
    [string]$OutputDirectory,
    [ValidateRange(8,240)][int]$SequenceFrames=72,
    [ValidateRange(5,60)][int]$Fps=24,
    [ValidateRange(3,1000)][int]$BenchmarkFrames=60,
    [ValidateRange(160,1280)][int]$Width=640,
    [ValidateRange(96,720)][int]$Height=360,
    [switch]$SkipBuild
)
$ErrorActionPreference='Stop'
$demoRoot=Split-Path -Parent $PSScriptRoot
$runtime=& (Join-Path $PSScriptRoot 'configure-vulkan-runtime.ps1')
if($Width%2 -or $Height%2){throw 'Video dimensions must be even.'}
if(-not $OutputDirectory){$OutputDirectory=Join-Path $demoRoot ('output\resume-demo-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
$pack=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $pack){throw 'Use a new directory; previous evidence is preserved.'}
if(Get-Process -Name emberframe_workbench -ErrorAction SilentlyContinue){throw 'Close the workbench before exporting: its runtime log is shared.'}
if(-not $SkipBuild){
    & (Join-Path $PSScriptRoot 'build-windows.ps1') -Config Release -Target emberframe_workbench
    if($LASTEXITCODE -ne 0){throw 'Workbench build failed.'}
    cmake --build (Join-Path $demoRoot 'build') --config Release --target emberframe_lab_tests emberframe_demo_video --parallel
    if($LASTEXITCODE -ne 0){throw 'Demo support build failed.'}
}
New-Item -ItemType Directory -Path $pack | Out-Null
$exe=Join-Path $demoRoot 'bin\Release\emberframe_workbench.exe'
$encoder=Join-Path $demoRoot 'bin\Release\emberframe_demo_video.exe'
$cases=[Collections.Generic.List[object]]::new()
$sha=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
if(-not ('ResumeHdrCheck' -as [type])){
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Globalization;
public static class ResumeHdrCheck {
    static string Line(BinaryReader r) { string s=""; for(int i=0;i<96;i++){char c=(char)r.ReadByte();if(c=='\n')return s.Trim();s+=c;}throw new InvalidDataException("PFM header too long"); }
    static float[] Read(string path,out int w,out int h) {
        using(var r=new BinaryReader(File.OpenRead(path))) {
            if(Line(r)!="PF")throw new InvalidDataException("Not RGB PFM");
            var d=Line(r).Split(new[]{' '},StringSplitOptions.RemoveEmptyEntries);w=int.Parse(d[0]);h=int.Parse(d[1]);
            if(w<1||h<1||(long)w*h>16777216||float.Parse(Line(r),CultureInfo.InvariantCulture)!=-1)throw new InvalidDataException("PFM dimensions/endian/scale unsupported");
            int bytes=checked(w*h*3*4);var data=r.ReadBytes(bytes);if(data.Length!=bytes)throw new InvalidDataException("Truncated PFM");
            var f=new float[w*h*3];Buffer.BlockCopy(data,0,f,0,bytes);return f;
        }
    }
    public static double[] Compare(string a,string b) {
        int aw,ah,bw,bh;var x=Read(a,out aw,out ah);var y=Read(b,out bw,out bh);if(aw!=bw||ah!=bh)throw new InvalidDataException("HDR size mismatch");
        double sum=0,max=0,scaled=0,bad=0;for(int i=0;i<x.Length;i++){if(float.IsNaN(x[i])||float.IsInfinity(x[i])||float.IsNaN(y[i])||float.IsInfinity(y[i]))throw new InvalidDataException("Non-finite HDR");double delta=Math.Abs((double)x[i]-y[i]);sum+=delta;max=Math.Max(max,delta);scaled=Math.Max(scaled,delta/Math.Max(1,Math.Max(Math.Abs(x[i]),Math.Abs(y[i]))));if(delta>.01)bad++;}return new[]{sum/x.Length,max,scaled,bad/x.Length};
    }
}
'@
}

function Invoke-DemoProcess {
    param([string]$Name,[string]$Program,[string[]]$DemoArguments)
    $directory=Join-Path $pack $Name
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    @{program=$Program;arguments=$DemoArguments;workbench_sha256=$sha} | ConvertTo-Json -Depth 5 | Out-File -LiteralPath (Join-Path $directory 'command.json') -Encoding utf8
    # 所有进程串行。只停止本函数创建且超时的进程，不触及用户其他程序。
    $quoted=($DemoArguments | ForEach-Object {'"'+($_ -replace '"','\"')+'"'}) -join ' '
    $process=Start-Process -FilePath $Program -ArgumentList $quoted -WorkingDirectory $demoRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $directory 'stdout.log') -RedirectStandardError (Join-Path $directory 'stderr.log')
    if(-not $process.WaitForExit(120000)){$process.Kill();$process.WaitForExit();throw "Demo timeout: $Name"}
    $process.WaitForExit()
    $code=$process.ExitCode
    $validation=Select-String -LiteralPath (Join-Path $directory 'stdout.log'),(Join-Path $directory 'stderr.log') -Pattern 'Validation Error|VUID-' -Quiet
    $usesVulkan=($Program -eq $exe -and $DemoArguments -contains '--gpu-hidden')
    $layerEnabled=if($usesVulkan){[bool](Select-String -LiteralPath (Join-Path $directory 'stdout.log'),(Join-Path $directory 'stderr.log') -Pattern 'VK_LAYER_KHRONOS_validation: enabled' -Quiet)}else{$null}
    $cases.Add([pscustomobject]@{name=$Name;exit=$code;validation_error=[bool]$validation;validation_layer_enabled=$layerEnabled})
    $cases | ConvertTo-Json -Depth 5 | Out-File -LiteralPath (Join-Path $pack 'cases.json') -Encoding utf8
    if($code -ne 0 -or $validation){throw "Demo failed: $Name; logs retained."}
    if($usesVulkan -and $runtime.validation_configured -and -not $layerEnabled){throw "Configured validation layer did not load: $Name"}
    Write-Output "PASS $Name"
}
function Capture-Static {
    param([string]$Name,[string]$Profile,[string[]]$Extra=@())
    $dir=Join-Path $pack $Name
    Invoke-DemoProcess $Name $exe (@('--resume-demo',$Profile,'--gpu-hidden','--no-ui','--width',"$Width",'--height',"$Height",'--frames','16','--output',(Join-Path $dir 'result.bmp'),'--demo-stats',(Join-Path $dir 'stats.json'))+$Extra)
    $stats=Get-Content -LiteralPath (Join-Path $dir 'stats.json') -Raw | ConvertFrom-Json
    if($stats.actualPath -gt 1){throw "Unexpected CPU fallback: $Name"}
    if(-not(Test-Path -LiteralPath (Join-Path $dir 'result.bmp'))){throw "Missing capture: $Name"}
}
function Capture-Sequence {
    param([string]$Name,[string]$Profile,[string]$Motion,[string[]]$Extra=@())
    $dir=Join-Path $pack $Name
    Invoke-DemoProcess $Name $exe (@('--resume-demo',$Profile,'--gpu-hidden','--no-ui','--width',"$Width",'--height',"$Height",'--camera-replay',$Motion,'--sequence-frames',"$SequenceFrames",'--capture-sequence',(Join-Path $dir 'frames'),'--demo-stats',(Join-Path $dir 'stats.json'))+$Extra)
    $sequence=Get-Content -LiteralPath (Join-Path $dir 'frames\sequence.json') -Raw | ConvertFrom-Json
    $images=@(Get-ChildItem -LiteralPath (Join-Path $dir 'frames') -File -Filter '*.bmp')
    if($sequence.capturedFrames -ne $SequenceFrames -or $images.Count -ne $SequenceFrames){throw "Incomplete sequence: $Name"}
    $serials=@($sequence.frames.serial | Select-Object -Unique)
    if($serials.Count -ne $SequenceFrames -or @($sequence.frames | Where-Object {$_.path -gt 1}).Count){throw "Invalid completed GPU sequence: $Name"}
}
function Compare-Sequence {
    param([string]$Name,[string]$Left,[string]$Right)
    $a=Get-Content -LiteralPath (Join-Path $pack "$Left\frames\sequence.json") -Raw | ConvertFrom-Json
    $b=Get-Content -LiteralPath (Join-Path $pack "$Right\frames\sequence.json") -Raw | ConvertFrom-Json
    for($i=0;$i -lt $SequenceFrames;$i++){for($axis=0;$axis -lt 3;$axis++){if([Math]::Abs($a.frames[$i].camera[$axis]-$b.frames[$i].camera[$axis]) -gt .00001){throw "A/B camera mismatch: $Name frame $i"}}}
    $dir=Join-Path $pack $Name
    Invoke-DemoProcess $Name $encoder @('--compare',(Join-Path $pack "$Left\frames"),(Join-Path $pack "$Right\frames"),(Join-Path $dir 'comparison.mp4'),"$Fps")
}

try {
    Invoke-DemoProcess 'algorithm-checks' (Join-Path $demoRoot 'bin\Release\emberframe_lab_tests.exe') @('all')
    Capture-Static '01-pbr-forward' 'pbr'
    Capture-Static '01-pbr-deferred' 'pbr' @('--path','deferred')
    Capture-Static '01-kc-off' 'kc'
    Capture-Static '01-kc-on' 'kc' @('--energy')
    foreach($mode in 0..2){Capture-Static "02-lights-$mode" 'lights' @('--culling',"$mode")}
    Capture-Static '02-cluster-light-count' 'lights' @('--culling','2','--debug','11')
    $hdrChecks=@()
    foreach($pair in @(@{name='forward-deferred';a='01-pbr-forward';b='01-pbr-deferred'},@{name='all-tiled';a='02-lights-0';b='02-lights-1'},@{name='all-clustered';a='02-lights-0';b='02-lights-2'})){
        $delta=[ResumeHdrCheck]::Compare((Join-Path $pack ($pair.a+'\result.pfm')),(Join-Path $pack ($pair.b+'\result.pfm')))
        $hdrChecks+=@{pair=$pair.name;mean_absolute_error=$delta[0];maximum_error=$delta[1];maximum_scaled_error=$delta[2];channel_fraction_above_001=$delta[3]}
        $hdrChecks | ConvertTo-Json -Depth 4 | Out-File -LiteralPath (Join-Path $pack 'hdr-checks.json') -Encoding utf8
        # FP16 G-buffer 的法线量化在尖锐 HDR 高光处会放大绝对差异：保留峰值，不冒充逐像素相等。
        # 前向/延迟约束平均误差与异常比例；灯表替换则仍严格约束最大误差。
        if($delta[0] -gt .005 -or $delta[3] -gt .01 -or ($pair.name -ne 'forward-deferred' -and $delta[1] -gt .001)){throw ('HDR comparison exceeds fixed-demo tolerance: '+$pair.name)}
    }
    foreach($mode in 0..6){Capture-Static "03-shadow-$mode" 'shadows' @('--shadow',"$mode",'--debug','7')}
    Capture-Static '03-pcss-final' 'shadows' @('--shadow','2')
    foreach($mode in 0..2){Capture-Static "04-gi-$mode" 'temporal' @('--gi',"$mode")}
    Capture-Static '05-sh' 'shadows' @('--environment-diffuse','1')
    Capture-Static '05-prt' 'shadows' @('--environment-diffuse','2')
    Capture-Static '05-geometry' 'geometry'
    Invoke-DemoProcess '05-cpu-path-reference' $exe @('--resume-demo','temporal','--headless','--path','trace','--width','160','--height','96','--samples','8','--frames','4','--svgf','--output',(Join-Path $pack '05-cpu-path-reference\result.bmp'))

    Capture-Sequence '04-temporal-raw' 'temporal' 'pan'
    Capture-Sequence '04-temporal-taa' 'temporal' 'pan' @('--taa')
    Capture-Sequence '04-temporal-svgf' 'temporal' 'pan' @('--svgf')
    Capture-Sequence '03-pcf-motion' 'shadows' 'pan' @('--shadow','1','--debug','7')
    Capture-Sequence '03-csm-motion' 'shadows' 'pan' @('--shadow','6','--debug','7')
    Capture-Sequence '05-lod-off' 'geometry' 'dolly'
    Capture-Sequence '05-lod-on' 'geometry' 'dolly' @('--auto-lod')
    Compare-Sequence 'video-TAA-left-off-right-on' '04-temporal-raw' '04-temporal-taa'
    Compare-Sequence 'video-SVGF-left-off-right-on' '04-temporal-raw' '04-temporal-svgf'
    Compare-Sequence 'video-shadow-left-PCF-right-CSM' '03-pcf-motion' '03-csm-motion'
    Compare-Sequence 'video-LOD-left-off-right-on' '05-lod-off' '05-lod-on'

    # 不读回 HDR、不截图、不回放；预热 30 个有效完成帧后采独立 GPU timestamp。
    $timings=@()
    foreach($path in @('forward','deferred')){foreach($mode in 0..2){
        $name="benchmark-$path-$mode";$dir=Join-Path $pack $name
        Invoke-DemoProcess $name $exe @('--resume-demo','lights','--gpu-hidden','--no-ui','--width',"$Width",'--height',"$Height",'--path',$path,'--culling',"$mode",'--benchmark-frames',"$BenchmarkFrames",'--benchmark-output',$dir)
        $timing=Get-Content -LiteralPath (Join-Path $dir 'gpu-timings.json') -Raw | ConvertFrom-Json
        if($timing.available -and $timing.samples.Count -ne $BenchmarkFrames){throw "Incomplete timing samples: $name"}
        $timings+=@{case=$name;report=$timing}
    }}
    $single=Get-Content -LiteralPath (Join-Path $pack '01-kc-off\stats.json') -Raw | ConvertFrom-Json
    $multiple=Get-Content -LiteralPath (Join-Path $pack '01-kc-on\stats.json') -Raw | ConvertFrom-Json
    if($multiple.centerLinearRgb[0] -le $single.centerLinearRgb[0]){throw 'KC did not increase furnace center energy.'}
    $lights=Get-Content -LiteralPath (Join-Path $pack '02-lights-2\stats.json') -Raw | ConvertFrom-Json
    if($lights.visibleObjects -ge $lights.candidateObjects){throw 'No actual offscreen object culling demonstrated.'}
    $probe=(Get-Content -LiteralPath (Join-Path $pack '05-geometry\stats.json') -Raw | ConvertFrom-Json).bvhProbe
    if(-not $probe.matchesBrute -or $probe.bvhTriangleTests -le 0 -or $probe.bvhTriangleTests -ge $probe.bruteTriangleTests){throw 'BVH probe did not match/reduce triangle tests.'}
    $lod=Get-Content -LiteralPath (Join-Path $pack '05-lod-on\frames\sequence.json') -Raw | ConvertFrom-Json
    $lodMin=($lod.frames.selectedTriangles | Measure-Object -Minimum).Minimum
    $lodMax=($lod.frames.selectedTriangles | Measure-Object -Maximum).Maximum
    if($lodMin -ge $lodMax){throw 'LOD replay did not change actual geometry.'}
    @{schema='emberframe.resume-demo-pack.v1';program_sha256=$sha;runtime=$runtime;width=$Width;height=$Height;sequence_frames=$SequenceFrames;video_fps=$Fps;cases=$cases;kc_single=$single.centerLinearRgb;kc_compensated=$multiple.centerLinearRgb;hdr_checks=$hdrChecks;visible_objects=$lights.visibleObjects;candidate_objects=$lights.candidateObjects;bvh=$probe;lod_min_triangles=$lodMin;lod_max_triangles=$lodMax;timings=$timings;visual_review_required=$true;video_is_not_realtime_fps=$true} | ConvertTo-Json -Depth 12 | Out-File -LiteralPath (Join-Path $pack 'summary.json') -Encoding utf8
    $lines=@('# 简历演示包','',"程序 SHA256：$sha",'',"KC 白炉中心线性 RGB：$($single.centerLinearRgb -join ', ') → $($multiple.centerLinearRgb -join ', ')","GPU 可见对象：$($lights.visibleObjects) / $($lights.candidateObjects)","QEM 运动回放实际三角形：$lodMax → $lodMin", "SAH BVH 求交：$($probe.bruteTriangleTests) → $($probe.bvhTriangleTests) 次三角形测试；命中一致=$($probe.matchesBrute)",'','## 连续帧左右对照','','左关闭 / 右开启：TAA、SVGF、LOD；阴影为左 PCF / 右稳定化 CSM。','每个源帧来自真实 GPU，清单保存相机与完成 serial；视频时轴不代表实时帧率。','TAA/SVGF 画质需观看确认，未用不同相机帧差伪造质量提升指标。','','## 独立性能报告','','六组 benchmark-* 中 gpu-timings.json 来自无截图的独立运行。','小场景中分块/聚簇不保证更快；没有 timestamp 的设备报告 unavailable。','','## 现场操作','','双击项目根目录 Start-Resume-Demo.cmd，使用顶部“演示”菜单。','完整操作与讲解见 docs/RESUME_DEMO.md。')
    $lines | Out-File -LiteralPath (Join-Path $pack 'README.md') -Encoding utf8
    @('',"验证层 manifest：$($runtime.layer_manifest)",'实际加载状态见每组 validation_layer_enabled 与日志；未加载不冒充检查通过。','本包计时保留运行时验证层状态；不同验证配置的数值不能直接比较。') | Out-File -LiteralPath (Join-Path $pack 'README.md') -Encoding utf8 -Append
    Write-Output "Resume demo pack complete: $pack"
}catch{
    @{error=$_.Exception.Message;cases=$cases;program_sha256=$sha} | ConvertTo-Json -Depth 5 | Out-File -LiteralPath (Join-Path $pack 'failure.json') -Encoding utf8
    throw
}
