# Launch Ymir with Mimas.
# Usage: powershell -ExecutionPolicy Bypass -File run_ymir.ps1 [-Build] [-Repack] [-Paused] [-Debug] [-Wad <name>]
#
# -Build  : rebuild the disc image before launching
# -Repack : (with -Build) also build the per-map .DRP (forwarded to build.ps1; big WADs)
# -Cdda   : (with -Build) multi-file CDDA disc -- small data .bin + WAV tracks referenced
#           separately (fast build/mount, no giant interleaved .bin).  music/track_NN.wav.
# -Paused : start Ymir paused (useful with -Debug)
# -Debug  : enable Ymir debug tracing (F11 to toggle in-session)
# -Wad    : IWAD from wads_temoins/ to use.
#           * with -Build : swap + build, and the disc is STASHED to build/wads/<name>/.
#           * WITHOUT -Build : launch the previously-stashed build/wads/<name>/ disc
#             instantly (no rebuild/repack) -- the per-WAD "latest build" cache.

param(
    [switch]$Build,
    [switch]$Repack,
    [switch]$Cdda,
    [switch]$Paused,
    [switch]$Debug,
    [string]$Wad
)

$ErrorActionPreference = "Stop"
$root    = Split-Path -Parent $MyInvocation.MyCommand.Path
$ymir    = "C:\Users\pcico\Games\Sega Saturn\Ymir\ymir-sdl3.exe"
$profile = Join-Path $root "ymir_profile"

if (-not (Test-Path $ymir)) {
    Write-Error "Ymir not found at: $ymir"
    exit 1
}

if ($Build) {
    Write-Host "Building..."
    $buildArgs = @("-ExecutionPolicy", "Bypass", "-File", (Join-Path $root "build.ps1"))
    if ($Wad)    { $buildArgs += @("-Wad", $Wad) }
    if ($Repack) { $buildArgs += "-Repack" }
    if ($Cdda)   { $buildArgs += "-Cdda" }
    powershell @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed (exit $LASTEXITCODE)" }
}

# Choose the disc.  Always launch the per-WAD STASH when -Wad is given (with OR without
# -Build): -Build just populated it, and for -Cdda the audio WAVs live ONLY next to the
# stashed .cue (Ymir loads referenced files from the .cue's own directory).  No -Wad ->
# the fresh build/ output.
if ($Wad) {
    $wadName = [System.IO.Path]::GetFileNameWithoutExtension($Wad)
    $cue = Join-Path $root "build\wads\$wadName\Mimas.cue"
    if (-not (Test-Path $cue)) {
        Write-Error "No stashed build for '$Wad' (build/wads/$wadName/) -- build it once: run_ymir.ps1 -Build -Wad $Wad"
        exit 1
    }
    Write-Host "Using stashed build: build/wads/$wadName/"
}
else {
    $cue = Join-Path $root "build\Mimas.cue"
    if (-not (Test-Path $cue)) {
        Write-Error "build/Mimas.cue not found -- build first: powershell -File build.ps1"
        exit 1
    }
}

$launchArgs = @("-d", $cue, "-p", $profile)
if ($Paused) { $launchArgs += "-P" }
if ($Debug)  { $launchArgs += "-D" }

Write-Host "Launching Ymir..."
Write-Host "  Disc   : $cue"
Write-Host "  Profile: $profile"

Stop-Process -Name ymir-sdl3 -Force -ErrorAction SilentlyContinue
Start-Process -FilePath $ymir -ArgumentList $launchArgs
