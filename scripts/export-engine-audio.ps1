# Interactive NEODRIVE export for engine-sim (Windows).
# Usage: powershell -File scripts\export-engine-audio.ps1
#    or: .\export-audio.cmd

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Cli = Join-Path $RepoRoot "build\Release\engine-sim-cli.exe"
$CatalogBaseUrl = "https://catalog.engine-sim.parts"

function Write-Title([string]$Text) {
    Write-Host ""
    Write-Host "=== $Text ===" -ForegroundColor Cyan
}

function Test-CancelInput([string]$Value) {
    return ($Value -match '^(c|cancel|q|quit)$')
}

function Read-YesNoCancel([string]$Prompt, [bool]$DefaultYes = $true) {
    $suffix = if ($DefaultYes) { "[Y/n/C]" } else { "[y/N/C]" }
    while ($true) {
        $answer = Read-Host "$Prompt $suffix"
        if ([string]::IsNullOrWhiteSpace($answer)) { return $(if ($DefaultYes) { 'yes' } else { 'no' }) }
        switch ($answer.ToLower()) {
            "y" { return 'yes' }
            "yes" { return 'yes' }
            "n" { return 'no' }
            "no" { return 'no' }
            "c" { return 'cancel' }
            "cancel" { return 'cancel' }
            "q" { return 'cancel' }
            "quit" { return 'cancel' }
            default { Write-Host 'Answer Y (yes), N (no), or C (cancel).' -ForegroundColor Yellow }
        }
    }
}

function Exit-Cancelled([string]$Message = 'Cancelled.') {
    Write-Host $Message -ForegroundColor Yellow
    exit 0
}

function Get-EngineNodeId([string]$MrPath) {
    $lines = Get-Content -LiteralPath $MrPath
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*public\s+node\s+(\S+)\s*\{') {
            $node = $Matches[1]
            if ($node -eq 'main') { continue }
            $end = [Math]::Min($i + 50, $lines.Count - 1)
            for ($j = $i; $j -le $end; $j++) {
                if ($lines[$j] -match '^\s*engine\s+engine\s*\(') {
                    return $node
                }
            }
        }
    }
    return [System.IO.Path]::GetFileNameWithoutExtension($MrPath)
}

function Get-EngineScripts {
    $assets = Join-Path $RepoRoot "assets\engines"
    Get-ChildItem -LiteralPath $assets -Recurse -Filter "*.mr" |
        Where-Object {
            $_.Name -notlike "_*" -and
            $_.Name -ne "engine_sim.mr" -and
            $_.Name -ne "radial.mr" -and
            $_.FullName -notmatch '\\catalog\\'
        } |
        Sort-Object FullName
}

function Repair-CatalogScript([string]$Script) {
    # Catalog scripts often target a newer engine-sim (vehicle/run/convolution).
    # Keep only the engine definition for headless CLI export.
    $lines = $Script -split "`r?`n"
    $result = New-Object System.Collections.Generic.List[string]
    $skip = $false
    $depth = 0

    foreach ($line in $lines) {
        if (-not $skip) {
            if ($line -match '^\s*private\s+node\s+\w+') {
                if ($line -match '^\s*private\s+node\s+(nissan_skyline|fs5r30a)\b') {
                    $skip = $true
                    $depth = Count-BraceDepth $line
                    if ($depth -le 0) { $skip = $false; $depth = 0 }
                    continue
                }
            }
            if ($line -match '^\s*public\s+node\s+main\s*\{') {
                $skip = $true
                $depth = Count-BraceDepth $line
                if ($depth -le 0) { $skip = $false; $depth = 0 }
                continue
            }
            if ($line -match '^\s*main\s*\(\s*\)\s*$') { continue }
            if ($line -match '^\s*(convolution|block_temperature|max_brake_force)\s*:') { continue }
            $result.Add($line)
        }
        else {
            $depth += Count-BraceDepth $line
            if ($depth -le 0) {
                $skip = $false
                $depth = 0
            }
        }
    }

    return (($result -join "`n").TrimEnd() + "`n")
}

function Count-BraceDepth([string]$Line) {
    $open = ([regex]::Matches($Line, '\{')).Count
    $close = ([regex]::Matches($Line, '\}')).Count
    return $open - $close
}

function Get-CatalogPartId([string]$InputText) {
    $trimmed = $InputText.Trim()
    if ($trimmed -match 'parts/(\d+)') { return $Matches[1] }
    if ($trimmed -match '^\d+$') { return $trimmed }
    return $null
}

function Import-CatalogEngine([string]$UrlOrId) {
    $partId = Get-CatalogPartId $UrlOrId
    if (-not $partId) {
        throw "Invalid link. Example: $CatalogBaseUrl/parts/2531"
    }

    $apiUrl = "$CatalogBaseUrl/api/parts/$partId"
    Write-Host "Downloading from catalog (part #$partId)..." -ForegroundColor Cyan

    $part = Invoke-RestMethod -Uri $apiUrl -UseBasicParsing
    if (-not $part.script) {
        throw "This catalog entry has no exportable engine script."
    }

    $scriptName = if ($part.script_name) { $part.script_name } else { "part_$partId.mr" }
    $catalogDir = Join-Path $RepoRoot "assets\engines\catalog\part_$partId"
    New-Item -ItemType Directory -Force -Path $catalogDir | Out-Null

    $mrPath = Join-Path $catalogDir $scriptName
    $repaired = Repair-CatalogScript $part.script
    $utf8NoBom = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($mrPath, $repaired, $utf8NoBom)
    Write-Host 'Script adapted for CLI export (vehicle/run stripped).' -ForegroundColor DarkGray

    $engineId = Get-EngineNodeId $mrPath
    $rel = $mrPath.Substring($RepoRoot.Length + 1)

    Write-Host "Saved: $rel" -ForegroundColor Green
    if ($part.name) {
        Write-Host "Catalog name: $($part.name)" -ForegroundColor DarkGray
    }

    return [PSCustomObject]@{
        FullName = $mrPath
        RelPath = $rel
        EngineId = $engineId
        CatalogPartId = $partId
        CatalogName = $part.name
    }
}

function Select-LocalEngine {
    $engines = @(Get-EngineScripts)
    if ($engines.Count -eq 0) {
        Write-Host 'No .mr files found under assets\engines' -ForegroundColor Red
        return $null
    }

    Write-Host ""
    Write-Host "Local engines ($($engines.Count)):" -ForegroundColor Green
    for ($i = 0; $i -lt $engines.Count; $i++) {
        $rel = $engines[$i].FullName.Substring($RepoRoot.Length + 1)
        $nodeId = Get-EngineNodeId $engines[$i].FullName
        Write-Host ("  {0,2}. {1}  (id: {2})" -f ($i + 1), $rel, $nodeId)
    }

    Write-Host ""
    Write-Host '  C. Cancel' -ForegroundColor DarkGray
    $pick = Read-Host "Engine number (1-$($engines.Count), C = cancel)"
    if (Test-CancelInput $pick) { return 'cancel' }
    if (-not ($pick -match '^\d+$') -or [int]$pick -lt 1 -or [int]$pick -gt $engines.Count) {
        Write-Host 'Invalid choice.' -ForegroundColor Red
        return $null
    }

    $engineFile = $engines[[int]$pick - 1]
    return [PSCustomObject]@{
        FullName = $engineFile.FullName
        RelPath = $engineFile.FullName.Substring($RepoRoot.Length + 1)
        EngineId = Get-EngineNodeId $engineFile.FullName
        CatalogPartId = $null
        CatalogName = $null
    }
}

function Select-CatalogEngine {
    Write-Host ""
    Write-Host 'Paste a catalog link, for example:' -ForegroundColor DarkGray
    Write-Host "  $CatalogBaseUrl/parts/2531" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host '  C. Cancel' -ForegroundColor DarkGray
    $url = Read-Host 'Catalog URL or part number (C = cancel)'
    if (Test-CancelInput $url) { return 'cancel' }
    if ([string]::IsNullOrWhiteSpace($url)) {
        Write-Host 'Empty link.' -ForegroundColor Red
        return $null
    }

    try {
        return Import-CatalogEngine $url
    }
    catch {
        Write-Host $_.Exception.Message -ForegroundColor Red
        return $null
    }
}

function Invoke-EngineExport([PSCustomObject]$Selection) {
    $engineRel = $Selection.RelPath
    $suggestedId = $Selection.EngineId

    Write-Host ""
    Write-Host "File: $engineRel"
    if ($Selection.CatalogName) {
        Write-Host "Catalog: $($Selection.CatalogName)"
    }
    Write-Host "Detected ID: $suggestedId"

    $useId = Read-YesNoCancel 'Use this ID for WAV filenames?' $true
    if ($useId -eq 'cancel') { return 'cancel' }

    if ($useId -eq 'yes') {
        $engineId = $suggestedId
    } else {
        $engineId = Read-Host 'Enter engine-id (e.g. RB26DETT, C = cancel)'
        if (Test-CancelInput $engineId) { return 'cancel' }
        if ([string]::IsNullOrWhiteSpace($engineId)) {
            Write-Host 'Empty ID.' -ForegroundColor Red
            return $null
        }
    }

    $fullExport = Read-YesNoCancel 'Full export: 8 RPM tiers (16 WAV files)?' $true
    if ($fullExport -eq 'cancel') { return 'cancel' }
    $steps = if ($fullExport -eq 'yes') { 8 } else { 1 }

    $outputDir = Join-Path $RepoRoot ("out\" + $engineId)
    Write-Host ""
    Write-Host "Output: $outputDir"

    if ((Test-Path -LiteralPath $outputDir) -and (Get-ChildItem -LiteralPath $outputDir -ErrorAction SilentlyContinue)) {
        $overwrite = Read-YesNoCancel 'Output folder already exists. Overwrite?' $false
        if ($overwrite -eq 'cancel') { return 'cancel' }
        if ($overwrite -eq 'no') {
            Exit-Cancelled
        }
    }

    Write-Title 'Exporting...'
    Push-Location $RepoRoot
    try {
        $cliArgs = @(
            '--engine', $engineRel,
            '--engine-id', $engineId,
            '--output', $outputDir,
            '--steps', $steps,
            '--clip-duration', '1.0',
            '--warmup', '2.0',
            '--sample-rate', '44100',
            '--loop-mode', 'crossfade'
        )
        & $Cli @cliArgs

        if ($LASTEXITCODE -ne 0) {
            Write-Host "Export failed (exit code $LASTEXITCODE)." -ForegroundColor Red
            $errLog = Join-Path $RepoRoot 'error_log.log'
            if (Test-Path -LiteralPath $errLog) {
                Write-Host '--- error_log.log ---' -ForegroundColor DarkYellow
                Get-Content -LiteralPath $errLog -Tail 8 | ForEach-Object { Write-Host $_ -ForegroundColor DarkYellow }
            }
            return 'failed'
        }
    }
    finally {
        Pop-Location
    }

    Write-Host ""
    Write-Host 'Export complete.' -ForegroundColor Green

    $openFolder = Read-YesNoCancel 'Open output folder in Explorer to preview WAV files?' $true
    if ($openFolder -eq 'cancel') { return 'cancel' }
    if ($openFolder -eq 'yes') {
        explorer.exe $outputDir
    }

    return 'ok'
}

function Show-MainMenu {
    Write-Host ""
    Write-Host '  1. Local engine (assets\engines folder)' -ForegroundColor Green
    Write-Host '  2. Catalog link (catalog.engine-sim.parts)' -ForegroundColor Green
    Write-Host '  C. Quit' -ForegroundColor DarkGray
    Write-Host ""
    $choice = Read-Host 'Choice (1, 2, or C)'
    if (Test-CancelInput $choice) { return 'cancel' }
    switch ($choice) {
        '1' { return 'local' }
        '2' { return 'catalog' }
        default {
            Write-Host 'Invalid choice.' -ForegroundColor Red
            return $null
        }
    }
}

# --- Main ---

Write-Title 'engine-sim - NEODRIVE audio export'

if (-not (Test-Path -LiteralPath $Cli)) {
    Write-Host "Not found: $Cli" -ForegroundColor Red
    Write-Host 'Build first: cmake --build build --config Release' -ForegroundColor Yellow
    exit 1
}

while ($true) {
    $mode = Show-MainMenu
    if ($mode -eq 'cancel') { Exit-Cancelled 'Goodbye.' }

    $selection = $null
    if ($mode -eq 'local') {
        $selection = Select-LocalEngine
    }
    elseif ($mode -eq 'catalog') {
        $selection = Select-CatalogEngine
    }

    if ($selection -eq 'cancel') { continue }
    if ($null -eq $selection) { continue }

    $result = Invoke-EngineExport $selection
    if ($result -eq 'cancel') { continue }
    if ($result -eq 'failed') { continue }

    $again = Read-YesNoCancel 'Export another engine?' $false
    if ($again -eq 'cancel' -or $again -eq 'no') {
        Exit-Cancelled 'Goodbye.'
    }
}
