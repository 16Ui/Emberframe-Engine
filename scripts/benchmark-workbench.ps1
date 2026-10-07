#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$EngineRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$OutputDirectory,
    [ValidateRange(3, 1000)][int]$Frames = 180,
    [ValidateRange(1, 10000)][int]$WarmupFrames = 30,
    [ValidateRange(1, 20)][int]$Repeats = 3,
    [ValidateRange(8, 4096)][int]$Width = 640,
    [ValidateRange(8, 4096)][int]$Height = 360,
    [ValidateRange(0, 3)][int]$Preset = 3,
    [ValidateRange(10, 600)][int]$TimeoutSeconds = 120,
    [switch]$Cpu,
    [switch]$SkipGpu,
    [switch]$Rebuild
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$enginePath = [IO.Path]::GetFullPath($EngineRoot)
$executable = Join-Path $enginePath 'bin\Release\emberframe_workbench.exe'
if ($Frames + $WarmupFrames + 4 -gt 100000) { throw 'Frames + WarmupFrames + 4 must be <= 100000.' }
if ($SkipGpu -and -not $Cpu) { throw 'Select -Cpu when using -SkipGpu.' }
if ($Rebuild) {
    & (Join-Path $enginePath 'scripts\build-windows.ps1') -Config Release -Target emberframe_workbench
    if ($LASTEXITCODE -ne 0) { throw 'Workbench build failed.' }
}
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Build emberframe_workbench first: $executable" }
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $enginePath ('output\benchmark-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$reportRoot = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $reportRoot) { throw 'Choose a new output directory to preserve prior benchmark evidence.' }
New-Item -ItemType Directory -Path $reportRoot | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($false)
$records = [Collections.Generic.List[object]]::new()
$failures = [Collections.Generic.List[object]]::new()
$currentCase = ''

function Save-Json {
    param([object]$Value, [string]$Path)
    [IO.File]::WriteAllText($Path, (ConvertTo-Json -InputObject $Value -Depth 12), $utf8)
}
function Get-TimingSummary {
    param([double[]]$Milliseconds)
    if ($Milliseconds.Count -eq 0) { return $null }
    foreach ($v in $Milliseconds) {
        if ([double]::IsNaN($v) -or [double]::IsInfinity($v) -or $v -lt 0) { throw 'Invalid timing sample.' }
    }
    $sorted = @($Milliseconds | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    $median = $sorted[$middle]
    if ($sorted.Count % 2 -eq 0) { $median = 0.5 * $sorted[$middle - 1] + 0.5 * $sorted[$middle] }
    $rank = [int][Math]::Ceiling(0.95 * $sorted.Count) - 1
    return [pscustomobject]@{ sample_count = $sorted.Count; minimum_ms = $sorted[0]; median_ms = $median; p95_ms = $sorted[$rank]; maximum_ms = $sorted[-1] }
}
function Quote-NativeArgument {
    param([string]$Value)
    # Start-Process 会拼接 ArgumentList；这里按 Windows argv 规则保护空格/引号/尾反斜杠。
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $quoted = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $quoted = [regex]::Replace($quoted, '(\\+)$', '$1$1')
    return '"' + $quoted + '"'
}
function Invoke-WorkbenchCase {
    param([string]$Name, [string[]]$Arguments, [string]$Directory)
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    Save-Json -Value ([ordered]@{ executable = $executable; arguments = @($Arguments) }) -Path (Join-Path $Directory 'command.json')
    $started = [DateTime]::UtcNow
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $commandLine = (@($Arguments | ForEach-Object { Quote-NativeArgument $_ }) -join ' ')
    $process = Start-Process -FilePath $executable -ArgumentList $commandLine -WorkingDirectory $enginePath -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $Directory 'stdout.log') -RedirectStandardError (Join-Path $Directory 'stderr.log')
    try {
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill(); $process.WaitForExit()
            throw "Case $Name exceeded $TimeoutSeconds seconds (only the process launched by this script was stopped)."
        }
        $process.WaitForExit(); $exitCode = $process.ExitCode
    } finally { $timer.Stop(); $process.Dispose() }
    # 旧入口的日志/last-run 使用固定 output；逐个进程运行并立即复制，禁止同时开另一工作台。
    foreach ($file in @('workbench.log', 'last-run.json')) {
        $source = Join-Path $enginePath ('output\' + $file)
        if ((Test-Path -LiteralPath $source -PathType Leaf) -and (Get-Item -LiteralPath $source).LastWriteTimeUtc -ge $started) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $Directory $file)
        }
    }
    $logs = @(Get-ChildItem -LiteralPath $Directory -Filter '*.log' | Select-Object -ExpandProperty FullName)
    if ($logs.Count -and (Select-String -LiteralPath $logs -Pattern 'Validation Error|VUID-' -Quiet)) { throw "Vulkan validation failure: $Name" }
    if ($exitCode -ne 0) { throw "Case $Name exited with code $exitCode. See $Directory" }
    return [pscustomobject]@{ wall_ms = $timer.Elapsed.TotalMilliseconds; started_utc = $started.ToString('o'); exit_code = $exitCode }
}

try {
    # 只探测能力；旧 CLI 用已存在的 --frames/--path/--culling。
    $helpCase = Invoke-WorkbenchCase -Name 'help' -Arguments @('--help') -Directory (Join-Path $reportRoot 'help')
    $helpText = [IO.File]::ReadAllText((Join-Path $reportRoot 'help\stdout.log'))
    $hasCpuBenchmark = $helpText -match '(?<![\w-])--benchmark(?![\w-])'
    $hasFrameSamples = $helpText -match '(?<![\w-])--benchmark-frames(?![\w-])'
    $hasReportOutput = $helpText -match '(?<![\w-])--benchmark-output(?![\w-])'
    $hasHiddenGpu = $helpText -match '(?<![\w-])--gpu-hidden(?![\w-])'
    if ($hasFrameSamples -and $WarmupFrames -ne 30) { throw '--benchmark-frames uses a fixed 30-frame warmup; pass -WarmupFrames 30.' }
    $hardware = [ordered]@{ cpu = 'unavailable'; video_controllers = @(); power_scheme = 'unavailable' }
    try {
        $hardware.cpu = @((Get-CimInstance Win32_Processor -ErrorAction Stop).Name)
        $hardware.video_controllers = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | Select-Object Name, DriverVersion, DriverDate)
    } catch { $hardware.hardware_query_error = $_.Exception.Message }
    try { $hardware.power_scheme = ((& powercfg.exe /getactivescheme 2>&1) -join ' ') } catch {}
    Save-Json -Value ([ordered]@{
        schema = 'emberframe.benchmark_environment.v1'; utc = [DateTime]::UtcNow.ToString('o'); executable = $executable
        executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
        os = [Environment]::OSVersion.VersionString; logical_processors = [Environment]::ProcessorCount
        validation_layer_path = $env:VK_LAYER_PATH; synchronization_validation = $env:VK_LAYER_VALIDATE_SYNC
        hardware = $hardware
        power_shell = $PSVersionTable.PSVersion.ToString(); frames = $Frames; warmup_frames = $WarmupFrames; repeats = $Repeats
        width = $Width; height = $Height; preset = $Preset; supports_cpu_benchmark = $hasCpuBenchmark
        supports_frame_samples = $hasFrameSamples; supports_report_output = $hasReportOutput; supports_hidden_gpu = $hasHiddenGpu
        note = 'GPU 型号见每组 gpu-timings.json，CIM 列出的驱动可能包含集显；Windows 电源方案不等于实时功耗/频率。使用 Release；关闭其他工作台；各 case 不截屏、不 readback、不热重载。验证层会带来额外开销。'
    }) -Path (Join-Path $reportRoot 'environment.json')
    if ($Cpu) {
        $currentCase = 'cpu-system'
        if (-not $hasCpuBenchmark) { throw 'This executable has no --benchmark CLI. Integrate run_system_benchmark and rebuild before using -Cpu.' }
        if (-not $hasReportOutput) { throw 'CPU benchmark needs --benchmark-output DIR.' }
        $cpuDirectory = Join-Path $reportRoot $currentCase
        $run = Invoke-WorkbenchCase -Name $currentCase -Arguments @('--benchmark', '--benchmark-output', $cpuDirectory) -Directory $cpuDirectory
        $cpuReport = Join-Path $cpuDirectory 'system-benchmark.json'
        if (-not (Test-Path -LiteralPath $cpuReport -PathType Leaf)) { throw '--benchmark did not save system-benchmark.json under --benchmark-output.' }
        $cpuData = Get-Content -Raw -LiteralPath $cpuReport | ConvertFrom-Json
        if ($cpuData.schema -ne 'emberframe.system_benchmark.v1' -or -not $cpuData.all_outputs_match) { throw 'CPU benchmark output validation failed.' }
        $records.Add([pscustomobject]@{ name = $currentCase; mode = 'cpu_system'; exit_code = $run.exit_code; wall_ms = $run.wall_ms; report = $cpuReport })
    }
    if (-not $SkipGpu) {
        foreach ($path in @('forward', 'deferred')) {
            foreach ($culling in 0..2) {
                for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
                    $currentCase = "gpu-$path-culling$culling-run$repeat"
                    $caseDirectory = Join-Path $reportRoot $currentCase
                    # 不能使用 --headless：现有 headless 强制 CPU 参考路径。
                    $arguments = @('--path', $path, '--preset', "$Preset", '--width', "$Width", '--height', "$Height", '--culling', "$culling")
                    if ($hasHiddenGpu) { $arguments += @('--gpu-hidden') }
                    if ($hasFrameSamples) {
                        if (-not $hasReportOutput) { throw '--benchmark-frames needs --benchmark-output DIR.' }
                        $arguments += @('--benchmark-frames', "$Frames", '--benchmark-output', $caseDirectory)
                    } else { $arguments += @('--frames', "$($WarmupFrames + $Frames + 4)") }
                    Write-Host "Running $currentCase"
                    $run = Invoke-WorkbenchCase -Name $currentCase -Arguments $arguments -Directory $caseDirectory
                    $sampleCount = 0; $median = $null; $p95 = $null; $lastOnly = $null; $gpuName = ''; $sampleStatus = 'last_timestamp_only'
                    $samplesPath = Join-Path $caseDirectory 'gpu-timings.json'
                    if ($hasFrameSamples) {
                        if (-not (Test-Path -LiteralPath $samplesPath -PathType Leaf)) { throw '--benchmark-frames must write gpu-timings.json under --benchmark-output.' }
                        $data = Get-Content -Raw -LiteralPath $samplesPath | ConvertFrom-Json
                        if ($data.schema -ne 'emberframe.gpu_timings.v1' -or $data.render_path -ne $path -or $data.warmup_frames -ne $WarmupFrames) { throw 'GPU sample schema/path/warmup does not match this case.' }
                        $gpuName = $data.gpu
                        $serials = [Collections.Generic.HashSet[UInt64]]::new()
                        $timings = [Collections.Generic.List[double]]::new()
                        foreach ($sample in $data.samples) {
                            if ([UInt64]$sample.frame_serial -eq 0 -or -not $serials.Add([UInt64]$sample.frame_serial)) { throw 'Repeated/unidentified timestamp samples cannot be a frame distribution.' }
                            $timings.Add([double]$sample.gpu_ms)
                        }
                        $summary = Get-TimingSummary -Milliseconds $timings.ToArray()
                        if ($data.available) {
                            if ($timings.Count -ne $Frames) { throw 'GPU sample count differs from --benchmark-frames; truncated benchmark rejected.' }
                            $sampleCount = $summary.sample_count; $median = $summary.median_ms; $p95 = $summary.p95_ms; $sampleStatus = 'completed_frame_samples'
                        } else {
                            if ($timings.Count -ne 0) { throw 'Unavailable GPU report contains samples.' }
                            $sampleStatus = 'timestamps_unavailable'
                        }
                    } else {
                        $lastRun = Join-Path $caseDirectory 'last-run.json'
                        if (-not (Test-Path -LiteralPath $lastRun -PathType Leaf)) { throw 'No fresh last-run.json was produced; do not use stale metrics.' }
                        $lastData = Get-Content -Raw -LiteralPath $lastRun | ConvertFrom-Json
                        $gpuName = $lastData.gpu
                        if ([double]$lastData.gpuMilliseconds -ge 0) { $lastOnly = [double]$lastData.gpuMilliseconds }
                    }
                    $records.Add([pscustomobject]@{
                        name = $currentCase; mode = 'gpu'; path = $path; culling = $culling; repeat = $repeat
                        exit_code = $run.exit_code; wall_ms = $run.wall_ms; gpu = $gpuName; sample_status = $sampleStatus
                        gpu_sample_count = $sampleCount; gpu_median_ms = $median; gpu_p95_ms = $p95; gpu_ms_last_only = $lastOnly
                        artifacts = $caseDirectory
                    })
                }
            }
        }
    }
} catch {
    $failures.Add([pscustomobject]@{ category = 'benchmark'; name = $currentCase; severity = 'error'; message = $_.Exception.Message; artifact = (Join-Path $reportRoot $currentCase); frame_serial = 0 })
    throw
} finally {
    Save-Json -Value ([ordered]@{ schema = 'emberframe.benchmark_runs.v1'; runs = @($records.ToArray()) }) -Path (Join-Path $reportRoot 'runs.json')
    Save-Json -Value ([ordered]@{ schema = 'emberframe.diagnostics.v1'; session = 'benchmark-workbench'; records = @($failures.ToArray()) }) -Path (Join-Path $reportRoot 'diagnostics.json')
    # 首行可能是 CPU 记录，不能让 Export-Csv 只选其字段而丢掉后续 GPU 分布。
    if ($records.Count) {
        $records.ToArray() | Select-Object name,mode,path,culling,repeat,exit_code,wall_ms,gpu,sample_status,gpu_sample_count,gpu_median_ms,gpu_p95_ms,gpu_ms_last_only,report,artifacts |
            Export-Csv -LiteralPath (Join-Path $reportRoot 'runs.csv') -NoTypeInformation -Encoding UTF8
    }
    $markdown = [Collections.Generic.List[string]]::new()
    $markdown.Add('# EmberFrame 工作台性能记录')
    $markdown.Add('')
    $markdown.Add('GPU 区间包含场景、后处理与 UI；CPU 启动/上传/窗口/退出成本只计入 wall_ms。每次进程运行分别报告，不混合不同路径与 culling。')
    $markdown.Add('旧 CLI 只提供最后一次 timestamp：样本数记 0，中位数/p95 留空。跨进程的最后一个 timestamp 不能冒充逐帧分布。')
    $markdown.Add('')
    $markdown.Add('| case | GPU 样本数 | 中位数 ms | p95 ms | 仅末帧 ms | 进程总耗时 ms |')
    $markdown.Add('|---|---:|---:|---:|---:|---:|')
    foreach ($record in $records) {
        if ($record.mode -eq 'gpu') { $markdown.Add("| $($record.name) | $($record.gpu_sample_count) | $($record.gpu_median_ms) | $($record.gpu_p95_ms) | $($record.gpu_ms_last_only) | $($record.wall_ms) |") }
    }
    $markdown.Add('')
    $markdown.Add('逐帧 API 必须在 FrameSlot fence 完成、query 成功后按提交 serial 只采一次，并在资源上传完成后预热；不能反复采集没有更新的 stats.gpu_ms。')
    $markdown.Add('样本 JSON 与命令、程序 SHA256、stdout/stderr、fresh last-run/workbench.log 均保留。若无 timestamp 支持，记录 unavailable，不填 0 ms。')
    [IO.File]::WriteAllText((Join-Path $reportRoot 'benchmark.md'), ($markdown -join "`n"), $utf8)
    Write-Host "Benchmark evidence: $reportRoot"
}
