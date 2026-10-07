[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$sceneRepo = Split-Path -Parent $PSScriptRoot
$sampleRoot = Join-Path $sceneRepo 'assets/import_samples'
$assetRevision = 'edc7c9e67c639d230715049ee31f9a96a6babbbe'
# 固定上游版本，避免同名资源更新后静默改变示例；不覆盖已有下载。
function Get-SampleFile([string]$Url, [string]$Destination) {
    if (Test-Path -LiteralPath $Destination) { return }
    New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
    $partial = "$Destination.partial"
    Invoke-WebRequest -Uri $Url -OutFile $partial -TimeoutSec 90
    Move-Item -LiteralPath $partial -Destination $Destination
}
foreach ($sampleModel in @('BoxVertexColors','CesiumMilkTruck')) {
    $upstream = "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/$assetRevision/Models/$sampleModel"
    Get-SampleFile "$upstream/glTF-Binary/$sampleModel.glb" (Join-Path $sampleRoot "models/$sampleModel/$sampleModel.glb")
    Get-SampleFile "$upstream/README.md" (Join-Path $sampleRoot "models/$sampleModel/SOURCE-LICENSE.md")
    Write-Host "Ready: $sampleModel (license retained in SOURCE-LICENSE.md)"
}
Get-SampleFile "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/$assetRevision/LICENSES/LicenseRef-LegalMark-Cesium.txt" (Join-Path $sampleRoot 'models/CesiumMilkTruck/CESIUM-MARK-LICENSE.txt')
$textureArchive = Join-Path $sampleRoot 'downloads/Tiles074_1K-JPG.zip'
Get-SampleFile 'https://ambientcg.com/get?file=Tiles074_1K-JPG.zip' $textureArchive
$textureFolder = Join-Path $sampleRoot 'textures/Tiles074'
New-Item -ItemType Directory -Path $textureFolder -Force | Out-Null
# 只提取运行时能使用的三张图片。不给归档内任意文件名构造写入目标。
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archiveHandle = [System.IO.Compression.ZipFile]::OpenRead($textureArchive)
try {
    foreach ($imageName in @('Tiles074_1K-JPG_Color.jpg','Tiles074_1K-JPG_NormalGL.jpg','Tiles074_1K-JPG_Roughness.jpg')) {
        $entry = $archiveHandle.GetEntry($imageName)
        if ($null -eq $entry) { throw "Texture package lacks $imageName" }
        $destination = Join-Path $textureFolder $imageName
        if (-not (Test-Path -LiteralPath $destination)) {
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $false)
        }
    }
} finally { $archiveHandle.Dispose() }
Write-Host 'Ready: Tiles074 color / OpenGL normal / roughness (CC0)'
Write-Host "Resource directory: $sampleRoot"
