# Mimas build script.
# Runs SRL's make build via MSYS2 MINGW64.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build.ps1                 # incremental (MUS synth = DEFAULT until CDDA fixed)
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Clean          # full rebuild
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Mus            # data-only MUS synth disc (= the default)
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Wad Doom2.wad  # swap the IWAD
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Repack         # + per-level repack
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Cdda           # multi-file CDDA disc
#   powershell -ExecutionPolicy Bypass -File build.ps1 -WarpMap "1 8"  # boot straight into E1M8
#   powershell -ExecutionPolicy Bypass -File build.ps1 -SegsFirst      # M5 staging order A/B: verts+segs before nodes
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
# -RotLevel <8|4|2|1> : (with -Repack) sprite-rotation degradation ladder -- strip
#   rotation lumps above the level from the .DRP blobs (PLAY* always kept) and flag
#   the header so the engine quantizes rotations to match (core sat_sprite_rotlevel).
#     8 = full (default, no strip)          5 lumps per rotated frame
#     4 = front/back/left/right             3 lumps -- RECOMMENDED for cart discs:
#         fits the 4MB cart on Doom II/TNT/Plutonia AND keeps monster facing
#         readable per split view ("who is it targeting")
#     2 = front/back                        2 lumps -- minimum for multiplayer
#     1 = front only (Hexen-Saturn trick)   1 lump  -- SOLO-oriented: in split, every
#         player sees every monster face-on (the targeting cue dies for everyone)
#   In-combat first-sight page-ins shrink accordingly (5->3->2->1 lumps per frame).
# -FrontOnly : legacy alias for -RotLevel 1. See STREAMING_FLUIDITY_ROADMAP.md.
#
# SRL's shared.mk handles: compile, link, ISO (xorrisofs), CUE generation.
# CDDA music: place WAV/FLAC/OGG/MP3 files in cd/music/ and create a
# cd/music/tracklist file listing them in order (one per line, track 02+).
# SRL will convert them with sox and append them to the disc image.

param(
    [switch]$Clean,
    [string]$Wad,
    [switch]$Repack,
    [int]$RotLevel = 8,
    [switch]$FrontOnly,
    [switch]$Cdda,
    [switch]$Mus,
    [string]$WarpMap = "",
    [string]$WarpSkill = "4",
    [switch]$SegsFirst
)

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$msys2 = "C:\msys64"
$bash  = "$msys2\usr\bin\bash.exe"

if (-not (Test-Path $bash)) {
    throw "MSYS2 not found at $msys2. Run SaturnRingLib\setup_compiler.bat first."
}

# MUS is the DEFAULT build until the CDDA path is fixed: a bare `build.ps1` produces the
# data-only MUS-synth disc.  Pass -Cdda to force a CDDA disc (-Mus is then redundant = the default).
if (-not $Cdda -and -not $Mus) {
    $Mus = $true
    Write-Host "Default build = -Mus (data-only MUS synth, CDDA path parked); pass -Cdda for a CDDA disc."
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
    # remember which IWAD this is so the finished disc can be stashed per-WAD below
    $wadName = [System.IO.Path]::GetFileNameWithoutExtension($src)
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

    if ($FrontOnly) { $RotLevel = 1 }   # legacy alias
    if (@(8,4,2,1) -notcontains $RotLevel) { throw "-RotLevel must be 8, 4, 2 or 1" }
    Write-Host "Repack: ensuring cd/data/DOOMRP.DRP matches the IWAD..."
    $foArgs = @(); if ($RotLevel -ne 8) { $foArgs = @("--rot-level=$RotLevel") }
    & $py $toolFile $wadFile $infoFile "--emit=$drpFile" "--if-stale" @foArgs
    if ($LASTEXITCODE -ne 0) { throw "repack_wad.py failed (exit $LASTEXITCODE)" }
    Write-Host "Repack OK ($('{0:N0}' -f (Get-Item $drpFile).Length) bytes)"
}
elseif ($FrontOnly -or $RotLevel -ne 8) { throw "-RotLevel/-FrontOnly require -Repack (.DRP build flags)" }

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
$cddaWavs  = @()    # -Cdda multi-file: the WAV tracks referenced separately in the .cue

# -Mus: force a DATA-ONLY (MUS synth) disc. shared.mk appends whatever sits in cd/music/, so
# the only robust way to guarantee no CDDA track is to move cd/music aside (and drop the
# CDAUDIO.TXT marker so the runtime stays on the MUS synth) for this build, then restore it in
# the finally below -- non-destructive, so the CDDA setup is never lost. Overrides -Cdda.
$musStash = $null
if ($Mus) {
    if ($Cdda) { Write-Warning "-Mus overrides -Cdda (building data-only)."; $Cdda = $false }
    $musStash = Join-Path ([System.IO.Path]::GetTempPath()) "mimas_mus_stash_$PID"
    New-Item -ItemType Directory -Path $musStash -Force | Out-Null
    if (Test-Path $musicDir) {
        Get-ChildItem $musicDir -File -ErrorAction SilentlyContinue |
            ForEach-Object { Move-Item $_.FullName $musStash -Force }
    }
    $musMarker = Join-Path $root "cd\data\CDAUDIO.TXT"
    if (Test-Path $musMarker) { Move-Item $musMarker (Join-Path $musStash "CDAUDIO.TXT") -Force }
    Write-Host "MUS: cd/music cleared + CDAUDIO.TXT removed (data-only disc); restored after build."
}

if ($Cdda) {
    # MULTI-FILE CDDA (-Cdda): build a SMALL data-only .bin (no audio appended -> fast
    # build, fast Ymir mount) and reference each music track SEPARATELY in a multi-file
    # .cue (read on demand).  Each music/track_NN.wav is converted to a headerless, 2352-
    # byte-sector-aligned raw CD-DA stream (sox, the same recipe SRL's shared.mk uses) and
    # referenced as a BINARY AUDIO track -- a Saturn CD-DA track is raw 2352-byte sectors,
    # NOT a RIFF/WAVE container, so a .wav (RIFF + a LIST/INFO metadata chunk, audio payload
    # starting mid-sector and not a 2352 multiple) left the audio misaligned to the sector
    # grid and made boot crawl.  Avoids both the ~470 MB single-.bin AND the malformed WAVE.
    $musicSrcDir  = Join-Path $root "music"
    $musicSrcMsys = ConvertTo-Msys2Path $musicSrcDir
    if (Test-Path $musicDir) { Remove-Item (Join-Path $musicDir '*') -Recurse -Force -ErrorAction SilentlyContinue }
    $cddaWavs = @(Get-ChildItem -Path (Join-Path $root "music") -Filter "track_*.wav" -ErrorAction SilentlyContinue |
                  Where-Object { $_.BaseName -match '^track_\d+$' } |
                  Sort-Object { [int]($_.BaseName -replace 'track_','') })
    if ($cddaWavs.Count -eq 0) { Write-Warning "-Cdda: no music/track_*.wav found -> data-only (no CDDA)." }
    else { Write-Host "CDDA (multi-file): $($cddaWavs.Count) track(s) referenced separately (no big .bin)." }
    $cddaAppend = $false   # keep cd/music empty so shared.mk appends nothing -> small data .bin
}
else {
    $cddaAppend = (Test-Path $trackList) -or
            ((Test-Path $musicDir) -and (Get-ChildItem $musicDir -Include "*.wav","*.mp3","*.flac","*.ogg" -Recurse -ErrorAction SilentlyContinue).Count -gt 0)

    # Handle optional external music/ directory (compatibility with SaturnDoom layout)
    $extMusic = Join-Path $root "music"
    if (-not $Mus -and -not $cddaAppend -and (Test-Path $extMusic)) {
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
            $cddaAppend = $true
        }
    }
}

# CDDA marker: the runtime detects CDDA via this GFS data file, NOT a raw CDC_TgetToc TOC
# probe -- that probe HANGS ~10 min under Ymir now that the boot-time CDC_CdInit is deferred
# (SAT_DEFER_SOUND_INIT).  Present iff the disc actually carries audio (multi-file -Cdda OR the
# shared.mk append); absent -> the runtime stays on the MUS synth and issues no CD command at boot.
$cddaMarker = Join-Path $root "cd\data\CDAUDIO.TXT"
if (($Cdda -and $cddaWavs.Count -gt 0) -or $cddaAppend) {
    Set-Content -Path $cddaMarker -Value "cdda" -Encoding ascii -NoNewline
    Write-Host "CDDA marker: cd/data/CDAUDIO.TXT (runtime CDDA music ON)"
} else {
    Remove-Item $cddaMarker -Force -ErrorAction SilentlyContinue
    Write-Host "CDDA marker: removed (MUS synth, no CD-block probe at boot)"
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
    if ($cddaAppend) { $makeArgs = "CDDA_MUSIC=1" }
    # Benchmark warp: boot straight into a map (Makefile SAT_WARP_MAP -> core -warp),
    # skipping the title menu for reproducible captures.  Doom1: -WarpMap "1 8" (episode
    # map, two single digits); Doom2: -WarpMap 15.  Single quotes keep the space in
    # "1 8" as ONE make-var value through the bash -c.  (dg_saturn.cxx is touched every
    # build, so toggling warp on/off always recompiles it -- no stale-warp.)
    if ($WarpMap -ne "") { $makeArgs += " SAT_WARP_MAP='$WarpMap' SAT_WARP_SKILL='$WarpSkill'" }
    # M5 staging-order A/B (Makefile SAT_BSP_STAGE_SEGS_FIRST -> core/p_setup.c):
    # verts -> segs -> subsectors -> nodes instead of nodes-first (overlay: st29/40 vs st17/40).
    if ($SegsFirst) { $makeArgs += " SAT_BSP_STAGE_SEGS_FIRST=1" }

    Write-Host "Building Mimas$(if ($cddaAppend) {' (CDDA)'})..."
    # Touch the file carrying the on-screen build stamp (dg_saturn.cxx -> row 18
    # "b:<__TIME__>") so every build recompiles it with a fresh timestamp -- lets
    # you confirm on hardware that you flashed THIS build even when only core/
    # files changed (which otherwise leaves dg_saturn.o, and its __TIME__, stale).
    # core/p_setup.c is touched too: the M5 staging-order define lives there and make does
    # not track CFLAGS changes, so toggling -SegsFirst would otherwise leave a stale .o.
    Invoke-Msys2 "cd '$rootMsys' && touch src/dg_saturn.cxx core/p_setup.c && make $makeTarget $makeArgs"

    # SRL outputs to build/Mimas.bin + build/Mimas.cue
    $binPath = Join-Path $root "build\Mimas.bin"
    $cuePath = Join-Path $root "build\Mimas.cue"
    if (Test-Path $binPath) {
        $bin = Get-Item $binPath
        Write-Output "OK  bin=$([string]::Format('{0:N0}', $bin.Length)) bytes"
        if (Test-Path $cuePath) { Write-Output "CUE: $cuePath" }

        # -Cdda: rewrite the single-track .cue as a MULTI-FILE .cue -- data .bin (track 01)
        # + each WAV as its own AUDIO track (WAVE, absolute path -> shared, not copied).
        # TRACK NN = the file's number (track_NN.wav -> CD track NN) so CDDAMAP stays valid.
        # The data .bin stays tiny; Ymir reads each WAV on demand.
        if ($Cdda -and $cddaWavs.Count -gt 0) {
            # Convert each track to a headerless, 2352-padded raw CD-DA stream (sox; same
            # recipe as SRL's shared.mk CONVERT_AUDIO_TO_RAW) and reference the .raw as a
            # BINARY AUDIO track.  A .wav referenced as WAVE put a RIFF/LIST header inside the
            # audio sectors and a payload that is not a 2352 multiple -> misaligned to the CD
            # sector grid; raw BINARY is the only correct Saturn CD-DA track format.  sox is
            # in the same MSYS2/MINGW64 env Invoke-Msys2 uses (/mingw64/bin/sox).
            $cueDir = Split-Path $cuePath
            $sb = New-Object System.Text.StringBuilder
            [void]$sb.AppendLine('FILE "Mimas.bin" BINARY')
            [void]$sb.AppendLine('  TRACK 01 MODE1/2352')
            [void]$sb.AppendLine('    INDEX 01 00:00:00')
            $firstAudio = $true
            foreach ($w in $cddaWavs) {
                $nn  = [int]($w.BaseName -replace 'track_','')
                $raw = Join-Path $musicSrcDir "$($w.BaseName).raw"
                # (re)convert only when the .raw is missing or older than its .wav source
                if (-not (Test-Path $raw) -or
                    ((Get-Item $w.FullName).LastWriteTime -gt (Get-Item $raw).LastWriteTime)) {
                    Write-Host "  sox $($w.Name) -> $($w.BaseName).raw (raw CD-DA, 2352-aligned)"
                    Invoke-Msys2 "cd '$musicSrcMsys' && sox '$($w.Name)' -t raw -r 44100 -e signed-integer -b 16 -c 2 '$($w.BaseName).raw' && sz=`$(stat -c%s '$($w.BaseName).raw'); pad=`$(( (2352 - sz % 2352) % 2352 )); if [ `$pad -ne 0 ]; then dd if=/dev/zero bs=1 count=`$pad >> '$($w.BaseName).raw' 2>/dev/null; fi; true"
                }
                # RELATIVE name only -- Ymir ignores absolute paths and loads each FILE from
                # the .cue's own directory (CHANGELOG: "Ignore absolute paths...").  Put the
                # .raw next to the .cue so the fresh build/ disc is self-contained too.
                Copy-Item $raw (Join-Path $cueDir "$($w.BaseName).raw") -Force
                [void]$sb.AppendLine("FILE `"$($w.BaseName).raw`" BINARY")
                [void]$sb.AppendLine(("  TRACK {0:D2} AUDIO" -f $nn))
                if ($firstAudio) { [void]$sb.AppendLine('    PREGAP 00:02:00'); $firstAudio = $false }
                [void]$sb.AppendLine('    INDEX 01 00:00:00')
            }
            [System.IO.File]::WriteAllText($cuePath, $sb.ToString())
            Write-Output "CUE: multi-file -- data .bin + $($cddaWavs.Count) raw CD-DA track(s) (2352-aligned BINARY)"
        }

        # Stash the finished disc per-IWAD so each WAD's latest build is kept and can be
        # launched instantly later (no rebuild/repack): run_ymir.ps1 -Wad <name>.  The
        # .cue references Mimas.bin by relative name, so copying both into the folder
        # keeps it self-contained.  Only when -Wad gave us a name.
        if ($wadName) {
            $stashDir = Join-Path $root "build\wads\$wadName"
            New-Item -ItemType Directory -Path $stashDir -Force | Out-Null
            Copy-Item $binPath (Join-Path $stashDir "Mimas.bin") -Force
            if (Test-Path $cuePath) { Copy-Item $cuePath (Join-Path $stashDir "Mimas.cue") -Force }
            # -Cdda: the multi-file .cue references the raw CD-DA tracks by relative name, so
            # they must sit next to it -- copy them into the stash (Ymir reads them on demand).
            # Stale track_*.wav/.raw from a previous CDDA build are cleared first.
            if ($Cdda) {
                Get-ChildItem $stashDir -EA SilentlyContinue |
                    Where-Object { $_.Name -match '^track_\d+\.(wav|raw)$' } | Remove-Item -Force
                foreach ($w in $cddaWavs) {
                    Copy-Item (Join-Path $musicSrcDir "$($w.BaseName).raw") (Join-Path $stashDir "$($w.BaseName).raw") -Force
                }
            }
            Write-Output "Stashed -> build/wads/$wadName/  (launch later: run_ymir.ps1 -Wad $wadName)"
        }
    } else {
        Write-Warning "build/Mimas.bin not found -- check make output above"
    }
}
finally {
    Pop-Location
    # -Mus: restore the stashed cd/music tracks + CDAUDIO.TXT marker so the CDDA setup is intact.
    if ($musStash -and (Test-Path $musStash)) {
        $musMarker = Join-Path $musStash "CDAUDIO.TXT"
        if (Test-Path $musMarker) { Move-Item $musMarker (Join-Path $root "cd\data\CDAUDIO.TXT") -Force }
        Get-ChildItem $musStash -File -ErrorAction SilentlyContinue |
            ForEach-Object { Move-Item $_.FullName $musicDir -Force }
        Remove-Item $musStash -Recurse -Force -ErrorAction SilentlyContinue
    }
}
