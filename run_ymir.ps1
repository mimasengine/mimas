# Launch Ymir with DoomSRL.
# Usage: powershell -ExecutionPolicy Bypass -File run_ymir.ps1 [-Build] [-Paused] [-Debug]
#
# -Build  : rebuild the disc image before launching
# -Paused : start Ymir paused (useful with -Debug)
# -Debug  : enable Ymir debug tracing (F11 to toggle in-session)

param(
    [switch]$Build,
    [switch]$Paused,
    [switch]$Debug
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
    powershell -ExecutionPolicy Bypass -File (Join-Path $root "build.ps1")
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
