# Interactive NEODRIVE export for engine-sim (Windows).
# Usage: powershell -File scripts\export-engine-audio.ps1
#    or: .\export-audio.cmd
#
# Navigation: use the Up/Down arrow keys + Enter to pick options.
# You only type text when entering a catalog part number / URL or a custom id.

$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Cli = Join-Path $RepoRoot "build\Release\engine-sim-cli.exe"
$CatalogBaseUrl = "https://catalog.engine-sim.parts"
$Utf8NoBom = New-Object System.Text.UTF8Encoding $false

# ---------------------------------------------------------------------------
# UI helpers
# ---------------------------------------------------------------------------

function Write-Banner {
    Write-Host ""
    Write-Host "  +----------------------------------------------+" -ForegroundColor Cyan
    Write-Host "  |    engine-sim  .  NEODRIVE audio export       |" -ForegroundColor Cyan
    Write-Host "  +----------------------------------------------+" -ForegroundColor Cyan
}

function Write-Title([string]$Text) {
    Write-Host ""
    Write-Host "  == $Text ==" -ForegroundColor Cyan
}

function Write-Field([string]$Label, [string]$Value) {
    Write-Host ("  {0,-14}" -f "$Label :") -ForegroundColor DarkGray -NoNewline
    Write-Host $Value -ForegroundColor White
}

# Arrow-key menu. Returns the selected 0-based index, or -1 if the user
# pressed Escape. Falls back to a numbered prompt when input is redirected.
function Read-Menu {
    param(
        [Parameter(Mandatory)][string[]]$Options,
        [string]$Title = "",
        [int]$Default = 0
    )

    if ($Title) { Write-Host ""; Write-Host "  $Title" -ForegroundColor Cyan }

    if ([Console]::IsInputRedirected) {
        for ($i = 0; $i -lt $Options.Count; $i++) {
            Write-Host ("    {0}. {1}" -f ($i + 1), $Options[$i])
        }
        while ($true) {
            $a = Read-Host "  Choice (number)"
            if ($a -match '^\d+$' -and [int]$a -ge 1 -and [int]$a -le $Options.Count) {
                return [int]$a - 1
            }
        }
    }

    $rui = $Host.UI.RawUI
    $sel = [Math]::Max(0, [Math]::Min($Default, $Options.Count - 1))
    $width = [Math]::Max(24, $rui.BufferSize.Width - 1)

    # Print the rows once so the buffer scrolls if we are near the bottom, then
    # anchor above them. Redrawing in place via RawUI is reliable across hosts,
    # unlike [Console]::SetCursorPosition which can no-op and stack the menus.
    for ($i = 0; $i -lt $Options.Count; $i++) { Write-Host "" }
    $anchor = $rui.CursorPosition
    $anchor.X = 0
    $anchor.Y = $anchor.Y - $Options.Count

    $draw = {
        try { $rui.CursorPosition = $anchor } catch {}
        for ($i = 0; $i -lt $Options.Count; $i++) {
            $prefix = if ($i -eq $sel) { " > " } else { "   " }
            $line = $prefix + $Options[$i]
            if ($line.Length -gt $width) { $line = $line.Substring(0, $width) }
            $line = $line.PadRight($width)
            if ($i -eq $sel) {
                Write-Host $line -ForegroundColor Black -BackgroundColor Cyan
            }
            else {
                Write-Host $line -ForegroundColor Gray
            }
        }
    }

    try { [Console]::CursorVisible = $false } catch {}
    & $draw
    while ($true) {
        $key = [Console]::ReadKey($true)
        switch ($key.Key) {
            'UpArrow'   { $sel = ($sel - 1 + $Options.Count) % $Options.Count; & $draw }
            'DownArrow' { $sel = ($sel + 1) % $Options.Count; & $draw }
            'Enter'     { try { [Console]::CursorVisible = $true } catch {}; return $sel }
            'Escape'    { try { [Console]::CursorVisible = $true } catch {}; return -1 }
        }
    }
}

# Run the CLI, stream its stdout, and turn "PROGRESS done total" lines into a
# live progress bar. Returns the process exit code. Runs from the repo root so
# relative --engine paths resolve.
function Invoke-CliWithProgress {
    param([Parameter(Mandatory)][string[]]$CliArgs, [string]$Activity = "Working")
    $code = 0
    Push-Location $RepoRoot
    try {
        & $Cli @CliArgs | ForEach-Object {
            $line = "$_"
            if ($line -match '^PROGRESS\s+(\d+)\s+(\d+)') {
                $done = [int]$Matches[1]; $total = [int]$Matches[2]
                $pct = if ($total -gt 0) { [int](100 * $done / $total) } else { 0 }
                Write-Progress -Activity $Activity -Status "$pct%  ($done / $total)" -PercentComplete $pct
            }
            elseif ($line -match '\S') {
                Write-Host "  $line" -ForegroundColor DarkGray
            }
        }
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
        Write-Progress -Activity $Activity -Completed
    }
    return $code
}

# Yes / No / Cancel as an arrow menu. Returns 'yes' | 'no' | 'cancel'.
function Confirm-Menu([string]$Prompt, [bool]$DefaultYes = $true) {
    $default = if ($DefaultYes) { 0 } else { 1 }
    $i = Read-Menu -Title $Prompt -Options @("Yes", "No") -Default $default
    switch ($i) {
        0       { return 'yes' }
        1       { return 'no' }
        default { return 'cancel' }
    }
}

function Exit-Cancelled([string]$Message = 'Cancelled.') {
    Write-Host ""
    Write-Host "  $Message" -ForegroundColor Yellow
    exit 0
}

function Test-CancelInput([string]$Value) {
    return ($Value -match '^(c|cancel|q|quit)$')
}

# ---------------------------------------------------------------------------
# Engine script parsing
# ---------------------------------------------------------------------------

function Count-BraceDepth([string]$Line) {
    $open = ([regex]::Matches($Line, '\{')).Count
    $close = ([regex]::Matches($Line, '\}')).Count
    return $open - $close
}

function Count-ParenDepth([string]$Line) {
    $open = ([regex]::Matches($Line, '\(')).Count
    $close = ([regex]::Matches($Line, '\)')).Count
    return $open - $close
}

function Read-NodeBlock([string[]]$Lines, [int]$StartIndex) {
    $block = New-Object System.Collections.Generic.List[string]
    $block.Add($Lines[$StartIndex])
    $depth = Count-BraceDepth $Lines[$StartIndex]
    $i = $StartIndex + 1

    if ($depth -eq 0) {
        while ($i -lt $Lines.Count) {
            $block.Add($Lines[$i])
            $depth += Count-BraceDepth $Lines[$i]
            $i++
            if ($depth -le 0) { break }
        }
    }
    else {
        while ($i -lt $Lines.Count -and $depth -gt 0) {
            $block.Add($Lines[$i])
            $depth += Count-BraceDepth $Lines[$i]
            $i++
        }
    }

    return @{
        Lines     = $block
        NextIndex = $i
        Text      = ($block -join "`n")
    }
}

function Get-EngineNodeId([string]$MrPath) {
    $lines = Get-Content -LiteralPath $MrPath
    $engineNodeCandidates = New-Object System.Collections.Generic.List[string]

    for ($i = 0; $i -lt $lines.Count; $i++) {
        # Capture only the identifier: `node foo{` (no space) must not swallow the brace.
        if ($lines[$i] -notmatch '^\s*(private|public)\s+node\s+([A-Za-z_]\w*)') { continue }

        $node = $Matches[2]
        if ($node -eq 'main') { continue }

        $block = Read-NodeBlock $lines $i
        $i = $block.NextIndex - 1

        if ($block.Text -match 'alias\s+output\s+__out:\s*engine\b') {
            return $node
        }
        if ($block.Text -match '(?m)^\s*(engine\s+engine|wankel_engine\s+engine)\s*\(') {
            [void]$engineNodeCandidates.Add($node)
        }
    }

    if ($engineNodeCandidates.Count -gt 0) {
        return $engineNodeCandidates[0]
    }

    return [System.IO.Path]::GetFileNameWithoutExtension($MrPath)
}

# Rough cylinder layout, e.g. "v8", "i6", "i4", "v10". Used to make ids readable.
function Get-EngineLayout([string]$MrPath) {
    $text = Get-Content -Raw -LiteralPath $MrPath
    $cyl = ([regex]::Matches($text, '\.add_cylinder\s*\(')).Count
    if ($cyl -le 0) { return '' }
    $banks = ([regex]::Matches($text, '(?m)^\s*cylinder_bank\s+\w+\s*\(')).Count
    if ($banks -ge 2) { return "v$cyl" }
    return "i$cyl"
}

# Build a readable, valid node/file id from a catalog name.
# "2010 BMW M5 E60 S85 V10"      -> "bmw_m5_e60_s85_v10"
# "Ferrari F40  /  2.9L TT V8"   -> "ferrari_f40_2_9l_tt_v8"
# "V0.1.14A Chevrolet Corvette"  -> "chevrolet_corvette"   (version prefix dropped)
function ConvertTo-EngineSlug([string]$Name) {
    if ([string]::IsNullOrWhiteSpace($Name)) { return $null }
    $s = $Name.ToLower()
    $s = $s -replace '[^a-z0-9]+', '_'            # non-alphanumeric -> underscore
    $s = $s -replace '^v\d+(_\d+[a-z]?)+_', ''    # drop leading version tokens (v0_1_14a_)
    $s = $s -replace '_(19|20)\d\d(?=_|$)', ''    # drop 4-digit years
    $s = $s -replace '^[_0-9]+', ''               # identifiers can't start with a digit
    $s = $s -replace '_+', '_'
    $s = $s.Trim('_')
    if ([string]::IsNullOrWhiteSpace($s)) { return $null }
    return $s
}

# Append a layout suffix (_v8, _i6, ...) unless the id already conveys it.
function Add-LayoutSuffix([string]$MrPath, [string]$Id) {
    $lay = Get-EngineLayout $MrPath
    if ($lay -and ($Id -notmatch "(^|_)$lay(_|$)")) {
        return "${Id}_$lay"
    }
    return $Id
}

# Rename only the engine node's declaration. After run()/main() are stripped the
# engine node is never *called* in the script (a generated wrapper calls it), so
# renaming the declaration is enough and avoids touching unrelated identifiers.
function Rename-EngineNodeInScript([string]$Script, [string]$OldName, [string]$NewName) {
    if ($OldName -eq $NewName) { return $Script }
    $pattern = "(?m)^(\s*(?:public|private)\s+node\s+)$([regex]::Escape($OldName))\b"
    return [regex]::Replace($Script, $pattern, "`${1}$NewName")
}

# ---------------------------------------------------------------------------
# Catalog import + repair
# ---------------------------------------------------------------------------

function Test-CatalogEngineSupported([string]$Script) {
    if ($Script -match '\bwankel_engine\b') {
        return @{
            Supported = $false
            Reason    = @(
                'Rotary / Wankel engines are not supported by this engine-sim build (no wankel_engine in the compiler).'
                'Use piston engines instead.'
            ) -join ' '
        }
    }
    return @{ Supported = $true; Reason = '' }
}

function Should-SkipCatalogNode([string]$NodeName, [string]$BlockText) {
    if ($NodeName -eq 'main') { return $true }
    if ($BlockText -match '\bvehicle\s*\(') { return $true }
    if ($BlockText -match '\btransmission\s*\(') { return $true }
    if ($BlockText -match '\brun\s*\(') { return $true }
    return $false
}

function Repair-CatalogScript([string]$Script) {
    # Catalog scripts often target a newer engine-sim (vehicle/run/convolution).
    # Strip GUI-only nodes and unsupported engine fields for headless CLI export.
    $lines = $Script -split "`r?`n"
    $result = New-Object System.Collections.Generic.List[string]
    $i = 0

    while ($i -lt $lines.Count) {
        $line = $lines[$i]

        if ($line -match '^\s*(private|public)\s+node\s+([A-Za-z_]\w*)') {
            $nodeName = $Matches[2]
            $block = Read-NodeBlock $lines $i
            $i = $block.NextIndex

            if (-not (Should-SkipCatalogNode $nodeName $block.Text)) {
                foreach ($bl in $block.Lines) {
                    if ($bl -match '^\s*(convolution|block_temperature|max_brake_force)\s*:') { continue }
                    $result.Add($bl)
                }
            }
            continue
        }

        # Strip top-level run(...) / main(...) calls, which may span several lines.
        # They reference the vehicle/transmission nodes we removed; the wrapper
        # instantiates the engine itself, so any leftover call breaks compilation.
        if ($line -match '^\s*(run|main)\b\s*\(') {
            $depth = Count-ParenDepth $line
            $i++
            while ($i -lt $lines.Count -and $depth -gt 0) {
                $depth += Count-ParenDepth $lines[$i]
                $i++
            }
            continue
        }

        if ($line -match '^\s*(convolution|block_temperature|max_brake_force)\s*:') { $i++; continue }
        $result.Add($line)
        $i++
    }

    return (($result -join "`n").TrimEnd() + "`n")
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
    Write-Host "  Downloading from catalog (part #$partId)..." -ForegroundColor Cyan

    $part = Invoke-RestMethod -Uri $apiUrl -UseBasicParsing
    if (-not $part.script) {
        throw "This catalog entry has no exportable engine script."
    }

    $support = Test-CatalogEngineSupported $part.script
    if (-not $support.Supported) {
        throw $support.Reason
    }

    $scriptName = if ($part.script_name) { $part.script_name } else { "part_$partId.mr" }
    $catalogDir = Join-Path $RepoRoot "assets\engines\catalog\part_$partId"
    New-Item -ItemType Directory -Force -Path $catalogDir | Out-Null

    $mrPath = Join-Path $catalogDir $scriptName
    $repaired = Repair-CatalogScript $part.script
    [System.IO.File]::WriteAllText($mrPath, $repaired, $Utf8NoBom)
    Write-Host "  Script adapted for CLI export (vehicle/run stripped)." -ForegroundColor DarkGray

    $nodeId = Get-EngineNodeId $mrPath

    # Readable id from the catalog name + detected layout (e.g. bmw_m5_e60_s85_v10).
    $slug = ConvertTo-EngineSlug $part.name
    $suggestedId = if ($slug) { $slug } else { $nodeId }
    $suggestedId = Add-LayoutSuffix $mrPath $suggestedId

    $rel = $mrPath.Substring($RepoRoot.Length + 1)
    Write-Host "  Saved: $rel" -ForegroundColor Green

    return [PSCustomObject]@{
        FullName      = $mrPath
        RelPath       = $rel
        NodeId        = $nodeId
        EngineId      = $suggestedId
        CatalogPartId = $partId
        CatalogName   = $part.name
    }
}

# ---------------------------------------------------------------------------
# Engine selection
# ---------------------------------------------------------------------------

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

function Select-LocalEngine {
    $engines = @(Get-EngineScripts)
    if ($engines.Count -eq 0) {
        Write-Host "  No .mr files found under assets\engines" -ForegroundColor Red
        return $null
    }

    $labels = @()
    foreach ($e in $engines) {
        $rel = $e.FullName.Substring($RepoRoot.Length + 1)
        $labels += $rel
    }
    $labels += "<- Back"

    $pick = Read-Menu -Title "Pick a local engine:" -Options $labels
    if ($pick -lt 0 -or $pick -eq $engines.Count) { return 'cancel' }

    $engineFile = $engines[$pick]
    $nodeId = Get-EngineNodeId $engineFile.FullName
    $suggested = Add-LayoutSuffix $engineFile.FullName (ConvertTo-EngineSlug $nodeId)

    return [PSCustomObject]@{
        FullName      = $engineFile.FullName
        RelPath       = $engineFile.FullName.Substring($RepoRoot.Length + 1)
        NodeId        = $nodeId
        EngineId      = if ($suggested) { $suggested } else { $nodeId }
        CatalogPartId = $null
        CatalogName   = $null
    }
}

function Select-CatalogEngine {
    Write-Host ""
    Write-Host "  Paste a catalog link or part number, for example:" -ForegroundColor DarkGray
    Write-Host "    $CatalogBaseUrl/parts/2531   (or just: 2531)" -ForegroundColor DarkGray
    Write-Host "    Leave empty / type C to cancel." -ForegroundColor DarkGray
    $url = Read-Host "  Catalog URL or part number"
    if ([string]::IsNullOrWhiteSpace($url) -or (Test-CancelInput $url)) { return 'cancel' }

    try {
        return Import-CatalogEngine $url
    }
    catch {
        Write-Host "  $($_.Exception.Message)" -ForegroundColor Red
        return $null
    }
}

# ---------------------------------------------------------------------------
# Export + preview
# ---------------------------------------------------------------------------

# Ensure the file we hand to the CLI contains a node named exactly $Id.
# Returns the repo-relative path to use, plus the staged file to clean up (if any).
function Resolve-ExportScript([PSCustomObject]$Selection, [string]$Id) {
    if ($Id -eq $Selection.NodeId) {
        return [PSCustomObject]@{ RelPath = $Selection.RelPath; Staged = $null }
    }

    $text = Get-Content -Raw -LiteralPath $Selection.FullName
    $renamed = Rename-EngineNodeInScript $text $Selection.NodeId $Id
    if ($renamed -eq $text) {
        # Could not rename (node not found as a declaration) - fall back to raw file.
        return [PSCustomObject]@{ RelPath = $Selection.RelPath; Staged = $null }
    }

    $dir = Split-Path -Parent $Selection.FullName
    $stagedFull = Join-Path $dir "_staged_$Id.mr"
    [System.IO.File]::WriteAllText($stagedFull, $renamed, $Utf8NoBom)
    return [PSCustomObject]@{
        RelPath = $stagedFull.Substring($RepoRoot.Length + 1)
        Staged  = $stagedFull
    }
}

function Play-Wav([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { Write-Host "  No preview file was produced." -ForegroundColor Red; return }
    Write-Host "  Now playing: $(Split-Path $Path -Leaf)" -ForegroundColor DarkGray
    try {
        $player = New-Object System.Media.SoundPlayer $Path
        $player.PlaySync()
        $player.Dispose()
    }
    catch {
        Write-Host "  Could not play automatically - opening the folder instead." -ForegroundColor Yellow
        explorer.exe (Split-Path -Parent $Path)
    }
}

# Render a 10 s idle->redline->idle rev sweep and let the user audition it
# before committing to the full export. Falls back to a short steady clip when
# the CLI predates the --sweep flag.
function Invoke-SoundPreview([string]$ExportRel, [string]$Id) {
    $previewDir = Join-Path $RepoRoot "out\.preview"
    if (Test-Path -LiteralPath $previewDir) { Remove-Item -LiteralPath $previewDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $previewDir | Out-Null

    Write-Title "Rendering 10 s rev-sweep preview (idle -> redline -> idle)"
    $code = Invoke-CliWithProgress @(
        '--engine', $ExportRel, '--engine-id', $Id, '--output', $previewDir,
        '--sweep', '10', '--sample-rate', '44100'
    ) 'Rendering preview'

    $wav = Join-Path $previewDir ($Id + "_sweep.wav")

    if ($code -ne 0 -or -not (Test-Path -LiteralPath $wav)) {
        Write-Host "  Sweep unavailable (rebuild the CLI for it) - using a short steady clip." -ForegroundColor DarkYellow
        Remove-Item -LiteralPath $previewDir -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force -Path $previewDir | Out-Null
        $code = Invoke-CliWithProgress @(
            '--engine', $ExportRel, '--engine-id', $Id, '--output', $previewDir,
            '--rpms', '3000', '--clip-duration', '0.7', '--warmup', '1.0',
            '--sample-rate', '44100', '--loop-mode', 'crossfade'
        ) 'Rendering preview'
        $wav = Join-Path $previewDir ($Id + "_3000_on.wav")
        if (-not (Test-Path -LiteralPath $wav)) {
            $first = Get-ChildItem -LiteralPath $previewDir -Filter "*.wav" -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($first) { $wav = $first.FullName }
        }
    }

    if (-not (Test-Path -LiteralPath $wav)) {
        Write-Host "  Preview failed." -ForegroundColor Red
        Remove-Item -LiteralPath $previewDir -Recurse -Force -ErrorAction SilentlyContinue
        return
    }

    while ($true) {
        Play-Wav $wav
        $c = Read-Menu -Title "Preview:" -Options @("Play again", "Continue")
        if ($c -ne 0) { break }
    }

    Remove-Item -LiteralPath $previewDir -Recurse -Force -ErrorAction SilentlyContinue
}

function Invoke-EngineExport([PSCustomObject]$Selection) {
    Write-Title "Engine"
    Write-Field "File" $Selection.RelPath
    if ($Selection.CatalogName) { Write-Field "Catalog" $Selection.CatalogName }
    Write-Field "Detected node" $Selection.NodeId
    Write-Field "Suggested id" $Selection.EngineId

    # --- choose id ---
    $useId = Read-Menu -Title "File-name id for the WAV set:" -Options @(
        "Use suggested  ($($Selection.EngineId))",
        "Type a custom id",
        "Cancel"
    )
    if ($useId -lt 0 -or $useId -eq 2) { return 'cancel' }

    if ($useId -eq 0) {
        $engineId = $Selection.EngineId
    }
    else {
        $raw = Read-Host "  Enter engine-id (lowercase snake_case, e.g. merc_amg_m156_v8)"
        if ([string]::IsNullOrWhiteSpace($raw) -or (Test-CancelInput $raw)) { return 'cancel' }
        $engineId = ($raw.Trim() -replace '[^A-Za-z0-9_]+', '_' -replace '_+', '_').Trim('_').ToLower()
        if ([string]::IsNullOrWhiteSpace($engineId)) { Write-Host "  Empty id." -ForegroundColor Red; return $null }
    }

    # --- make sure the script exposes a node with that id ---
    $resolved = Resolve-ExportScript $Selection $engineId
    $exportRel = $resolved.RelPath

    try {
        # --- optional preview ---
        $step = Read-Menu -Title "Before the full export:" -Options @(
            "Preview the sound first",
            "Skip preview, export now",
            "Cancel"
        )
        if ($step -lt 0 -or $step -eq 2) { return 'cancel' }
        if ($step -eq 0) {
            Invoke-SoundPreview $exportRel $engineId
            $go = Confirm-Menu "Export the full set now?" $true
            if ($go -ne 'yes') { return 'cancel' }
        }

        # --- scope ---
        $scope = Read-Menu -Title "Export scope:" -Options @(
            "Full  - 8 RPM tiers (16 WAV files)",
            "Quick - 1 tier (2 WAV files)",
            "Cancel"
        )
        if ($scope -lt 0 -or $scope -eq 2) { return 'cancel' }
        $steps = if ($scope -eq 0) { 8 } else { 1 }

        $outputDir = Join-Path $RepoRoot ("out\" + $engineId)
        Write-Host ""
        Write-Field "Output" $outputDir

        if ((Test-Path -LiteralPath $outputDir) -and (Get-ChildItem -LiteralPath $outputDir -ErrorAction SilentlyContinue)) {
            $overwrite = Confirm-Menu "Output folder already exists. Overwrite?" $false
            if ($overwrite -ne 'yes') { return 'cancel' }
        }

        Write-Title "Exporting..."
        $code = Invoke-CliWithProgress @(
            '--engine', $exportRel,
            '--engine-id', $engineId,
            '--output', $outputDir,
            '--steps', "$steps",
            '--clip-duration', '1.0', '--warmup', '2.0', '--sample-rate', '44100',
            '--loop-mode', 'crossfade'
        ) "Exporting $engineId"

        if ($code -ne 0) {
            Write-Host "  Export failed (exit code $code)." -ForegroundColor Red
            $errLog = Join-Path $RepoRoot 'error_log.log'
            if (Test-Path -LiteralPath $errLog) {
                Write-Host "  --- error_log.log ---" -ForegroundColor DarkYellow
                Get-Content -LiteralPath $errLog -Tail 8 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkYellow }
            }
            return 'failed'
        }
    }
    finally {
        if ($resolved.Staged -and (Test-Path -LiteralPath $resolved.Staged)) {
            Remove-Item -LiteralPath $resolved.Staged -Force -ErrorAction SilentlyContinue
        }
    }

    Write-Host ""
    Write-Host "  Export complete -> out\$engineId" -ForegroundColor Green

    $open = Confirm-Menu "Open the output folder in Explorer?" $true
    if ($open -eq 'yes') { explorer.exe $outputDir }

    return 'ok'
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

Write-Banner

if (-not (Test-Path -LiteralPath $Cli)) {
    Write-Host "  Not found: $Cli" -ForegroundColor Red
    Write-Host "  Build first: cmake --build build --config Release" -ForegroundColor Yellow
    exit 1
}

while ($true) {
    $choice = Read-Menu -Title "What do you want to export?" -Options @(
        "Local engine  (assets\engines folder)",
        "Catalog link  (catalog.engine-sim.parts)",
        "Quit"
    )
    if ($choice -lt 0 -or $choice -eq 2) { Exit-Cancelled 'Goodbye.' }

    $selection = if ($choice -eq 0) { Select-LocalEngine } else { Select-CatalogEngine }

    if ($selection -eq 'cancel' -or $null -eq $selection) { continue }

    $result = Invoke-EngineExport $selection
    if ($result -ne 'ok') { continue }

    $again = Confirm-Menu "Export another engine?" $false
    if ($again -ne 'yes') { Exit-Cancelled 'Goodbye.' }
}
