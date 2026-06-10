# Normalize NEODRIVE export folders under out/ (lowercase ids, consistent WAV names).
# Usage: powershell -File scripts\normalize-engine-exports.ps1

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$OutRoot = Join-Path $RepoRoot "out"

$Renames = @(
    @{ From = "SR20DE_Autech";            To = "sr20de_autech" }
    @{ From = "skyline";                  To = "skyline_r33" }
    @{ From = "toyota_2jzgte_engine";     To = "toyota_2jz_gte" }
    @{ From = "lamborghini_fkp_37_engine"; To = "lamborghini_sian" }
    @{ From = "subaru_test";              To = "subaru_ej25" }
    @{ From = "RB26DETT";                 To = "rb26dett_r32" }
)

$Remove = @("vtc_camshaft_builder", "Tuned 13-B")

function Update-Manifest([string]$ManifestPath, [string]$OldId, [string]$NewId) {
    if (-not (Test-Path -LiteralPath $ManifestPath)) { return }
    $json = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    $json.id = $NewId
    foreach ($layer in $json.layers) {
        if ($layer.off) { $layer.off = $layer.off -replace [regex]::Escape($OldId), $NewId }
        if ($layer.on)  { $layer.on  = $layer.on  -replace [regex]::Escape($OldId), $NewId }
    }
    ($json | ConvertTo-Json -Depth 6) + "`n" | Set-Content -LiteralPath $ManifestPath -Encoding UTF8
}

foreach ($dirName in $Remove) {
    $path = Join-Path $OutRoot $dirName
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force
        Write-Host "Removed empty/failed: $dirName" -ForegroundColor Yellow
    }
}

foreach ($map in $Renames) {
    $fromDir = Join-Path $OutRoot $map.From
    $toDir = Join-Path $OutRoot $map.To
    if (-not (Test-Path -LiteralPath $fromDir)) {
        if (Test-Path -LiteralPath $toDir) {
            Write-Host "Already normalized: $($map.To)" -ForegroundColor DarkGray
        }
        continue
    }

    $oldId = $map.From
    if (Test-Path (Join-Path $fromDir "manifest.json")) {
        $oldId = (Get-Content (Join-Path $fromDir "manifest.json") -Raw | ConvertFrom-Json).id
    }

    Get-ChildItem -LiteralPath $fromDir -Filter "*.wav" | ForEach-Object {
        $newName = $_.Name -replace [regex]::Escape($oldId), $map.To
        if ($newName -ne $_.Name) {
            Rename-Item -LiteralPath $_.FullName -NewName $newName
        }
    }

    Update-Manifest (Join-Path $fromDir "manifest.json") $oldId $map.To

    if ($map.From -ne $map.To) {
        if (Test-Path -LiteralPath $toDir) {
            throw "Target already exists: $toDir"
        }
        $tmpDir = Join-Path $OutRoot ("_tmp_" + $map.To)
        if (Test-Path -LiteralPath $tmpDir) { Remove-Item -LiteralPath $tmpDir -Recurse -Force }
        Rename-Item -LiteralPath $fromDir -NewName (Split-Path -Leaf $tmpDir)
        Rename-Item -LiteralPath $tmpDir -NewName $map.To
    }

    Write-Host "Normalized: $($map.From) -> $($map.To)" -ForegroundColor Green
}

Write-Host ""
Write-Host "Done. See README-CLI-AUDIO-EXPORT.md for inventory." -ForegroundColor Cyan
