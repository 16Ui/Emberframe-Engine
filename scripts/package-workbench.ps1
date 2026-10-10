# Windows x64 portable delivery. This script never runs the Workbench or its tests.
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$BinaryDirectory,
    [string]$OutputDirectory,
    [string]$VulkanSdk = $env:VULKAN_SDK,
    [string]$FontPath,
    [string]$FontLicensePath,
    [string]$FontSourcePath,
    [string]$AssetManifestPath,
    [string]$BuildMetadataPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $BinaryDirectory) { $BinaryDirectory = Join-Path $repoRoot 'bin/Release' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'output/delivery' }
if (-not $FontPath) { $FontPath = Join-Path $repoRoot 'assets/fonts/DroidSansFallback.ttf' }
if (-not $FontLicensePath) { $FontLicensePath = Join-Path $repoRoot 'assets/fonts/LICENSE.txt' }
$BinaryDirectory = [IO.Path]::GetFullPath($BinaryDirectory)
$defaultBinaryDirectory = [IO.Path]::GetFullPath((Join-Path $repoRoot 'bin/Release'))
if (-not $SkipBuild -and -not $BinaryDirectory.TrimEnd([char[]]'\/').Equals(
    $defaultBinaryDirectory.TrimEnd([char[]]'\/'), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'A custom -BinaryDirectory requires -SkipBuild; the existing build script only writes bin/Release.'
}

function Require-File([string]$Path) {
    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($item.PSIsContainer -or $item.Length -eq 0) { throw "Expected a nonempty file: $Path" }
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Delivery inputs must be regular files, not links: $Path"
    }
    return $item.FullName
}

function Write-Utf8([string]$Path, [string]$Content) {
    [IO.File]::WriteAllText($Path, $Content, [Text.UTF8Encoding]::new($false))
}

function Invoke-RepoGit([string[]]$GitArguments) {
    # Per-command trust only: never mutate a user's global Git configuration.
    $result = & git -c "safe.directory=$($repoRoot.Replace('\','/'))" -C $repoRoot @GitArguments
    if ($LASTEXITCODE -ne 0) { throw "Could not record Git provenance: $($GitArguments -join ' ')" }
    return $result
}

# Read import tables instead of invoking dumpbin or loading DLLs. -SkipBuild needs
# PowerShell and the existing outputs, not a VS shell or a Vulkan SDK installation.
function Get-PeImports([string]$Path) {
    [byte[]]$bytes = [IO.File]::ReadAllBytes((Require-File $Path))
    function U16([long]$Offset) {
        if ($Offset -lt 0 -or $Offset + 2 -gt $bytes.LongLength) { throw "Truncated PE: $Path" }
        return [BitConverter]::ToUInt16($bytes, [int]$Offset)
    }
    function U32([long]$Offset) {
        if ($Offset -lt 0 -or $Offset + 4 -gt $bytes.LongLength) { throw "Truncated PE: $Path" }
        return [BitConverter]::ToUInt32($bytes, [int]$Offset)
    }
    if ((U16 0) -ne 0x5a4d) { throw "Not a PE binary: $Path" }
    [long]$pe = U32 0x3c
    if ((U32 $pe) -ne 0x4550 -or (U16 ($pe + 4)) -ne 0x8664) {
        throw "Only AMD64 PE binaries are supported: $Path (Release provenance is recorded separately)"
    }
    $optional = $pe + 24
    if ((U16 $optional) -ne 0x20b) { throw "Expected PE32+ binary: $Path" }
    $count = U16 ($pe + 6)
    $optionalSize = U16 ($pe + 20)
    $sectionBase = $optional + $optionalSize
    if ($optionalSize -lt 112 -or $sectionBase + [long]$count * 40 -gt $bytes.LongLength) {
        throw "Invalid optional header or section table: $Path"
    }
    $directoryCount = U32 ($optional + 108)
    if ($directoryCount -gt [math]::Floor(($optionalSize - 112) / 8)) {
        throw "PE data directories exceed optional header: $Path"
    }
    $headerSize = U32 ($optional + 60)
    if ($headerSize -lt $sectionBase + [long]$count * 40 -or $headerSize -gt $bytes.LongLength) {
        throw "Invalid PE header extent: $Path"
    }
    function Rva-Offset([uint32]$Rva) {
        if ($Rva -lt $headerSize) { return [long]$Rva }
        for ($section = 0; $section -lt $count; ++$section) {
            $s = $sectionBase + $section * 40
            $start = U32 ($s + 12); $rawSize = U32 ($s + 16)
            if ($Rva -ge $start -and [uint64]$Rva -lt ([uint64]$start + $rawSize)) {
                [long]$raw = (U32 ($s + 20)) + ([long]$Rva - $start)
                if ($raw -lt $headerSize -or $raw -ge $bytes.LongLength) { throw "Invalid PE raw extent: $Path" }
                return $raw
            }
        }
        throw "Unmapped PE RVA $Rva in $Path"
    }
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    # Normal imports and delay-load imports both have to be satisfied at runtime.
    foreach ($table in @(
        [pscustomobject]@{ Index = 1; Stride = 20; NameOffset = 12 },
        [pscustomobject]@{ Index = 13; Stride = 32; NameOffset = 4 }
    )) {
        if ($directoryCount -le $table.Index) { continue }
        $directory = $optional + 112 + $table.Index * 8
        $rva = U32 $directory; $size = U32 ($directory + 4)
        if ($rva -eq 0 -and $size -eq 0) { continue }
        if ($rva -eq 0 -or $size -lt $table.Stride -or $size -gt $bytes.LongLength -or
            [uint64]$rva + $size -gt [uint32]::MaxValue) {
            throw "Invalid import directory $($table.Index): $Path"
        }
        $terminated = $false
        for ([long]$offset = 0; $offset + $table.Stride -le $size; $offset += $table.Stride) {
            # Map each descriptor through the RVA table. Do not assume all imports
            # occupy one contiguous file range just because the first one does.
            $descriptor = Rva-Offset ([uint32]([uint64]$rva + $offset))
            $lastByte = Rva-Offset ([uint32]([uint64]$rva + $offset + $table.Stride - 1))
            if ($lastByte -ne $descriptor + $table.Stride - 1) { throw "Split/truncated import descriptor: $Path" }
            $empty = $true
            for ($field = 0; $field -lt $table.Stride; $field += 4) {
                if ((U32 ($descriptor + $field)) -ne 0) { $empty = $false; break }
            }
            if ($empty) { $terminated = $true; break }
            $nameRva = U32 ($descriptor + $table.NameOffset)
            if ($nameRva -eq 0) { throw "Import descriptor has no DLL name: $Path" }
            if ($table.Index -eq 13 -and (U32 $descriptor) -ne 1) {
                throw "Unsupported VA-style delay import: $Path"
            }
            $nameBytes = [Collections.Generic.List[byte]]::new()
            $nameTerminated = $false
            for ($letter = 0; $letter -lt 260; ++$letter) {
                if ([uint64]$nameRva + $letter -gt [uint32]::MaxValue) { throw "Import name RVA overflow: $Path" }
                $at = Rva-Offset ([uint32]([uint64]$nameRva + $letter))
                if ($bytes[$at] -eq 0) { $nameTerminated = $true; break }
                if ($bytes[$at] -gt 127) { throw "Non-ASCII import name: $Path" }
                [void]$nameBytes.Add($bytes[$at])
            }
            if (-not $nameTerminated) { throw "Unterminated/oversized import name: $Path" }
            $name = [Text.Encoding]::ASCII.GetString($nameBytes.ToArray())
            if ($name -notmatch '^[A-Za-z0-9_.-]+\.dll$') { throw "Unsafe import name: $name" }
            [void]$names.Add($name)
        }
        if (-not $terminated) { throw "Import directory has no null descriptor: $Path" }
    }
    # Emit strings on the success stream, then collect with @(...) at the call site.
    # Do NOT return ,$array / Write-Output -NoEnumerate: that makes all DLLs one item.
    $names | Sort-Object
}

function Get-EmbeddedNotices([string]$Source, [string[]]$RequiredPatterns) {
    $text = [IO.File]::ReadAllText((Require-File (Join-Path $repoRoot $Source)))
    $comments = [regex]::Matches($text, '(?s:/\*.*?\*/)|(?m:^(?:[ \t]*//[^\r\n]*(?:\r?\n|$))+)')
    [string[]]$notices = @($comments | ForEach-Object { $_.Value } | Where-Object {
        $_ -match '(?i)Permission is hereby granted|Redistribution and use|Boost Software License|Licensed under the Apache License|code is distributed under the MIT license'
    } | Select-Object -Unique)
    if ($notices.Count -eq 0) { throw "No embedded license notice found in $Source" }
    $retained = $notices -join "`n`n"
    foreach ($pattern in $RequiredPatterns) {
        if ($retained -notmatch $pattern) { throw "Expected embedded notice '$pattern' is missing from $Source; review vendor changes." }
    }
    # Return one scalar document, not an array of comments or a match object.
    return "Notices retained from $Source`n`n$retained"
}

# Fail before creating an output directory if the mandatory licensed font is absent.
$FontPath = Require-File $FontPath
$FontLicensePath = Require-File $FontLicensePath
if ([IO.Path]::GetExtension($FontPath) -ine '.ttf') { throw 'Provide a Chinese-capable TTF, not a proprietary Windows TTC.' }
$fontTerms = [IO.File]::ReadAllText($FontLicensePath)
if ($fontTerms -notmatch 'Apache License|SIL OPEN FONT LICENSE|Permission is hereby granted') {
    throw 'Provide the complete redistribution license for the supplied font.'
}
$bundledFontPath = [IO.Path]::GetFullPath((Join-Path $repoRoot 'assets/fonts/DroidSansFallback.ttf'))
if (-not $FontSourcePath -and $FontPath.Equals($bundledFontPath, [StringComparison]::OrdinalIgnoreCase)) {
    [string[]]$fontSources = @(@('SOURCE.json','SOURCE.md','SOURCE.txt') | ForEach-Object {
        $candidate = Join-Path $repoRoot "assets/fonts/$_"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $candidate }
    })
    if ($fontSources.Count -gt 1) { throw 'Multiple font SOURCE records: choose one explicitly with -FontSourcePath.' }
    if ($fontSources.Count -eq 1) { $FontSourcePath = $fontSources[0] }
}
$fontSourceDestination = $null
if ($FontSourcePath) {
    $FontSourcePath = Require-File $FontSourcePath
    $sourceExtension = [IO.Path]::GetExtension($FontSourcePath).ToLowerInvariant()
    if ($sourceExtension -notin '.json','.md','.txt') { throw 'Font source record must be JSON, Markdown or text.' }
    $fontSourceDestination = "assets/fonts/SOURCE$sourceExtension"
}

# Named entries avoid positional nested-array flattening if a list is later
# returned by a helper. Preflight every notice before creating delivery output.
[object[]]$licenseCopies = @(
    [pscustomobject]@{ Source='third_party/SDL/LICENSE.txt'; Name='SDL2.txt' },
    [pscustomobject]@{ Source='third_party/SDL/src/video/yuv2rgb/LICENSE'; Name='SDL-yuv2rgb.txt' },
    [pscustomobject]@{ Source='third_party/SDL/src/hidapi/LICENSE-bsd.txt'; Name='SDL-hidapi-BSD.txt' },
    [pscustomobject]@{ Source='third_party/fastgltf/LICENSE.md'; Name='fastgltf.txt' },
    [pscustomobject]@{ Source='third_party/fmt/LICENSE.rst'; Name='fmt.txt' }
)
[object[]]$embeddedLicenses = @(
    [pscustomobject]@{ Source='third_party/vma/vk_mem_alloc.h'; Name='VMA-notices.txt'; Patterns=@('Advanced Micro Devices','Permission is hereby granted','THE SOFTWARE IS PROVIDED') },
    [pscustomobject]@{ Source='third_party/vkbootstrap/VkBootstrap.h'; Name='vk-bootstrap-notices.txt'; Patterns=@('Charles Giessen','Permission is hereby granted','THE SOFTWARE IS PROVIDED') },
    [pscustomobject]@{ Source='third_party/stb_image/stb_image.h'; Name='stb_image-notices.txt'; Patterns=@('Sean Barrett','ALTERNATIVE A','ALTERNATIVE B','THE SOFTWARE IS PROVIDED') },
    [pscustomobject]@{ Source='third_party/imgui/imstb_truetype.h'; Name='stb_truetype-notices.txt'; Patterns=@('Sean Barrett','ALTERNATIVE A','ALTERNATIVE B','THE SOFTWARE IS PROVIDED') },
    [pscustomobject]@{ Source='third_party/imgui/imstb_rectpack.h'; Name='stb_rectpack-notices.txt'; Patterns=@('Sean Barrett','ALTERNATIVE A','ALTERNATIVE B','THE SOFTWARE IS PROVIDED') },
    [pscustomobject]@{ Source='third_party/imgui/imstb_textedit.h'; Name='stb_textedit-notices.txt'; Patterns=@('Sean Barrett','ALTERNATIVE A','ALTERNATIVE B','THE SOFTWARE IS PROVIDED') },
    [pscustomobject]@{ Source='third_party/fastgltf/deps/simdjson/simdjson.h'; Name='simdjson-header-notices.txt'; Patterns=@('Martin Moene','Boost Software License','Facebook','Redistribution and use') },
    [pscustomobject]@{ Source='third_party/fastgltf/deps/simdjson/simdjson.cpp'; Name='simdjson-source-notices.txt'; Patterns=@('Martin Moene','Boost Software License','Facebook','Redistribution and use','2009 Florian\s+Loitsch','MIT license') }
)
foreach ($entry in $licenseCopies) { [void](Require-File (Join-Path $repoRoot $entry.Source)) }
$embeddedDocuments = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $embeddedLicenses) {
    $content = Get-EmbeddedNotices -Source $entry.Source -RequiredPatterns $entry.Patterns
    $embeddedDocuments.Add($entry.Name, $content)
}

if (-not $SkipBuild) {
    $buildParameters = @{ Config = 'Release'; Target = 'emberframe_workbench' }
    if ($VulkanSdk) { $buildParameters.VulkanSdk = $VulkanSdk }
    & (Join-Path $PSScriptRoot 'build-windows.ps1') @buildParameters
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed; no package created.' }
}
$BinaryDirectory = (Get-Item -LiteralPath $BinaryDirectory -ErrorAction Stop).FullName
$exe = Require-File (Join-Path $BinaryDirectory 'emberframe_workbench.exe')
$revision = (Invoke-RepoGit -GitArguments @('rev-parse', 'HEAD')).Trim()
$statusText = (Invoke-RepoGit -GitArguments @('status', '--porcelain', '--untracked-files=no')) -join "`n"
$dirty = -not [string]::IsNullOrWhiteSpace($statusText)
$binaryHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
$buildMetadata = $null
if ($BuildMetadataPath) {
    $buildMetadata = Get-Content -LiteralPath (Require-File $BuildMetadataPath) -Raw | ConvertFrom-Json
    if ($buildMetadata.executableSha256 -ine $binaryHash -or $buildMetadata.gitRevision -ine $revision) {
        throw 'Build metadata does not match both this binary and this checkout.'
    }
}

# Only import-reachable project DLLs are copied. Never copy System32, the loader,
# GPU drivers, SDK tools, debug runtimes, layers or all DLLs from a build folder.
$projectDlls = @('SDL2.dll', 'fastgltf.dll', 'fastgltf_simdjson.dll', 'simdjson.dll', 'fmt.dll')
$systemDlls = @('kernel32.dll','kernelbase.dll','ntdll.dll','user32.dll','gdi32.dll','gdi32full.dll',
    'advapi32.dll','shell32.dll','shlwapi.dll','ole32.dll','oleaut32.dll','comdlg32.dll',
    'comctl32.dll','imm32.dll','winmm.dll','version.dll','setupapi.dll','hid.dll','cfgmgr32.dll',
    'ws2_32.dll','rpcrt4.dll','bcrypt.dll','bcryptprimitives.dll','crypt32.dll','secur32.dll',
    'wintrust.dll','msvcrt.dll','ucrtbase.dll','dwmapi.dll','uxtheme.dll','powrprof.dll',
    'd3d9.dll','d3d11.dll','dxgi.dll','dxguid.dll','dinput8.dll','dsound.dll','avrt.dll',
    'winhttp.dll','wininet.dll','normaliz.dll','psapi.dll','dwrite.dll','msimg32.dll')
$pending = [Collections.Generic.Queue[string]]::new(); $pending.Enqueue($exe)
$runtimeFiles = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
$externalImports = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
while ($pending.Count -gt 0) {
    $file = $pending.Dequeue()
    [string[]]$imports = @(Get-PeImports -Path $file)
    foreach ($dll in $imports) {
        if ($projectDlls -contains $dll) {
            if (-not $runtimeFiles.ContainsKey($dll)) {
                $source = Require-File (Join-Path $BinaryDirectory $dll)
                $runtimeFiles.Add($dll, $source); $pending.Enqueue($source)
            }
        } elseif ($dll -ieq 'vulkan-1.dll' -or $systemDlls -contains $dll -or
                  $dll -match '^(api-ms-win-|ext-ms-win-).+\.dll$' -or
                  $dll -match '^(msvcp140(?:_[0-9]+|_atomic_wait|_codecvt_ids)?|vcruntime140(?:_1)?|concrt140)\.dll$') {
            [void]$externalImports.Add($dll)
        } else {
            throw "Unclassified dependency '$dll' from '$file'. Review its origin/license; do not copy arbitrary DLLs."
        }
    }
}

# Source-derived shader names detect missing deployed SPV after adding a new pass.
$shaderSources = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'engine/lab/shaders') -File |
    Where-Object { $_.Extension -in '.vert','.frag','.comp' } | Sort-Object Name)
if ($shaderSources.Count -eq 0) { throw 'No Workbench shader sources found.' }
$shaderFiles = @($shaderSources | ForEach-Object {
    $path = Require-File (Join-Path $BinaryDirectory "lab_shaders/$($_.Name).spv")
    $code = [IO.File]::ReadAllBytes($path)
    if ($code.Length -lt 20 -or $code.Length % 4 -ne 0 -or [BitConverter]::ToUInt32($code, 0) -ne 0x07230203) {
        throw "Invalid SPV file: $path"
    }
    $path
})

# Optional assets are individually approved and hashed. A folder is never included
# recursively: a GLB/HDR, its dependencies, its attribution and its license are entries.
$extraAssets = @()
if ($AssetManifestPath) {
    $assetList = Get-Content -LiteralPath (Require-File $AssetManifestPath) -Raw | ConvertFrom-Json
    if ($assetList.schemaVersion -ne 1) { throw 'Asset manifest schemaVersion must be 1.' }
    $base = Split-Path -Parent ([IO.Path]::GetFullPath($AssetManifestPath))
    $extraAssets = @($assetList.files | ForEach-Object {
        $destination = [string]$_.destination
        if ($destination -notmatch '^assets/showcase/[A-Za-z0-9_./-]+$' -or $destination -match '(^|/)\.\.?(/|$)') {
            throw "Optional assets must stay under assets/showcase/: $destination"
        }
        if ($_.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or -not $_.author -or -not $_.license -or -not $_.origin) {
            throw "Missing SHA256, author, license or origin for $destination"
        }
        $source = Require-File (Join-Path $base $_.source)
        $terms = Require-File (Join-Path $base $_.licenseFile)
        if ($_.licenseSha256 -notmatch '^[a-fA-F0-9]{64}$' -or
            (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ine $_.sha256 -or
            (Get-FileHash -LiteralPath $terms -Algorithm SHA256).Hash -ine $_.licenseSha256) {
            throw "Asset or license hash mismatch: $destination"
        }
        if ([IO.Path]::GetExtension($source) -notin '.glb','.gltf','.hdr','.exr','.png','.jpg','.jpeg','.obj','.mtl','.bin','.ktx2','.ember','.txt','.md') {
            throw "Unsupported delivery asset type: $source"
        }
        [pscustomobject]@{ Source=$source; Destination=$destination; Terms=$terms; Author=$_.author; License=$_.license; Origin=$_.origin }
    })
}

# Always use a fresh child directory. Failed attempts remain recoverable and are not
# silently overwritten or deleted; re-running creates a new child.
$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$runName = "EmberFrame-Windows-x64-$($revision.Substring(0, 8))-$stamp-$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$deliveryRoot = [IO.Path]::GetFullPath((Join-Path $OutputDirectory $runName))
if (Test-Path -LiteralPath $deliveryRoot) { throw "Refusing to overwrite $deliveryRoot" }
$packageName = 'EmberFrame-Windows-x64'
$packageRoot = Join-Path $deliveryRoot $packageName
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
$copied = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
function Copy-Payload([string]$Source, [string]$Relative) {
    if ([IO.Path]::IsPathRooted($Relative) -or $Relative -match '(^|[\\/])\.\.([\\/]|$)') { throw "Unsafe destination: $Relative" }
    if (-not $copied.Add($Relative.Replace('\','/'))) { throw "Duplicate delivery file: $Relative" }
    $destination = [IO.Path]::GetFullPath((Join-Path $packageRoot $Relative))
    if (-not $destination.StartsWith($packageRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Destination outside package: $Relative"
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Require-File $Source) -Destination $destination
}
Copy-Payload $exe 'emberframe_workbench.exe'
if ((Get-FileHash -LiteralPath (Join-Path $packageRoot 'emberframe_workbench.exe') -Algorithm SHA256).Hash -ine $binaryHash) {
    throw 'EXE changed during packaging. Retained this incomplete batch; retry after the build has finished.'
}
foreach ($name in @($runtimeFiles.Keys | Sort-Object)) { Copy-Payload $runtimeFiles[$name] $name }
foreach ($shader in $shaderFiles) { Copy-Payload $shader ('lab_shaders/' + [IO.Path]::GetFileName($shader)) }
foreach ($name in @('emberframe-icon.png','emberframe.ico')) {
    Copy-Payload (Join-Path $repoRoot "assets/branding/$name") "assets/branding/$name"
}
Copy-Payload (Join-Path $repoRoot 'assets/software_renderer/textured_cube.obj') 'assets/software_renderer/textured_cube.obj'
Copy-Payload $FontPath 'assets/fonts/DroidSansFallback.ttf'
Copy-Payload $FontLicensePath 'licenses/UI-FONT-LICENSE.txt'
if ($FontSourcePath) { Copy-Payload $FontSourcePath $fontSourceDestination }
Copy-Payload (Join-Path $repoRoot 'LICENSE.txt') 'LICENSE.txt'
Copy-Payload (Join-Path $PSScriptRoot 'start-portable-workbench.cmd') 'Start-EmberFrame.cmd'
Copy-Payload (Join-Path $repoRoot 'docs/DEMO_DELIVERY.md') 'DEMO_DELIVERY.md'
Copy-Payload (Join-Path $repoRoot 'docs/THIRD_PARTY_LICENSES.md') 'THIRD_PARTY_LICENSES.md'
Copy-Payload (Join-Path $repoRoot 'docs/licenses/WORKBENCH-NOTICES.txt') 'licenses/WORKBENCH-NOTICES.txt'
Copy-Payload (Join-Path $repoRoot 'docs/licenses/Apache-2.0.txt') 'licenses/Apache-2.0.txt'
Copy-Payload (Join-Path $repoRoot 'docs/licenses/Boost-1.0.txt') 'licenses/Boost-1.0.txt'
foreach ($entry in $licenseCopies) { Copy-Payload (Join-Path $repoRoot $entry.Source) ('licenses/' + $entry.Name) }
foreach ($entry in $embeddedLicenses) {
    Write-Utf8 (Join-Path $packageRoot ('licenses/' + $entry.Name)) $embeddedDocuments[$entry.Name]
}
$assetAttributions = @(); $assetIndex = 0
foreach ($asset in $extraAssets) {
    Copy-Payload $asset.Source $asset.Destination
    $licenseDestination = "licenses/asset-$assetIndex.txt"
    Copy-Payload $asset.Terms $licenseDestination
    $assetAttributions += [ordered]@{ path=$asset.Destination; author=$asset.Author; license=$asset.License; origin=$asset.Origin; licenseFile=$licenseDestination }
    ++$assetIndex
}
Write-Utf8 (Join-Path $packageRoot 'portable.ini') @'
; Presence-only marker. The runtime does not parse this file.
; Assets and precompiled shaders are relative to this executable.
; Driver cache uses SDL_GetPrefPath("16Ui","EmberFrame")/<ABI>/cache.
; Logs and saved projects use SDL_GetPrefPath("16Ui","EmberFrame")/output.
; The package folder may be read-only; the SDL user preference directory must be writable.
'@
$requirements = [ordered]@{
    os='Windows 10/11 x64'; gpuApi='Vulkan 1.3'; driver='GPU vendor driver including the system Vulkan loader'
    requiredFeatures=@('VK_KHR_swapchain','dynamicRendering','synchronization2','graphics+compute queue','at least 7 color attachments','required render/storage/sampling formats')
    cpuRuntime='Microsoft Visual C++ v14 x64 Redistributable, at least as new as the MSVC toolset used for the EXE'
    cpuRuntimeUrl='https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist'
    sdkRequired=$false; validationLayerRequired=$false; externalModelRequired=$false
    writableCache='SDL_GetPrefPath("16Ui","EmberFrame")/<ABI>/cache'; userOutput='SDL_GetPrefPath("16Ui","EmberFrame")/output'
    cacheAbiDirectory='Runtime-selected ABI key'; portableReadonlyFolderSupported=$true; userPrefMustBeWritable=$true
    shaderHotCompilation='Not included; optional development feature requires source and compiler'
}
Write-Utf8 (Join-Path $packageRoot 'requirements.json') ($requirements | ConvertTo-Json -Depth 6)
Write-Utf8 (Join-Path $packageRoot 'START_HERE.txt') @'
EmberFrame Windows x64 Demo

Extract the entire ZIP to a folder. Do not run inside the ZIP.
The package folder may be read-only. The SDL user preference directory must be writable.
Double-click Start-EmberFrame.cmd for the procedural material showcase.
Other showcases: Start-EmberFrame.cmd --showcase interior
                 Start-EmberFrame.cmd --showcase many-objects

No source checkout, Vulkan SDK or validation layer is required.
Install a current GPU vendor driver with Vulkan 1.3 support and a matching
Microsoft Visual C++ v14 x64 Redistributable if missing. See requirements.json.
The package does not contain the Vulkan loader, drivers or SDK executables.
Logs and project saves are in SDL_GetPrefPath("16Ui","EmberFrame")/output.
Driver cache is created in SDL_GetPrefPath("16Ui","EmberFrame")/<ABI>/cache,
where <ABI> is the runtime-selected ABI key. No cache is redistributed.
Neither cache nor daily output requires writes to the EXE/package folder.

See DEMO_DELIVERY.md for delivery acceptance, limitations and license inventory.
Build success or package hashes do not constitute GPU/visual verification.
'@
$records = @(Get-ChildItem -LiteralPath $packageRoot -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path=$_.FullName.Substring($packageRoot.Length + 1).Replace('\','/'); bytes=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
$manifest = [ordered]@{
    schemaVersion=1; product='EmberFrame'; target='windows-x64'; configuration='Release'
    createdUtc=[DateTime]::UtcNow.ToString('o'); gitRevision=$revision; trackedCheckoutDirty=$dirty
    buildInvoked=(-not $SkipBuild); executableSha256=$binaryHash
    binarySourceIdentity=if ($buildMetadata) { 'supplied-build-record-hash-matched' } elseif ($SkipBuild) { 'unknown-existing-binary' } else { 'built-in-this-invocation' }
    buildMetadata=$buildMetadata; portableMarker='portable.ini'; entrypoint='Start-EmberFrame.cmd'
    defaultArguments=@('--portable','--showcase','materials','--fit-viewport'); font='assets/fonts/DroidSansFallback.ttf'
    fontInput=[ordered]@{ fileName=[IO.Path]::GetFileName($FontPath); licenseFileName=[IO.Path]::GetFileName($FontLicensePath); sourceRecord=$fontSourceDestination; licenseMapping='supplied-by-packager-not-legally-audited' }
    requirements=$requirements; systemProvidedImports=@($externalImports | Sort-Object)
    optionalAssetAttributions=$assetAttributions; cpuTestsExecutedByPackager=$false; gpuTestsExecutedByPackager=$false
    manifestCoverage='All payload files except manifest.json itself; manifest hash is in the adjacent delivery summary.'
    files=$records
}
$manifestPath = Join-Path $packageRoot 'manifest.json'
Write-Utf8 $manifestPath ($manifest | ConvertTo-Json -Depth 20)
$archive = Join-Path $deliveryRoot ($runName + '.zip')
Compress-Archive -LiteralPath $packageRoot -DestinationPath $archive -CompressionLevel Optimal
$archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Utf8 ($archive + '.sha256') ("$archiveHash  $([IO.Path]::GetFileName($archive))`n")
$summary = [ordered]@{
    schemaVersion=1; archive=[IO.Path]::GetFileName($archive); archiveSha256=$archiveHash
    manifestSha256=(Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    gitRevision=$revision; payloadFileCount=$records.Count; shaderCount=$shaderFiles.Count
    runtimeValidation='not-run'; signing='unsigned'; releasePublication='not-performed'
}
$summaryPath = Join-Path $deliveryRoot 'delivery-summary.json'
Write-Utf8 $summaryPath ($summary | ConvertTo-Json -Depth 6)
Write-Host "Created portable package: $archive"
[pscustomobject]@{ PackageDirectory=$packageRoot; Archive=$archive; Manifest=$manifestPath; Summary=$summaryPath }
