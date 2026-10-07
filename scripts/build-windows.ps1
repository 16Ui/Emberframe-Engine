
param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",

    [string]$Target = "chapter_6",

    [string]$VulkanSdk = $env:VULKAN_SDK,

    [switch]$BuildShaders,

    [switch]$Run
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

function Find-VulkanSdk {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return $RequestedPath
    }

    $roots = @("C:\VulkanSDK", "D:\develop\VulkanSDK")
    foreach ($root in $roots) {
        if (Test-Path -LiteralPath $root) {
            $candidate = Get-ChildItem -LiteralPath $root -Directory |
                Sort-Object Name -Descending |
                Select-Object -First 1
            if ($candidate) {
                return $candidate.FullName
            }
        }
    }

    throw "Vulkan SDK was not found. Install it or pass -VulkanSdk <path>."
}

$sdkPath = Find-VulkanSdk -RequestedPath $VulkanSdk
$headerPath = Join-Path $sdkPath "Include\vulkan\vulkan.h"
$validatorPath = Join-Path $sdkPath "Bin\glslangValidator.exe"

if (-not (Test-Path -LiteralPath $headerPath) -or
    -not (Test-Path -LiteralPath $validatorPath)) {
    throw "Invalid Vulkan SDK path: $sdkPath"
}

$env:VULKAN_SDK = $sdkPath
$env:Path = "$(Join-Path $sdkPath 'Bin');$env:Path"
$env:VK_LAYER_PATH = Join-Path $sdkPath "Bin"

Push-Location $repoRoot
try {
    cmake --preset windows-vs2022
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

    $BuildTargets = @($Target)
    if ($BuildShaders -or $Target -like "chapter_*") {
        $BuildTargets = @("Shaders", $Target)
    }

    cmake --build --preset windows-release-chapter6 --config $Config --target $BuildTargets --parallel
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }

    if ($Run) {
        $executablePath = Join-Path $repoRoot "bin\$Config\$Target.exe"
        if (-not (Test-Path -LiteralPath $executablePath)) {
            throw "Built executable was not found: $executablePath"
        }
        & $executablePath
        if ($LASTEXITCODE -ne 0) { throw "Executable failed with exit code $LASTEXITCODE." }
    }
}
finally {
    Pop-Location
}
