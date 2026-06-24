# DoomSRL build script.
# Runs SRL's make build via MSYS2 MINGW64.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build.ps1                 # incremental
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Clean          # full rebuild
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Wad Doom2.wad  # swap the IWAD
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Repack         # + per-level repack
#
# -Wad <name> : pick an IWAD from wads_temoins/ and copy it to cd/data/DOOM1.WAD
#   (the fixed filename the Saturn loads from CD) before building. Accepts a bare
#   name with or without extension, case-insensitive: -Wad Doom1.WAD, -Wad doom2,
#   -Wad Plutonia. Omit it to build with whatever IWAD is already in cd/data/.
#
# -Repack : per-level repack (STREAMING_ANALYSIS.md §7.4/7.9-7.10). Generate the
#   per-map LZSS container cd/data/DOOM1.WAD -> cd/data/DOOMRP.DRP before the ISO
#   step, so the disc carries BOTH the full WAD (raw fallback) AND the repacked
#   blobs. The Step-3 loader auto-detects DOOMRP.DRP (magic "DRP1" + matching
#   dir_crc32) and falls back to raw full-WAD streaming when it is absent/mismatched
#   -- both stay loadable. Needs python on PATH; skipped when the .DRP is already
#   up to date. Omit -Repack for a raw disc (default, unchanged).
#
# SRL's shared.mk handles: compile, link, ISO (xorrisofs), CUE generation.
# CDDA music: place WAV/FLAC/OGG/MP3 files in cd/music/ and create a
# cd/music/tracklist file listing them in order (one per line, track 02+).
# SRL will convert them with sox and append them to the disc image.

param(
    [switch]$Clean,
    [string]$Wad,
    [switch]$Repack
)

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$msys2 = "C:\msys64"
$bash  = "$msys2\usr\bin\bash.exe"

if (-not (Test-Path $bash)) {
    throw "MSYS2 not found at $msys2. Run SaturnRingLib\setup_compiler.bat first."
}

# Optional IWAD swap: copy the chosen WAD from wads_temoins/ onto the fixed
# cd/data/DOOM1.WAD the Saturn loads from CD. The WAD is bundled raw (no strip),
# matching the current build (cd/data/DOOM1.WAD is byte-identical to a renamed
# wads_temoins copy). Maps are picked at boot with SAT_WARP_MAP, not here.
if ($Wad) {
    $wadDir = Join-Path $root "wads_temoins"
    if (-not (Test-Path $wadDir)) { throw "wads_temoins/ not found at $wadDir" }

    $src = $null
    foreach ($cand in @($Wad, "$Wad.wad", "$Wad.WAD")) {
        $p = Join-Path $wadDir $cand
        if (Test-Path $p) { $src = (Get-Item $p).FullName; break }   # Windows FS is case-insensitive
    }
    if (-not $src) {
        $match = Get-ChildItem $wadDir -File -Filter *.wad |
                 Where-Object { $_.Name -ieq $Wad -or $_.BaseName -ieq $Wad } |
                 Select-Object -First 1
        if ($match) { $src = $match.FullName }
    }
    if (-not $src) {
        $avail = (Get-ChildItem $wadDir -File -Filter *.wad | ForEach-Object Name) -join ', '
        throw "WAD '$Wad' not found in $wadDir. Available: $avail"
    }

    $dst = Join-Path $root "cd\data\DOOM1.WAD"
    Copy-Item $src $dst -Force
    Write-Host "IWAD: $([System.IO.Path]::GetFileName($src)) -> cd/data/DOOM1.WAD ($('{0:N0}' -f (Get-Item $dst).Length) bytes)"
}

# Per-level repack: emit cd/data/DOOMRP.DRP before the ISO step (runs after any -Wad
# swap so it repacks the IWAD that will actually ship). The tool's --if-stale compares
# the .DRP header (n_lumps + dir_crc32) to the WAD and regenerates only on a real
# mismatch -- robust to a WAD SWAP (which file mtime alone misses).
if ($Repack) {
    $wadFile = Join-Path $root "cd\data\DOOM1.WAD"
    $drpFile = Join-Path $root "cd\data\DOOMRP.DRP"
    $infoFile = Join-Path $root "core\info.c"
    $toolFile = Join-Path $root "tools\repack_wad.py"
    if (-not (Test-Path $wadFile)) { throw "-Repack: $wadFile not found (build/copy the IWAD first)" }
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if (-not $py) { $py = (Get-Command py -ErrorAction SilentlyContinue).Source }
    if (-not $py) { throw "-Repack: python not found on PATH (needed to build the .DRP)" }

    Write-Host "Repack: ensuring cd/data/DOOMRP.DRP matches the IWAD..."
    & $py $toolFile $wadFile $infoFile "--emit=$drpFile" "--if-stale"
    if ($LASTEXITCODE -ne 0) { throw "repack_wad.py failed (exit $LASTEXITCODE)" }
    Write-Host "Repack OK ($('{0:N0}' -f (Get-Item $drpFile).Length) bytes)"
}

function ConvertTo-Msys2Path([string]$p) {
    $p = $p.Replace('\','/')
    if ($p -match '^([A-Za-z]):(.*)') { return '/' + $Matches[1].ToLower() + $Matches[2] }
    return $p
}

function Invoke-Msys2([string]$cmd) {
    # sh2eb-elf-gcc lives in SaturnRingLib/Compiler/sh2eb-elf/bin (extracted by setup_compiler.bat)
    $compilerBin = ConvertTo-Msys2Path (Join-Path $root "SaturnRingLib\Compiler\sh2eb-elf\bin")
    & $bash --login -c "export MSYSTEM=MINGW64; source /etc/profile 2>/dev/null; export PATH='$compilerBin':`$PATH; $cmd"
    if ($LASTEXITCODE -ne 0) { throw "MSYS2 command failed (exit $LASTEXITCODE)" }
}

# Detect CDDA music files
$musicDir  = Join-Path $root "cd\music"
$trackList = Join-Path $musicDir "tracklist"
$cdda      = (Test-Path $trackList) -or
             ((Test-Path $musicDir) -and (Get-ChildItem $musicDir -Include "*.wav","*.mp3","*.flac","*.ogg" -Recurse -ErrorAction SilentlyContinue).Count -gt 0)

# Handle optional external music/ directory (compatibility with SaturnDoom layout)
$extMusic = Join-Path $root "music"
if (-not $cdda -and (Test-Path $extMusic)) {
    $wavs = Get-ChildItem -Path $extMusic -Filter "track_*.wav" |
            Where-Object { $_.BaseName -match '^track_\d+$' } |
            Sort-Object { [int]($_.BaseName -replace 'track_','') }
    if ($wavs.Count -gt 0) {
        Write-Host "Copying $($wavs.Count) WAV tracks to cd/music/..."
        New-Item -ItemType Directory -Path $musicDir -Force | Out-Null
        $lines = @()
        foreach ($w in $wavs) {
            Copy-Item $w.FullName (Join-Path $musicDir $w.Name) -Force
            $lines += $w.Name
        }
        $lines | Out-File -FilePath $trackList -Encoding ascii -NoNewline
        Add-Content $trackList ""
        $cdda = $true
    }
}

$rootMsys = ConvertTo-Msys2Path $root

Push-Location $root
try {
    if ($Clean) {
        Write-Host "Cleaning..."
        Invoke-Msys2 "cd '$rootMsys' && make clean"
    }

    $makeTarget = "build"
    $makeArgs   = ""
    if ($cdda) { $makeArgs = "CDDA_MUSIC=1" }

    Write-Host "Building DoomSRL$(if ($cdda) {' (CDDA)'})..."
    # Touch the file carrying the on-screen build stamp (dg_saturn.cxx -> row 18
    # "b:<__TIME__>") so every build recompiles it with a fresh timestamp -- lets
    # you confirm on hardware that you flashed THIS build even when only core/
    # files changed (which otherwise leaves dg_saturn.o, and its __TIME__, stale).
    Invoke-Msys2 "cd '$rootMsys' && touch src/dg_saturn.cxx && make $makeTarget $makeArgs"

    # SRL outputs to build/DoomSRL.bin + build/DoomSRL.cue
    $binPath = Join-Path $root "build\DoomSRL.bin"
    $cuePath = Join-Path $root "build\DoomSRL.cue"
    if (Test-Path $binPath) {
        $bin = Get-Item $binPath
        Write-Output "OK  bin=$([string]::Format('{0:N0}', $bin.Length)) bytes"
        if (Test-Path $cuePath) { Write-Output "CUE: $cuePath" }
    } else {
        Write-Warning "build/DoomSRL.bin not found -- check make output above"
    }
}
finally { Pop-Location }
