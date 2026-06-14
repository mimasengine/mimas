# DoomSRL build script.
# Runs make via MSYS2 (SRL toolchain), then generates ISO + CUE.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build.ps1          # incremental
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Clean   # full rebuild
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Cdda    # force CDDA
#
# SRL's shared.mk handles: compile, link, ISO (xorrisofs), audio (sox).
# We generate the CUE here because absolute paths are needed for Kronos.

param(
    [switch]$Clean,
    [switch]$Cdda    # force CDDA even if music/ is absent (for testing)
)

$ErrorActionPreference = "Stop"
$root   = Split-Path -Parent $MyInvocation.MyCommand.Path
$msys2  = "C:\msys64"
$bash   = "$msys2\usr\bin\bash.exe"
$srlDir = Join-Path $root "SaturnRingLib"

if (-not (Test-Path $bash)) {
    throw "MSYS2 not found at $msys2. Run SaturnRingLib\setup_compiler.bat first."
}

# Convert Windows path to MSYS2 /c/... format
function To-Msys2Path([string]$p) {
    $p = $p.Replace('\','/')
    if ($p -match '^([A-Za-z]):(.*)') {
        return '/' + $Matches[1].ToLower() + $Matches[2]
    }
    return $p
}

function Invoke-Msys2([string]$cmd) {
    & $bash --login -c "export MSYSTEM=MINGW64; source /etc/profile 2>/dev/null; $cmd"
    if ($LASTEXITCODE -ne 0) { throw "MSYS2 command failed (exit $LASTEXITCODE)" }
}

Push-Location $root
try {
    # Detect CDDA WAV files
    $musicDir = Join-Path $root "music"
    $wavFiles = @()
    if (Test-Path $musicDir) {
        $wavFiles = Get-ChildItem -Path $musicDir -Filter "track_*.wav" |
                    Where-Object { $_.BaseName -match '^track_\d+$' } |
                    Sort-Object { [int]($_.BaseName -replace 'track_','') }
    }
    $cdda = ($wavFiles.Count -gt 0) -or $Cdda

    $rootMsys = To-Msys2Path $root

    if ($Clean) {
        Write-Host "Cleaning..."
        Invoke-Msys2 "cd '$rootMsys' && make clean"
    }

    $makeArgs = "make all"
    if ($cdda) { $makeArgs += " CDDA_MUSIC=1" }

    Write-Host "Building DoomSRL..."
    Invoke-Msys2 "cd '$rootMsys' && $makeArgs"

    # SRL's shared.mk produces game.iso (and copies audio tracks into it if sox found).
    # Generate a CUE with absolute paths so Kronos can find everything.
    $isoPath = Join-Path $root "game.iso"
    if (-not (Test-Path $isoPath)) { throw "game.iso not produced by make" }

    $isoAbs = (Resolve-Path $isoPath).Path
    $cue    = [System.Text.StringBuilder]::new()
    [void]$cue.AppendLine("FILE `"$isoAbs`" BINARY")
    [void]$cue.AppendLine('  TRACK 01 MODE1/2048')
    [void]$cue.AppendLine('    INDEX 01 00:00:00')
    [void]$cue.AppendLine('    POSTGAP 00:02:00')

    foreach ($wav in $wavFiles) {
        $tno = [int]($wav.BaseName -replace 'track_','')
        [void]$cue.AppendLine("FILE `"$($wav.FullName)`" WAVE")
        [void]$cue.AppendLine("  TRACK $($tno.ToString('D2')) AUDIO")
        [void]$cue.AppendLine('    INDEX 01 00:00:00')
    }

    $cue.ToString().TrimEnd() | Out-File -FilePath game.cue -Encoding ascii -NoNewline
    Add-Content -Path game.cue -Value ""

    $iso = Get-Item game.iso
    $msg = "OK  iso={0:N0} bytes" -f $iso.Length
    if ($cdda) { $msg += "  cdda=$($wavFiles.Count) tracks" }
    Write-Output $msg
}
finally { Pop-Location }
