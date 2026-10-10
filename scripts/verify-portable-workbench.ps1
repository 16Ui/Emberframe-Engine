param(
    [Parameter(Mandatory)][string]$Archive,
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$archivePath = (Get-Item -LiteralPath $Archive).FullName
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot ('output/verification/portable-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$audit = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $audit) { throw 'Choose a NEW evidence directory.' }
New-Item -ItemType Directory -Path $audit | Out-Null
# 保留每次独立的解压目录，不移动源码，也不删除用户的文件或旧验证结果。
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('EmberFrame 便携验收 ' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
$unrelatedCwd = Join-Path $temporaryRoot '不同工作目录'
$emptyLayers = Join-Path $temporaryRoot 'empty-explicit-layers'
New-Item -ItemType Directory -Path $unrelatedCwd,$emptyLayers | Out-Null
Expand-Archive -LiteralPath $archivePath -DestinationPath $temporaryRoot
$package = Join-Path $temporaryRoot 'EmberFrame-Windows-x64'
$manifest = Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw | ConvertFrom-Json
$requirements = Get-Content -LiteralPath (Join-Path $package 'requirements.json') -Raw | ConvertFrom-Json
if ($requirements.sdkRequired -or $requirements.validationLayerRequired) { throw 'Unexpected SDK-dependent package.' }
$packagePrefix = [IO.Path]::GetFullPath($package) + [IO.Path]::DirectorySeparatorChar
foreach ($entry in $manifest.files) {
    $file = [IO.Path]::GetFullPath((Join-Path $package $entry.path))
    if (-not $file.StartsWith($packagePrefix,[StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest path escapes package.' }
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ine $entry.sha256) { throw "Payload hash mismatch: $($entry.path)" }
    if ((Get-Item -LiteralPath $file).Length -ne $entry.bytes) { throw "Payload size mismatch: $($entry.path)" }
}
$exe = Join-Path $package 'emberframe_workbench.exe'
$exeHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
if ($exeHash -ine $manifest.executableSha256) { throw 'Executable identity mismatch.' }
foreach ($forbidden in @('vulkan-1.dll','VkLayer_khronos_validation.dll','glslangValidator.exe')) {
    if (Get-ChildItem -LiteralPath $package -Recurse -File -Filter $forbidden) { throw "SDK/loader payload must not be redistributed: $forbidden" }
}
# 只改当前验证进程的环境，不卸载 SDK、不修改注册表，不改变系统/用户环境。
$environmentNames = @('PATH','VULKAN_SDK','VK_LAYER_PATH','VK_ADD_LAYER_PATH','VK_INSTANCE_LAYERS',
    'VK_ICD_FILENAMES','VK_DRIVER_FILES','VK_ADD_DRIVER_FILES','VK_LOADER_LAYERS_ENABLE','VK_LOADER_LAYERS_DISABLE','VK_LOADER_DEBUG')
$savedEnvironment = @{}
foreach ($name in $environmentNames) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
$cases = [Collections.Generic.List[object]]::new()
try {
    # PowerShell 调用 .NET 时可能把 $null 转为空字符串；新 .NET 会保留空值变量。
    # 空 VK_DRIVER_FILES 会覆盖系统驱动发现，必须真正删除变量而不是设空值。
    foreach ($name in $environmentNames | Where-Object { $_ -ne 'PATH' }) { Remove-Item -LiteralPath ('Env:'+$name) -ErrorAction SilentlyContinue }
    $filteredPath = @($savedEnvironment['PATH'].Split(';') | Where-Object { $_ -and $_ -notmatch '(?i)VulkanSDK|[\\/]sdk[\\/]' }) -join ';'
    [Environment]::SetEnvironmentVariable('PATH',$filteredPath,'Process')
    [Environment]::SetEnvironmentVariable('VK_LAYER_PATH',$emptyLayers,'Process')
    [Environment]::SetEnvironmentVariable('VK_LOADER_DEBUG','error,warn,info','Process')
    foreach ($scene in @('materials','interior','many-objects')) {
        $stdout = Join-Path $audit ($scene+'.stdout.log')
        $stderr = Join-Path $audit ($scene+'.stderr.log')
        $image = Join-Path $audit ($scene+'.bmp')
        $arguments = @('--gpu-hidden','--no-ui','--showcase',$scene,'--width','320','--height','180',
            '--frames','20','--output',('"'+$image+'"'))
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $unrelatedCwd -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        if (-not $process.WaitForExit(90000)) {
            # 只终止此次验证自己启动的子进程；绝不寻找或关闭用户的编辑器窗口。
            $process.Kill(); $process.WaitForExit()
            throw "Portable $scene timed out; evidence and unpacked package retained."
        }
        $process.WaitForExit(); $timer.Stop()
        $text = (Get-Content -LiteralPath $stdout -Raw) + (Get-Content -LiteralPath $stderr -Raw)
        if ($process.ExitCode -ne 0) { throw "Portable $scene failed with exit $($process.ExitCode)." }
        if ($text -match 'VUID-|Validation Error') { throw "Portable $scene reported a Vulkan error." }
        if ($text -notmatch 'VK_LAYER_KHRONOS_validation: unavailable') { throw 'This check must actually run without the validation layer.' }
        if ($text -notmatch '\[Vulkan\] Selected device: ([^\r\n]+)') { throw 'Real GPU device record missing.' }
        $deviceName = $Matches[1]
        if ($text -notmatch '\[Workbench\] presented=(\d+)') { throw 'Presented frame record missing.' }
        $presented = [int]$Matches[1]
        if ($presented -lt 20 -or -not (Test-Path -LiteralPath $image)) { throw 'Real GPU capture missing.' }
        $cases.Add([pscustomobject]@{scene=$scene;exit=$process.ExitCode;seconds=$timer.Elapsed.TotalSeconds;
            gpu=$deviceName;presentedFrames=$presented;validationLayer='unavailable';image=($scene+'.bmp');
            imageSha256=(Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash})
        Write-Output "PASS relocated portable $scene (no SDK paths, no validation layer)"
    }
    # 便携模式的普通日志/缓存应在 SDL 用户目录，不能偷偷写回包内。
    $expectedPaths = @('manifest.json') + @($manifest.files.path)
    $unexpected = @(Get-ChildItem -LiteralPath $package -Recurse -File | ForEach-Object {
        $_.FullName.Substring($packagePrefix.Length).Replace('\','/')
    } | Where-Object { $_ -notin $expectedPaths })
    if ($unexpected.Count) { throw ('Unexpected files written inside the package: ' + ($unexpected -join ', ')) }
    $report = [ordered]@{
        schema='emberframe.portable-runtime-verification.v1';time=(Get-Date -Format o)
        archiveSha256=(Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
        executableSha256=$exeHash;payloadHashesVerified=$manifest.files.Count
        runtimeValidation='same-machine-relocated-no-sdk-paths-no-validation-layer';cases=$cases
        packageFileListUnchanged=$true;pathContainsSpacesAndChinese=$true;cwdUnrelatedToPackage=$true
        sdkInstalledOnHost=$true;sourceCheckoutRetained=$true;readOnlyAclTested=$false;crossMachineTested=$false
        scope='SDK remains installed, but no SDK PATH or explicit validation layer is available to these child runs. Original source checkout was not renamed/deleted. Not a clean-machine or read-only ACL acceptance.'
    }
    $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $audit 'summary.json') -Encoding utf8
    Write-Output "Portable evidence: $audit"
    Write-Output "Retained relocated package: $package"
} finally {
    foreach ($name in $environmentNames) {
        if ($null -eq $savedEnvironment[$name]) { Remove-Item -LiteralPath ('Env:'+$name) -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name,$savedEnvironment[$name],'Process') }
    }
}
