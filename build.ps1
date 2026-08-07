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
# DISC NAME: the outputs are named after the IWAD they carry -- -Wad Doom2 gives
#   build/Mimas-Doom2.bin/.cue, -Wad Doom1s -Cdda gives Mimas-Doom1s-CDDA.bin/.cue
#   (make CD_NAME override). No -Wad -> plain "Mimas". -Name <n> forces any name.
#   The per-WAD stash build/wads/<wad>/ keeps the SAME descriptive filenames, so a
#   .bin/.cue pair is self-describing once copied onto an SD card / ODE.
#
# -Repack : per-level repack (STREAMING_ANALYSIS.md §7.4/7.9-7.10). Generate the
#   per-map LZSS container cd/data/DOOM1.WAD -> cd/data/DOOMRP.DRP before the ISO
#   step, so the disc carries BOTH the full WAD (raw fallback) AND the repacked
#   blobs. The Step-3 loader auto-detects DOOMRP.DRP (magic "DRP1" + matching
#   dir_crc32) and falls back to raw full-WAD streaming when it is absent/mismatched
#   -- both stay loadable. Needs python on PATH; skipped when the .DRP is already
#   up to date. Omit -Repack for a raw disc (default, unchanged).
#
# -RotLevel <auto|8|4|2|1> : (with -Repack) sprite-rotation ladder -- strip rotation
#   lumps above the level from the .DRP blobs (PLAY* always kept); the engine
#   quantizes rotations to match, PER MAP (core sat_sprite_rotlevel, re-armed at
#   each map select from the v2 map records).
#     auto = DEFAULT: per map, the HIGHEST level whose blob fits the 4MB cart --
#            maps that fit at full keep all 8 rotations; only over-cart maps
#            degrade, one step at a time (measured: L4 suffices everywhere on
#            Doom II / TNT / Plutonia)
#     8 = full everywhere (over-cart maps then can't cart-stage: they stream from
#         CD with MUS music)      5 lumps per rotated frame
#     4 = front/back/left/right   3 lumps (facing/targeting stays readable per view)
#     2 = front/back              2 lumps -- minimum for multiplayer
#     1 = front only (Hexen)      1 lump  -- SOLO-oriented: in split, every player
#         sees every monster face-on (the targeting cue dies for everyone)
#   On top, a runtime distance-LOD draws far sprites front-only UNDER the ceiling
#   (sat_sprite_rotlod_dist, platform default ~768 map units).
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
    [string]$RotLevel = "auto",
    [switch]$FrontOnly,
    [switch]$Cdda,
    [switch]$Mus,
    [string]$WarpMap = "",
    [string]$WarpSkill = "4",
    [switch]$SegsFirst,
    [switch]$TestGod,
    [string]$Name = "",
    [string]$MusicSrc = "",   # NB: not -MusicDir -- PowerShell names are case-INSENSITIVE and
                              # $musicDir already means ./cd/music below, so the param would be
                              # silently overwritten by it.
    [string]$Tracks = "",
    [string]$OutDir = "",
    [switch]$Renumber
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
# Settle the MUS/CDDA arbitration HERE (was inside the -Mus stash block further down): the disc
# NAME below carries a -CDDA suffix, so it must know the final audio mode before the WAD block runs.
if ($Mus -and $Cdda) { Write-Warning "-Mus overrides -Cdda (building data-only)."; $Cdda = $false }

# Disc name: every output (.elf/.map/.bin/.cue/.iso) is named after the IWAD it carries, so a
# stray .bin on an SD card still says which game it is.  -Name wins; else Mimas-<wad>[-CDDA];
# else plain "Mimas" (the Makefile default).  Passed to make as CD_NAME.
$cdName = if ($Name) { $Name } else { "Mimas" }

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
    # ...and so the disc itself is NAMED after it (unless -Name forced one).
    if (-not $Name) { $cdName = "Mimas-$wadName" + $(if ($Cdda) { "-CDDA" } else { "" }) }
    # SATURN (stale-stash fix 2026-07-13): a WAD SWAP must force the ISO/.bin to regenerate.
    # Copy-Item above PRESERVES the source WAD's (old) mtime, so cd/data/DOOM1.WAD looks OLDER than a
    # prior build's ISO and make/shared.mk skips the rebuild; the DRP --if-stale ALSO skips when the
    # same WAD was built before (hash unchanged) -> NOTHING regenerates, and the stash copies the OLD
    # .bin while still printing "OK/Stashed" (the exact silent-stale bug: build/wads/<w>/ kept days-old
    # bytes across rebuilds).  Delete the outputs so make MUST rebuild them from the freshly-swapped
    # cd/data/, guaranteeing the stash gets today's bytes.
    Remove-Item (Join-Path $root "build\$cdName.iso"), (Join-Path $root "build\$cdName.bin") -Force -ErrorAction SilentlyContinue
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

    if ($FrontOnly) { $RotLevel = "1" }   # legacy alias
    if (@("auto","8","4","2","1") -notcontains "$RotLevel") { throw "-RotLevel must be auto, 8, 4, 2 or 1" }
    Write-Host "Repack: ensuring cd/data/DOOMRP.DRP matches the IWAD (rot-level $RotLevel)..."
    & $py $toolFile $wadFile $infoFile "--emit=$drpFile" "--if-stale" "--rot-level=$RotLevel"
    if ($LASTEXITCODE -ne 0) { throw "repack_wad.py failed (exit $LASTEXITCODE)" }
    Write-Host "Repack OK ($('{0:N0}' -f (Get-Item $drpFile).Length) bytes)"
}
elseif ($FrontOnly) { throw "-FrontOnly requires -Repack (.DRP build flag)" }

function ConvertTo-Msys2Path([string]$p) {
    $p = $p.Replace('\','/')
    if ($p -match '^([A-Za-z]):(.*)') { return '/' + $Matches[1].ToLower() + $Matches[2] }
    return $p
}

function Invoke-Msys2([string]$cmd) {
    # sh2eb-elf-gcc lives in SaturnRingLib/Compiler/sh2eb-elf/bin (extracted by setup_compiler.bat)
    $compilerBin = ConvertTo-Msys2Path (Join-Path $root "SaturnRingLib\Compiler\sh2eb-elf\bin")
    # The --login shell rebuilds a minimal PATH without the Windows python/git;
    # pre.makefile (make_ip.py, IP.BIN identity + V0.<git count>) needs both.
    # APPEND them (never prepend): the WindowsApps dir holding the python alias
    # also holds the WSL bash.exe alias, which would shadow MSYS /usr/bin/bash
    # for any 'bash ...' recipe line.
    $toolsPath = ""
    try { $toolsPath += ":" + (ConvertTo-Msys2Path (Split-Path (Get-Command python -ErrorAction Stop).Source)) } catch {}
    try { $toolsPath += ":" + (ConvertTo-Msys2Path (Split-Path (Get-Command git -ErrorAction Stop).Source)) } catch {}
    & $bash --login -c "export MSYSTEM=MINGW64; source /etc/profile 2>/dev/null; export PATH='$compilerBin':`$PATH'$toolsPath'; $cmd"
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
    # (-Mus vs -Cdda already arbitrated at the top, before the disc name was derived.)
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
    # -MusicSrc picks the WAV source (default ./music); -Tracks "2-15" / "2,3,7" filters which
    # track_NN.wav go on the disc -- a CD holds ~80 min of audio, so a 30-track library has to
    # be narrowed to the ones the shipping IWAD's music map can actually reach.
    $musicSrcDir  = if ($MusicSrc) { (Resolve-Path $MusicSrc).Path } else { Join-Path $root "music" }
    $musicSrcMsys = ConvertTo-Msys2Path $musicSrcDir
    if (Test-Path $musicDir) { Remove-Item (Join-Path $musicDir '*') -Recurse -Force -ErrorAction SilentlyContinue }
    $cddaWavs = @(Get-ChildItem -Path $musicSrcDir -Filter "track_*.wav" -ErrorAction SilentlyContinue |
                  Where-Object { $_.BaseName -match '^track_\d+$' } |
                  Sort-Object { [int]($_.BaseName -replace 'track_','') })
    if ($Tracks) {
        # "2-15", "2,3,7" or a mix -- expand to the set of accepted track numbers.
        $want = @()
        foreach ($part in ($Tracks -split ',')) {
            $p = $part.Trim()
            if ($p -match '^(\d+)\s*-\s*(\d+)$') { $want += [int]$Matches[1]..[int]$Matches[2] }
            elseif ($p -match '^\d+$')           { $want += [int]$p }
            elseif ($p)                          { throw "-Tracks: cannot parse '$p' (use 2-15 or 2,3,7)" }
        }
        $cddaWavs = @($cddaWavs | Where-Object { $want -contains [int]($_.BaseName -replace 'track_','') })
        if ($cddaWavs.Count -eq 0) { throw "-Tracks '$Tracks' matched no track_NN.wav in $musicSrcDir" }
    }
    if ($cddaWavs.Count -eq 0) { Write-Warning "-Cdda: no track_*.wav found in $musicSrcDir -> data-only (no CDDA)." }
    else {
        # Capacity check: a CD-R tops out at 74/80 min.  Audio is 2352 bytes/sector, 75 sectors/s.
        $audioMin = (($cddaWavs | Measure-Object Length -Sum).Sum / 2352) / 75 / 60
        Write-Host ("CDDA (multi-file): {0} track(s) from {1}, {2:N0} min of audio (data track adds ~2 min)." -f `
                    $cddaWavs.Count, $musicSrcDir, $audioMin)
        if ($audioMin -gt 78) { Write-Warning ("CDDA: {0:N0} min of audio EXCEEDS an 80-min CD-R -- emulator/ODE only, not burnable. Narrow with -Tracks." -f $audioMin) }
        elseif ($audioMin -gt 72) { Write-Warning ("CDDA: {0:N0} min of audio needs an 80-min CD-R (a 74-min disc will not take it)." -f $audioMin) }
    }
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
    # CD_NAME override = the per-IWAD disc name (shared.mk names .elf/.map/.bin/.cue/.iso from it).
    # Each WAD therefore links into its OWN output set: no cross-WAD stale-output aliasing, and the
    # .o files stay shared so switching WADs is still an incremental build.
    $makeArgs   = "CD_NAME='$cdName'"
    if ($cddaAppend) { $makeArgs += " CDDA_MUSIC=1" }
    # Benchmark warp: boot straight into a map (Makefile SAT_WARP_MAP -> core -warp),
    # skipping the title menu for reproducible captures.  Doom1: -WarpMap "1 8" (episode
    # map, two single digits); Doom2: -WarpMap 15.  Single quotes keep the space in
    # "1 8" as ONE make-var value through the bash -c.  (dg_saturn.cxx is touched every
    # build, so toggling warp on/off always recompiles it -- no stale-warp.)
    if ($WarpMap -ne "") { $makeArgs += " SAT_WARP_MAP='$WarpMap' SAT_WARP_SKILL='$WarpSkill'" }
    # M5 staging-order A/B (Makefile SAT_BSP_STAGE_SEGS_FIRST -> core/p_setup.c):
    # verts -> segs -> subsectors -> nodes instead of nodes-first (overlay: st29/40 vs st17/40).
    if ($SegsFirst) { $makeArgs += " SAT_BSP_STAGE_SEGS_FIRST=1" }
    # -TestGod (Makefile SAT_TEST_GOD -> core/g_game.c): flicker-test build = spawn invincible + full
    # kit so the HW tester stands in a horde without dying.  g_game.c must be touched too (make does
    # not track CFLAGS changes, so toggling it off would otherwise leave a godded .o).  Pair -WarpMap.
    $touchExtra = ""
    if ($TestGod) { $makeArgs += " SAT_TEST_GOD=1"; $touchExtra = " core/g_game.c" }

    Write-Host "Building $cdName$(if ($cddaAppend) {' (CDDA)'})..."
    # Touch the file carrying the on-screen build stamp (dg_saturn.cxx -> row 18
    # "b:<__TIME__>") so every build recompiles it with a fresh timestamp -- lets
    # you confirm on hardware that you flashed THIS build even when only core/
    # files changed (which otherwise leaves dg_saturn.o, and its __TIME__, stale).
    # core/p_setup.c is touched too: the M5 staging-order define lives there and make does
    # not track CFLAGS changes, so toggling -SegsFirst would otherwise leave a stale .o.
    Invoke-Msys2 "cd '$rootMsys' && touch src/dg_saturn.cxx core/p_setup.c$touchExtra && make $makeTarget $makeArgs"

    # TLSF pre-flight: the HWRAM TLSF pool (_end..__heap_end in build/<CD_NAME>.map)
    # must keep >= 4 KB or SRL's tlsf_add_pool rejects it at boot -> black
    # screen / boot loop.  Learned the hard way on the 2026-07-23 WPROBE image
    # (two 2 KB static .bss buffers sank the pool 8.4 -> 1.7 KB and the image
    # died at INIT CD on hardware).  Mirrors the Tethys build.ps1 gate.
    $mapPath = Join-Path $root "build\$cdName.map"
    if (Test-Path $mapPath) {
        $map   = Get-Content $mapPath -Raw
        $endM  = [regex]::Match($map, '0x([0-9a-f]+)\s+_end\b')
        $heapM = [regex]::Match($map, '0x([0-9a-f]+)\s+__heap_end\b')
        if ($endM.Success -and $heapM.Success) {
            $pool   = [Convert]::ToInt64($heapM.Groups[1].Value, 16) - [Convert]::ToInt64($endM.Groups[1].Value, 16)
            $poolKB = [math]::Round($pool / 1024, 2)
            Write-Host "pre-flight: HWRAM TLSF pool = $poolKB KB (_end..__heap_end)"
            # 4915 = 4.8 KB.  MEASURED 2026-08-06 on the TNT/M7 config, three builds in a row:
            # 5.14 KB booted, 4.67 KB HUNG right after its own "FRAME1 OK" print, 5.00 KB booted.
            # So the REAL floor is between 4.67 and 5.00 and sits WELL ABOVE tlsf_add_pool's 4 KB
            # limit -- 4 KB let a hanging image through and cost a debugging round-trip. Fail here.
            if ($pool -lt 4915) {
                throw "PRE-FLIGHT FAIL: HWRAM TLSF pool $poolKB KB < 4.8 KB -- MEASURED boot floor (2026-08-06, TNT/M7): 5.14 KB booted, 4.67 KB HUNG after FRAME1 OK, 5.00 KB booted. The floor drifts with SRL's init allocs and is well above tlsf_add_pool's 4 KB. Diet .text/.bss (or move buffers to LWRAM) before shipping."
            }
            if ($pool -lt 7168) {
                # 4.8 KB is the measured bar, 7 KB the comfort target -- between them a config
                # change (or another SRL init alloc) can still push it under. Warn, don't block.
                Write-Warning "pre-flight: pool $poolKB KB is above the 4.8 KB measured floor but below the 7 KB comfort target -- confirm boot before handing off"
            }
        } else {
            Write-Warning "pre-flight: _end/__heap_end not found in build/$cdName.map -- cannot verify TLSF pool"
        }
    } else {
        Write-Warning "pre-flight: build/$cdName.map missing -- cannot verify TLSF pool"
    }

    # SRL outputs to build/<CD_NAME>.bin + build/<CD_NAME>.cue
    $binPath = Join-Path $root "build\$cdName.bin"
    $cuePath = Join-Path $root "build\$cdName.cue"
    if (Test-Path $binPath) {
        $bin = Get-Item $binPath
        # STALE-BIN GUARD (2026-08-07).  iso2raw can FAIL -- "Failed to create output file:
        # ./build/<name>.bin" -- while a .bin from an earlier build is still sitting there, and
        # Test-Path alone happily reported "OK" on it.  The usual cause is the EMULATOR HOLDING THE
        # FILE OPEN (see [[ymir-locks-mimas-bin]]), which is exactly when you are iterating fastest.
        # The result is silent: you test a disc that is NOT the code you just compiled, and every
        # conclusion drawn from it is void. Compare against the .elf the link just produced.
        $elfPath = Join-Path $root "build\$cdName.elf"
        if (Test-Path $elfPath) {
            $elf = Get-Item $elfPath
            if ($bin.LastWriteTime -lt $elf.LastWriteTime) {
                throw "STALE DISC: build/$cdName.bin is OLDER than build/$cdName.elf -- the raw conversion did not run (emulator holding the file open?). Close the emulator and rebuild; do NOT test this disc, it is not the code you just compiled."
            }
        }
        Write-Output "OK  bin=$([string]::Format('{0:N0}', $bin.Length)) bytes"
        if (Test-Path $cuePath) { Write-Output "CUE: $cuePath" }

        # Where the finished disc lands: -OutDir wins, else the per-IWAD stash
        # build/wads/<wad>/ (kept so each WAD's latest build launches without a rebuild:
        # run_ymir.ps1 -Wad <name>).  A CDDA disc is a MULTI-FILE cue whose audio tracks
        # can run to hundreds of MB, so -OutDir gives it a folder of its own instead of
        # burying the library in the shared stash.
        $discDir = if ($OutDir) { $OutDir } elseif ($wadName) { Join-Path $root "build\wads\$wadName" } else { $null }
        if ($discDir) {
            New-Item -ItemType Directory -Path $discDir -Force | Out-Null
            $discDir = (Resolve-Path $discDir).Path
            # Discs left by the OLD naming scheme (everything was "Mimas.bin/.cue", so they
            # alias across WADs and can be years stale) are dropped; named variants of this
            # WAD -- Mimas-Doom1s vs Mimas-Doom1s-CDDA -- are KEPT side by side, and
            # run_ymir.ps1 launches the newest.
            Get-ChildItem $discDir -File -EA SilentlyContinue |
                Where-Object { $_.Name -in 'Mimas.cue','Mimas.bin','Mimas.iso' } |
                Remove-Item -Force
            Copy-Item $binPath (Join-Path $discDir "$cdName.bin") -Force
            if (Test-Path $cuePath) { Copy-Item $cuePath (Join-Path $discDir "$cdName.cue") -Force }
        }

        # -Cdda: replace the single-track .cue with a MULTI-FILE .cue -- data .bin (track 01)
        # + one AUDIO track per WAV.  TRACK NN = the file's number (track_NN.wav -> CD track
        # NN) so CDDAMAP.TXT stays valid.  Each track is converted to a headerless, 2352-padded
        # raw CD-DA stream (sox; same recipe as SRL's shared.mk CONVERT_AUDIO_TO_RAW): a .wav
        # referenced as WAVE puts a RIFF/LIST header inside the audio sectors and a payload
        # that is not a 2352 multiple -> misaligned to the CD sector grid.  Raw BINARY is the
        # only correct Saturn CD-DA track format.  sox lives in the same MSYS2/MINGW64 env
        # Invoke-Msys2 uses (/mingw64/bin/sox).
        if ($Cdda -and $cddaWavs.Count -gt 0) {
            if (-not $discDir) { throw "-Cdda needs a destination folder: pass -Wad or -OutDir." }
            $discMsys = ConvertTo-Msys2Path $discDir
            # Plan the TOC first: source .wav -> CD track number -> emitted .raw name.
            # Default CD track = the source file's number (track_07.wav -> TRACK 07), which keeps a
            # straight library aligned with CDDAMAP.TXT.  -Renumber assigns them sequentially from
            # 02 in file order instead: a Red Book TOC must be CONTIGUOUS, so any -Tracks selection
            # with a hole in it (e.g. "2-10,29-33") is only burnable renumbered.  The .raw is named
            # for its CD track either way, so the folder listing reads as the actual TOC -- and with
            # -Renumber, CDDAMAP.TXT must follow the NEW numbers, not the source library's.
            $cd = 1
            $cddaPlan = @(foreach ($w in $cddaWavs) {
                $cd = if ($Renumber) { $cd + 1 } else { [int]($w.BaseName -replace 'track_','') }
                [pscustomobject]@{ Wav = $w; Cd = $cd; Raw = ("track_{0:D2}" -f $cd) }
            })
            if (($cddaPlan.Cd | Select-Object -Unique).Count -ne $cddaPlan.Count) {
                throw "-Cdda: duplicate CD track numbers in the TOC -- check -Tracks / -Renumber."
            }
            if ($cddaPlan[0].Cd -ne 2 -or ($cddaPlan[-1].Cd - $cddaPlan[0].Cd + 1) -ne $cddaPlan.Count) {
                Write-Warning ("CDDA: TOC is not contiguous from 02 (tracks {0}) -- fine for an emulator, NOT burnable. Add -Renumber." -f (($cddaPlan.Cd) -join ','))
            }
            # Convert STRAIGHT INTO the disc folder: the .cue references each track by relative
            # name, so that is where they must end up anyway, and -MusicSrc may point at another
            # project's library that we must not litter with .raw files.  Re-converts only when
            # the .raw is missing or older than its .wav, so a rebuild into the same folder is free.
            Get-ChildItem $discDir -File -EA SilentlyContinue |
                Where-Object { $_.Name -match '^track_\d+\.(wav|raw)$' -and
                               $cddaPlan.Raw -notcontains $_.BaseName } |
                Remove-Item -Force        # drop tracks no longer in the TOC (narrower -Tracks, renumber)
            $sb = New-Object System.Text.StringBuilder
            [void]$sb.AppendLine("FILE `"$cdName.bin`" BINARY")
            [void]$sb.AppendLine('  TRACK 01 MODE1/2352')
            [void]$sb.AppendLine('    INDEX 01 00:00:00')
            $firstAudio = $true
            foreach ($t in $cddaPlan) {
                $w = $t.Wav
                $raw = Join-Path $discDir "$($t.Raw).raw"
                if (-not (Test-Path $raw) -or
                    ((Get-Item $w.FullName).LastWriteTime -gt (Get-Item $raw).LastWriteTime)) {
                    Write-Host "  sox $($w.Name) -> $($t.Raw).raw (CD track $($t.Cd), raw CD-DA, 2352-aligned)"
                    Invoke-Msys2 "cd '$discMsys' && sox '$musicSrcMsys/$($w.Name)' -t raw -r 44100 -e signed-integer -b 16 -c 2 '$($t.Raw).raw' && sz=`$(stat -c%s '$($t.Raw).raw'); pad=`$(( (2352 - sz % 2352) % 2352 )); if [ `$pad -ne 0 ]; then dd if=/dev/zero bs=1 count=`$pad >> '$($t.Raw).raw' 2>/dev/null; fi; true"
                }
                # RELATIVE name only -- Ymir ignores absolute paths and loads each FILE from
                # the .cue's own directory (CHANGELOG: "Ignore absolute paths...").
                [void]$sb.AppendLine("FILE `"$($t.Raw).raw`" BINARY")
                [void]$sb.AppendLine(("  TRACK {0:D2} AUDIO" -f $t.Cd))
                if ($firstAudio) { [void]$sb.AppendLine('    PREGAP 00:02:00'); $firstAudio = $false }
                [void]$sb.AppendLine('    INDEX 01 00:00:00')
            }
            [System.IO.File]::WriteAllText((Join-Path $discDir "$cdName.cue"), $sb.ToString())
            # build/<name>.cue keeps the plain data-only cue: the .raw tracks live in the disc
            # folder, so a multi-file cue in build/ would dangle.
            Write-Output "CUE: multi-file -- data .bin + $($cddaWavs.Count) raw CD-DA track(s) (2352-aligned BINARY)"
        }

        if ($discDir) {
            $tot = (Get-ChildItem $discDir -File | Measure-Object Length -Sum).Sum
            Write-Output ("Disc -> {0}\{1}.cue  ({2:N0} MB total)" -f $discDir, $cdName, ($tot/1MB))
            if (-not $OutDir) { Write-Output "  (launch later: run_ymir.ps1 -Wad $wadName)" }
        }
    } else {
        Write-Warning "build/$cdName.bin not found -- check make output above"
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
