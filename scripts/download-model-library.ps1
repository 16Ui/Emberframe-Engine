#requires -Version 7.0
[CmdletBinding()]
param([ValidateSet('All','Realistic','Cartoon','Anime')][string]$Group='All')
$ErrorActionPreference='Stop'
$modelRepo=Split-Path -Parent $PSScriptRoot
$modelRoot=[IO.Path]::GetFullPath((Join-Path $modelRepo 'assets/model_library'))
$revision='edc7c9e67c639d230715049ee31f9a96a6babbbe'
New-Item -ItemType Directory -Path $modelRoot -Force | Out-Null

function Get-LibraryPath([string]$Relative) {
    $result=[IO.Path]::GetFullPath((Join-Path $modelRoot $Relative))
    if(-not $result.StartsWith($modelRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){
        throw "Asset path escapes the model library: $Relative"
    }
    return $result
}
function Get-LibraryFile([string]$Url,[string]$Relative,[long]$ExpectedBytes=0) {
    $target=Get-LibraryPath $Relative
    if(Test-Path -LiteralPath $target){
        if($ExpectedBytes -gt 0 -and (Get-Item -LiteralPath $target).Length -ne $ExpectedBytes){
            throw "An existing file differs in size; it was kept, not overwritten: $Relative"
        }
        return
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    $partial="$target.partial"
    Invoke-WebRequest -Uri $Url -OutFile $partial -TimeoutSec 120 -MaximumRetryCount 2 -RetryIntervalSec 2 -Headers @{'User-Agent'='Emberframe-Personal-Asset-Downloader/1.0'}
    $length=(Get-Item -LiteralPath $partial).Length
    if($length -le 0 -or ($ExpectedBytes -gt 0 -and $length -ne $ExpectedBytes)){
        throw "Download was incomplete; the .partial file is not a usable asset: $Relative"
    }
    Move-Item -LiteralPath $partial -Destination $target
}
function Expand-LibraryArchive([string]$RelativeArchive,[string]$RelativeDirectory) {
    $archive=Get-LibraryPath $RelativeArchive
    $destination=Get-LibraryPath $RelativeDirectory
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip=[IO.Compression.ZipFile]::OpenRead($archive)
    $expanded=0L
    try {
        foreach($entry in $zip.Entries){
            # 只展开模型、依赖、许可和说明；不展开/运行下载包附带的可执行程序或脚本。
            if([IO.Path]::GetExtension($entry.Name).ToLowerInvariant() -notin @('.obj','.mtl','.glb','.gltf','.bin','.fbx','.blend','.vrm','.png','.jpg','.jpeg','.tga','.bmp','.webp','.ktx2','.txt','.md','.pdf')){continue}
            $expanded+=$entry.Length
            if($expanded -gt 1GB){throw 'Archive exceeds the 1 GiB extraction budget; original ZIP was retained'}
            $out=[IO.Path]::GetFullPath((Join-Path $destination $entry.FullName))
            if(-not $out.StartsWith($destination+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe path in model ZIP'}
            if(Test-Path -LiteralPath $out){continue}
            New-Item -ItemType Directory -Path (Split-Path -Parent $out) -Force | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$out,$false)
        }
    } finally {$zip.Dispose()}
}

if($Group -in @('All','Realistic')){
    # 原始未压缩 GLB / glTF；不用当前导入器不支持的 Draco / KTX2 变体。
    foreach($name in @('Avocado','WaterBottle','BoomBox','Lantern','AntiqueCamera','ToyCar','SheenChair')){
        $relative="03_realistic/$name"
        $upstream="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/$revision/Models/$name"
        $info=Invoke-RestMethod -Uri "https://api.github.com/repos/KhronosGroup/glTF-Sample-Assets/contents/Models/$name/glTF-Binary/$name.glb`?ref=$revision"
        Get-LibraryFile $info.download_url "$relative/$name.glb" $info.size
        Get-LibraryFile "$upstream/LICENSE.md" "$relative/LICENSE.md"
        Get-LibraryFile "$upstream/README.md" "$relative/SOURCE.md"
        Write-Host "Downloaded: $name"
    }
    $flight=Invoke-RestMethod -Uri "https://api.github.com/repos/KhronosGroup/glTF-Sample-Assets/contents/Models/FlightHelmet/glTF`?ref=$revision"
    foreach($file in $flight){Get-LibraryFile $file.download_url "03_realistic/FlightHelmet/glTF/$($file.name)" $file.size}
    $flightSource="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/$revision/Models/FlightHelmet"
    Get-LibraryFile "$flightSource/LICENSE.md" '03_realistic/FlightHelmet/LICENSE.md'
    Get-LibraryFile "$flightSource/README.md" '03_realistic/FlightHelmet/SOURCE.md'
    Get-LibraryFile "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/$revision/LICENSES/LicenseRef-LegalMark-UX3D.txt" '03_realistic/AntiqueCamera/UX3D-MARK-LICENSE.txt'
    Write-Host 'Downloaded: FlightHelmet (complete glTF + BIN + PNG dependencies)'
}

if($Group -in @('All','Cartoon','Anime')){
    $manifestPath=Join-Path $modelRoot 'download-manifest.json'
    if(-not (Test-Path -LiteralPath $manifestPath)){throw 'Additional model manifest is not present yet; use -Group Realistic first'}
    $manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    foreach($asset in $manifest.assets){
        if($Group -ne 'All' -and $asset.group -ne $Group){continue}
        foreach($file in $asset.files){Get-LibraryFile $file.url $file.path ([long]$file.bytes)}
        if($asset.archive -and $asset.extract_to){Expand-LibraryArchive $asset.archive $asset.extract_to}
        Write-Host "Downloaded: $($asset.name)"
    }
}
Write-Host "Library: $modelRoot"
Write-Host 'Downloads/extraction only. No editor, compilation, rendering, benchmarks or tests were started.'
