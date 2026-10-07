param([string]$OutputDirectory,[ValidateRange(60,600)][int]$ModelTimeoutSeconds=180)
$ErrorActionPreference='Stop'
$engineRoot=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $engineRoot 'bin\Release\emberframe_workbench.exe'
if(Get-Process emberframe_workbench -ErrorAction SilentlyContinue){throw 'Close EmberFrame before serial asset verification.'}
$out=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $engineRoot ('output\model-library-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
if(Test-Path -LiteralPath $out){throw 'Choose a new evidence directory; existing evidence is preserved.'}
New-Item -ItemType Directory -Path $out | Out-Null
$runtime=& (Join-Path $PSScriptRoot 'configure-vulkan-runtime.ps1')
if(-not ('EmberFrameBmpCoverage' -as [type])){
    # 样例捕获关闭后处理，天空仅沿纵向渐变。与同一行的两侧背景比较，
    # 可识别“有 Draw 但模型只有几像素”的聚焦错误，不把背景图当演示成功。
    Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class EmberFrameBmpCoverage {
    public static double Measure(string path) {
        byte[] data=File.ReadAllBytes(path);
        if(data.Length<54||data[0]!=66||data[1]!=77)throw new InvalidDataException("Not a BMP");
        int start=BitConverter.ToInt32(data,10),width=BitConverter.ToInt32(data,18),height=Math.Abs(BitConverter.ToInt32(data,22));
        int bytes=BitConverter.ToInt16(data,28)/8;
        if(width<=0||height<=0||(bytes!=3&&bytes!=4))throw new InvalidDataException("Unexpected BMP layout");
        int stride=(width*bytes+3)&~3;long foreground=0;
        if(start+(long)stride*height>data.LongLength)throw new InvalidDataException("Truncated BMP");
        for(int y=0;y<height;++y){int row=start+y*stride,right=row+(width-1)*bytes;
            for(int x=1;x<width-1;++x){int p=row+x*bytes;
                int a=Math.Abs(data[p]-data[row])+Math.Abs(data[p+1]-data[row+1])+Math.Abs(data[p+2]-data[row+2]);
                int b=Math.Abs(data[p]-data[right])+Math.Abs(data[p+1]-data[right+1])+Math.Abs(data[p+2]-data[right+2]);
                if(Math.Min(a,b)>9)++foreground;
            }
        }
        return (double)foreground/(width*(long)height);
    }
}
'@
}
$models=[Collections.Generic.List[object]]::new()
Get-ChildItem -LiteralPath (Join-Path $engineRoot 'assets\model_library\04_ready_static') -Recurse -Filter '*.glb' -File | ForEach-Object {
    $models.Add([pscustomobject]@{name=$_.BaseName;path=$_.FullName})
}
foreach($entry in @(
    @('cartoon-female','MiniCharacters/Models/OBJ format/character-female-a.obj'),
    @('cartoon-male','MiniCharacters/Models/OBJ format/character-male-a.obj'),
    @('cartoon-cat','CubePets/Models/OBJ format/animal-cat.obj'),
    @('cartoon-dog','CubePets/Models/OBJ format/animal-dog.obj'),
    @('cartoon-tree','NatureKit/Models/OBJ format/tree_oak.obj'),
    @('cartoon-bridge','NatureKit/Models/OBJ format/bridge_stone.obj'),
    @('cartoon-building','CityKitSuburban/Models/OBJ format/building-type-a.obj'),
    @('cartoon-apple','FoodKit/Models/OBJ format/apple.obj')
)){$models.Add([pscustomobject]@{name=$entry[0];path=(Join-Path $engineRoot ('assets/model_library/02_cartoon/'+$entry[1]))})}
Get-ChildItem -LiteralPath (Join-Path $engineRoot 'assets\import_samples\models') -Recurse -Filter '*.glb' -File | ForEach-Object {
    $models.Add([pscustomobject]@{name=$_.BaseName;path=$_.FullName})
}
$records=[Collections.Generic.List[object]]::new()
Push-Location $engineRoot
try {
    foreach($model in $models){foreach($route in @('forward','deferred')){
        $name=$model.name+'-'+$route
        $directory=Join-Path $out $name
        New-Item -ItemType Directory -Path $directory | Out-Null
        $arguments=@('--load',$model.path,'--path',$route,'--gpu-hidden','--no-ui','--frames','12','--width','480','--height','320','--no-bloom','--output',(Join-Path $directory 'result.bmp'),'--demo-stats',(Join-Path $directory 'stats.json'))
        @{program=$exe;arguments=$arguments} | ConvertTo-Json -Depth 5 | Out-File -LiteralPath (Join-Path $directory 'command.json') -Encoding utf8
        $quoted=($arguments | ForEach-Object {'"'+($_ -replace '"','\"')+'"'}) -join ' '
        $timer=[Diagnostics.Stopwatch]::StartNew()
        # 更新 Shader 后，驱动首次编译管线可能超过 60 秒。
        # 超时预算包含冷启动编译，不降低任何画面/验证断言，也不代表实时帧率。
        # 只结束本次创建的超时子进程；不关闭用户窗口。
        $process=Start-Process -FilePath $exe -ArgumentList $quoted -WorkingDirectory $engineRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $directory 'stdout.log') -RedirectStandardError (Join-Path $directory 'stderr.log')
        $timedOut=-not $process.WaitForExit($ModelTimeoutSeconds*1000)
        if($timedOut){$process.Kill();$process.WaitForExit()}else{$process.WaitForExit()}
        $timer.Stop()
        $code=if($timedOut){124}else{$process.ExitCode}
        $validation=[bool](Select-String -LiteralPath (Join-Path $directory 'stdout.log'),(Join-Path $directory 'stderr.log') -Pattern 'Validation Error|VUID-' -Quiet)
        $layer=[bool](Select-String -LiteralPath (Join-Path $directory 'stdout.log'),(Join-Path $directory 'stderr.log') -Pattern 'VK_LAYER_KHRONOS_validation: enabled' -Quiet)
        $statsPath=Join-Path $directory 'stats.json'
        $stats=if(Test-Path -LiteralPath $statsPath){Get-Content -LiteralPath $statsPath -Raw | ConvertFrom-Json}else{$null}
        $requested=if($route -eq 'forward'){0}else{1}
        $capture=Join-Path $directory 'result.bmp'
        $coverage=if(Test-Path -LiteralPath $capture){[EmberFrameBmpCoverage]::Measure($capture)}else{0.0}
        # 导入成功与源网格非空不等于真正画出了模型；必须检查已退休的 GPU 绘制结果。
        $passed=$code -eq 0 -and -not $validation -and $null -ne $stats -and $stats.actualPath -eq $requested -and $stats.selectedTriangles -gt 0 -and $stats.candidateObjects -gt 0 -and $stats.visibleObjects -gt 0 -and $coverage -gt .01 -and (-not $runtime.validation_configured -or $layer)
        $records.Add([pscustomobject]@{name=$name;source=$model.path;route=$route;passed=$passed;exit=$code;timedOut=$timedOut;timeoutSeconds=$ModelTimeoutSeconds;seconds=$timer.Elapsed.TotalSeconds;foregroundFraction=$coverage;validationError=$validation;validationLayerEnabled=$layer;stats=$stats})
        $records | ConvertTo-Json -Depth 10 | Out-File -LiteralPath (Join-Path $out 'results.json') -Encoding utf8
        Write-Output ((@('FAIL','PASS')[[int]$passed])+' model '+$name+' ('+[Math]::Round($timer.Elapsed.TotalSeconds,2)+'s)')
    }}
    $runtime | ConvertTo-Json -Depth 5 | Out-File -LiteralPath (Join-Path $out 'runtime.json') -Encoding utf8
    $failed=@($records | Where-Object {-not $_.passed})
    Write-Output ('Models: '+($records.Count-$failed.Count)+'/'+$records.Count+' passed; evidence: '+$out)
    if($failed.Count){exit 1}
    exit 0
}finally{Pop-Location}
