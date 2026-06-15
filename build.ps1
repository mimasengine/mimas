# DoomSRL build script.
# Runs SRL's make build via MSYS2 MINGW64.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build.ps1           # incremental
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Clean    # full rebuild
#
# SRL's shared.mk handles: compile, link, ISO (xorrisofs), CUE generation.
# CDDA music: place WAV/FLAC/OGG/MP3 files in cd/music/ and create a
# cd/music/tracklist file listing them in order (one per line, track 02+).
# SRL will convert them with sox and append them to the disc image.

param([switch]$Clean)

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$msys2 = "C:\msys64"
$bash  = "$msys2\usr\bin\bash.exe"

if (-not (Test-Path $bash)) {
    throw "MSYS2 not found at $msys2. Run SaturnRingLib\setup_compiler.bat first."
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
