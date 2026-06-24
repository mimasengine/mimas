# Launch Ymir with DoomSRL.
# Usage: powershell -ExecutionPolicy Bypass -File run_ymir.ps1 [-Build] [-Paused] [-Debug] [-Wad <name>]
#
# -Build  : rebuild the disc image before launching
# -Paused : start Ymir paused (useful with -Debug)
# -Debug  : enable Ymir debug tracing (F11 to toggle in-session)
# -Wad    : IWAD from wads_temoins/ to bundle (forwarded to build.ps1; needs -Build)

param(
    [switch]$Build,
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
    if ($Wad) { $buildArgs += @("-Wad", $Wad) }
    powershell @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed (exit $LASTEXITCODE)" }
}
elseif ($Wad) {
    Write-Warning "-Wad has no effect without -Build (the existing disc image is reused as-is)."
}

$cue = Join-Path $root "build\DoomSRL.cue"
if (-not (Test-Path $cue)) {
    Write-Error "build/DoomSRL.cue not found -- build first: powershell -File build.ps1"
    exit 1
}

$launchArgs = @("-d", $cue, "-p", $profile)
if ($Paused) { $launchArgs += "-P" }
if ($Debug)  { $launchArgs += "-D" }

Write-Host "Launching Ymir..."
Write-Host "  Disc   : $cue"
Write-Host "  Profile: $profile"

Stop-Process -Name ymir-sdl3 -Force -ErrorAction SilentlyContinue
Start-Process -FilePath $ymir -ArgumentList $launchArgs
