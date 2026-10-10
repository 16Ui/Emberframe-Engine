param([Parameter(Mandatory)][string]$AuditDirectory,[string]$ExtraDirectory,[string]$PortableDirectory)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repoRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$audit=(Get-Item -LiteralPath $AuditDirectory).FullName
$summary=Get-Content -LiteralPath (Join-Path $audit 'summary.json') -Raw | ConvertFrom-Json
$currentHash=(Get-FileHash -LiteralPath (Join-Path $repoRoot 'bin/Release/emberframe_workbench.exe') -Algorithm SHA256).Hash
if($currentHash -ine $summary.executableSha256){throw 'Evidence must match the current executable.'}
if(@($summary.cases | Where-Object {$_.exit -ne 0 -or $_.validationErrors}).Count){throw 'Audit contains a failed case.'}
if(-not $summary.boundsImageIdentical){throw 'A/B HDR output mismatch.'}
$evidence=Join-Path $repoRoot 'docs/evidence';$media=Join-Path $repoRoot 'docs/media'
New-Item -ItemType Directory -Path $evidence,$media -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $audit 'summary.json') -Destination (Join-Path $evidence 'internship-demo.json')
if($PortableDirectory){
    $portableSummary=Join-Path $PortableDirectory 'summary.json'
    $portable=Get-Content -LiteralPath $portableSummary -Raw | ConvertFrom-Json
    if($portable.executableSha256 -ine $currentHash -or @($portable.cases | Where-Object {$_.exit -ne 0}).Count -or -not $portable.packageFileListUnchanged){
        throw 'Portable evidence does not verify this executable.'
    }
    Copy-Item -LiteralPath $portableSummary -Destination (Join-Path $evidence 'portable-runtime.json')
}
foreach($name in @('cached','uncached','motion','materials')){
    $profileDirectory=Join-Path $audit "profile-$name"
    $index=Get-Content -LiteralPath (Join-Path $profileDirectory 'index.json') -Raw | ConvertFrom-Json
    $last=Join-Path $profileDirectory $index.profiles[-1]
    Copy-Item -LiteralPath $last -Destination (Join-Path $evidence "profile-$name.json")
}
$graph=Get-ChildItem -LiteralPath (Join-Path $audit 'graph-motion') -Filter '*.json' | Sort-Object Name | Select-Object -Last 1
Copy-Item -LiteralPath $graph.FullName -Destination (Join-Path $evidence 'graph-motion.json')
Add-Type -AssemblyName System.Drawing
$images=[Collections.Generic.List[object]]::new()
function Convert-EvidenceImage([string]$Source,[string]$Name){
    $target=Join-Path $media ($Name+'.png')
    $bitmap=[Drawing.Bitmap]::new($Source)
    try{$bitmap.Save($target,[Drawing.Imaging.ImageFormat]::Png)
        $images.Add([pscustomobject]@{file="docs/media/$Name.png";width=$bitmap.Width;height=$bitmap.Height;sha256=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash})
    }finally{$bitmap.Dispose()}
}
foreach($id in @('materials','interior','many-objects')){Convert-EvidenceImage (Join-Path $audit "showcase-$id.bmp") "showcase-$id"}
Convert-EvidenceImage (Join-Path $audit 'moving-object.bmp') 'moving-object'
if($ExtraDirectory){
    foreach($entry in @(@{source='materials-hdr';target='external-hdr'},@{source='editor-profiler';target='editor-profiler'})){
        Convert-EvidenceImage (Join-Path $ExtraDirectory ($entry.source+'.bmp')) $entry.target
    }
}
[ordered]@{schema='emberframe.public-media.v1';executableSha256=$currentHash;origin='Actual EmberFrame GPU readback; BMP converted losslessly to PNG; not generated illustrations';images=$images} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidence 'media.json') -Encoding utf8
Write-Output "Exported version-bound public evidence: $evidence"
