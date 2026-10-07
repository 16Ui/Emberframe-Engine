[CmdletBinding()]
param([ValidateSet('Release','Debug')][string]$Config='Release')
$ErrorActionPreference = 'Stop'
$sceneRepo = Split-Path -Parent $PSScriptRoot
$program = Join-Path $sceneRepo "bin/$Config/emberframe_workbench.exe"
if (-not (Test-Path -LiteralPath $program)) { throw '请先编译 emberframe_workbench。' }
$sampleDirectory = Join-Path $sceneRepo 'output/import-scenes'
New-Item -ItemType Directory -Path $sampleDirectory -Force | Out-Null
$sceneFile = Join-Path $sampleDirectory 'free-assets.ember'
if (-not (Test-Path -LiteralPath $sceneFile)) {
    & $program --make-import-project $sceneFile
    if ($LASTEXITCODE -ne 0) { throw '示例工程创建失败，请先运行 download-import-samples.ps1。' }
}
# 已编辑保存的示例工程不会被重新生成覆盖。只打开正常编辑器，不运行验证。
& $program --project $sceneFile --fit-viewport --ui-panel 3
if ($LASTEXITCODE -ne 0) { throw '示例工程打开失败。' }
