# boot_matrix.ps1 -- rank 4 of the 2026-08-25 roadmap.
#
# WHY THIS EXISTS.  Every core/ change ships into a submodule shared with a sibling port, and
# until today the only acceptance criterion was the owner playing.  The criterion itself has
# existed in prose since the zone-contiguity work -- "structs+slab => tout wads_temoins charge
# sauf Nuts" -- against 18 witness WADs, with zero automation.  Three failure modes the project
# has already been bitten by are exactly what this catches:
#   * boot-loop from TLSF pool starvation      -> the pool column, against the 4 800 B floor
#   * a stale per-WAD stash after a WAD swap   -> every row is a fresh -Repack build
#   * a stale DRP failing SILENTLY (~4 min)    -> -Repack is not optional here, it is the point
#
# WHAT IT CANNOT DO, stated up front so the report is never over-read: it BUILDS and PRE-FLIGHTS.
# It does not boot anything.  There is no headless Saturn in this repo, so "reaches the title
# screen and loads map 1" stays a human step -- the script ends by printing the exact launch
# lines for it, in the order that finds a regression fastest (smallest WAD first).
#
#   powershell -ExecutionPolicy Bypass -File tools/boot_matrix.ps1
#   powershell -ExecutionPolicy Bypass -File tools/boot_matrix.ps1 -Only Doom1s,Tnt,SCYTHE
#   powershell -ExecutionPolicy Bypass -File tools/boot_matrix.ps1 -Repack   # SLOW, see below
#
# 🔴 COST, MEASURED 2026-08-26 and an order of magnitude above the first estimate: FOUR WADs with
# -Repack took 517 MINUTES.  The LZSS repack of a full IWAD, not the compile, is what dominates.
# So -Repack is now OPT-IN.  Without it a sweep is minutes per WAD and still answers the three
# questions this script exists for -- does it COMPILE, does the POOL clear the boot-loop floor,
# is the CUE BOM-free.  ⚠ The discs a non-repack sweep leaves behind carry a STALE DRP and are
# NOT shippable ([[drp-repack-must-be-rebuilt]]): rebuild the one you actually ship, with -Repack.

# 🔴 DISK, and why this script DELETES AS IT GOES.  A sweep is a disc factory: build.ps1 leaves
# a loose build/Mimas-<w>.iso AND .bin, then stashes a second copy of the .bin under
# build/wads/<w>/.  That is ~3x the image per WAD -- 422 MB for TNT alone, and the drive this
# runs on sits at 94 % used.  Every column this script reports (pool from the linker MAP, byte
# size, cue BOM) is read BEFORE the cleanup below, so discarding the images costs the report
# nothing.  -Keep restores the old keep-everything behaviour when you actually want the discs.
param(
    [string[]]$Only = @(),
    [switch]$Repack,   # see the cost note above -- OFF by default, deliberately
    [switch]$Keep,     # keep every built image instead of deleting it once measured
    [string]$OutFile = "docs/captures/boot_matrix.md"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

# The pool floor below which the SGL boot allocates null and the game loops on the SEGA logo.
$POOL_FLOOR_KB = 4.8

# 🔴 IWAD vs PWAD, verified 2026-08-26 by reading every header in wads_temoins/.
#   Only an IWAD builds standalone.  A PWAD carries maps but inherits its textures, so
#   tools/flatten_textures.py dies on "no PNAMES lump" and build.ps1 aborts -- which is exactly
#   what grid1212 and HR did on the first real run of this script.  That is NOT a regression,
#   it is the corpus: 11 of the 18 witnesses are IWADs, 7 are PWADs, and Doom2HR / Doom2SCYTHE /
#   Doom2NUTS ARE the pre-merged builds of three of them.  grid1212 has NO merged counterpart.
#   ⚠ Merging one is not free either: tools/merge_wad.py writes lumps back-to-back while
#   strip_wad.py deliberately 4-pads, and an unaligned 32-bit read on the big-endian SH-2
#   returns garbage ([[saturn-cart-lump-alignment]]).  Fix that before minting new merges.
#   Two PWADs (SCYTHE, Nuts3) DO ship a PNAMES and therefore build anyway -- kind is what the
#   header says, `build` below is the ground truth.
# Ordered smallest-first: a break shows up in seconds, not in the 846 MB TNT build.
$WADS = @(
    @{ n = "Doom1s";      kind = "IWAD"; note = "shareware -- the reference ledger" },
    @{ n = "Doom1";       kind = "IWAD"; note = "registered v1.9" },
    @{ n = "Doom-ud";     kind = "IWAD"; note = "Ultimate (E4)" },
    @{ n = "Doom-ori";    kind = "IWAD"; note = "original v1.1" },
    @{ n = "Doom2";       kind = "IWAD"; note = "Doom II" },
    @{ n = "Tnt";         kind = "IWAD"; note = "Final Doom -- 4 maps degrade to L4 rotations" },
    @{ n = "Plutonia";    kind = "IWAD"; note = "Final Doom -- 3 maps degrade" },
    @{ n = "MPTEST";      kind = "IWAD"; note = "MP arming smoke test" },
    @{ n = "grid1212";    kind = "PWAD-nopn"; note = "1994 visplane-overflow demo -- sizes VP_POOL_PLANES, watch vp<peak>.<ovf>" },
    @{ n = "HR";          kind = "PWAD-nopn"; note = "Hell Revealed -- ds/o pressure" },
    @{ n = "HRMUS";       kind = "PWAD-nopn"; note = "HR + music" },
    @{ n = "Doom2HR";     kind = "IWAD"; note = "HR merged onto Doom II" },
    @{ n = "SCYTHE";      kind = "PWAD"; note = "MAP30 763 KB / MAP29 684 KB -- zone exhaustion, the last hard I_Error" },
    @{ n = "Doom2SCYTHE"; kind = "IWAD"; note = "Scythe merged" },
    @{ n = "nuts";        kind = "PWAD-nopn"; note = "EXPECTED TO FAIL -- documented, not a regression" },
    @{ n = "Nuts2";       kind = "PWAD-nopn"; note = "EXPECTED TO FAIL -- th/mo/dc pressure" },
    @{ n = "Nuts3";       kind = "PWAD"; note = "EXPECTED TO FAIL" },
    @{ n = "Doom2NUTS";   kind = "IWAD"; note = "EXPECTED TO FAIL" }
)
$EXPECT_FAIL = @("nuts", "Nuts2", "Nuts3", "Doom2NUTS")

# 🔴 -Only ARRIVES AS ONE STRING under `powershell -File` (that host does not split commas the
# way a normal call does), so `-Only a,b,c` used to match nothing and this script then wrote an
# EMPTY report that read exactly like a clean run -- the silent-failure class it exists to catch.
# Split defensively, and refuse to write a report at all when the filter selects nothing.
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($Only.Count -gt 0) {
    $known = $WADS | ForEach-Object { $_.n }
    $bad = $Only | Where-Object { $known -notcontains $_ }
    if ($bad) { throw ("-Only names no witness WAD: {0}. Known: {1}" -f ($bad -join ', '), ($known -join ', ')) }
    $WADS = $WADS | Where-Object { $Only -contains $_.n }
}
if ($WADS.Count -eq 0) { throw "no WADs selected -- refusing to write an empty report" }

function Get-PoolKB {
    # Read the pool the same way build.ps1's own pre-flight does, from the linker map, so the
    # number in this report and the number that gates the build can never disagree.
    param([string]$MapPath)
    if (-not (Test-Path $MapPath)) { return $null }
    $txt = Get-Content $MapPath -Raw
    $hs = [regex]::Match($txt, '(?m)^\s*0x([0-9a-fA-F]+)\s+__heap_start')
    $he = [regex]::Match($txt, '(?m)^\s*0x([0-9a-fA-F]+)\s+__heap_end')
    if (-not ($hs.Success -and $he.Success)) { return $null }
    $a = [Convert]::ToInt64($hs.Groups[1].Value, 16)
    $b = [Convert]::ToInt64($he.Groups[1].Value, 16)
    return [math]::Round(($b - $a) / 1024.0, 2)
}

function Test-CueBom {
    # The .cue MUST start with the bytes "FILE".  A BOM in front of it makes the emulator and
    # every ODE reject the disc with no useful message.
    param([string]$Cue)
    if (-not (Test-Path $Cue)) { return "missing" }
    $b = [System.IO.File]::ReadAllBytes($Cue)
    if ($b.Length -lt 4) { return "short" }
    if ($b[0] -eq 0x46 -and $b[1] -eq 0x49 -and $b[2] -eq 0x4C -and $b[3] -eq 0x45) { return "ok" }
    return ("BOM/" + ("{0:X2}{1:X2}{2:X2}{3:X2}" -f $b[0], $b[1], $b[2], $b[3]))
}

$rows = @()
$t0 = Get-Date

foreach ($w in $WADS) {
    $name = $w.n
    $src = "wads_temoins/$name.wad"
    if (-not (Test-Path $src)) { $src = "wads_temoins/$name.WAD" }
    if (-not (Test-Path $src)) {
        $rows += [pscustomobject]@{ wad = $name; build = "NO WAD"; pool = ""; bin = ""; cue = ""; note = $w.note }
        Write-Host ("[skip] {0} -- not in wads_temoins/" -f $name)
        continue
    }

    if ($w.kind -eq "PWAD-nopn") {
        $rows += [pscustomobject]@{ wad = $name; build = "PWAD (no PNAMES)"; pool = ""; bin = ""
                                    cue = ""; note = $w.note }
        Write-Host ("[skip] {0} -- PWAD without PNAMES: merge it onto an IWAD first" -f $name)
        continue
    }
    Write-Host ("[build] {0} ..." -f $name) -NoNewline
    $args = @("-ExecutionPolicy", "Bypass", "-File", "build.ps1", "-Wad", $name, "-Mus")
    if ($Repack) { $args += "-Repack" }
    # 🔴 NEVER `2>&1` A NATIVE EXE HERE.  In PowerShell 5.1 that merges the child's stderr
    # into the pipeline as ErrorRecords, and with $ErrorActionPreference='Stop' the FIRST such
    # line is fatal -- xorriso prints its banner ("GNU xorriso 1.5.6 ...") to stderr on every
    # successful run, so the matrix died on a build that had in fact succeeded.  Redirect to a
    # FILE instead (`*>` is a file redirect, not a stream merge) and judge on $LASTEXITCODE only.
    # ...and the file redirect is NOT enough either: `*>` still routes through the stream
    # machinery, so under $ErrorActionPreference='Stop' the wrapped stderr line is STILL fatal.
    # The only thing that actually disarms it is relaxing the preference around the call. The
    # child's own exit code is the verdict; its chatter is not our error to raise.
    $logFile = Join-Path $env:TEMP ("mimas_boot_{0}.log" -f $name)
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & powershell @args *> $logFile
    $ok = $LASTEXITCODE -eq 0
    $ErrorActionPreference = $eap
    $log = if (Test-Path $logFile) { Get-Content $logFile } else { @() }

    $map = "build/Mimas-$name.map"
    $pool = Get-PoolKB $map
    $disc = "build/wads/$name/Mimas-$name"
    $bin = if (Test-Path "$disc.bin") { (Get-Item "$disc.bin").Length } else { $null }
    $cue = Test-CueBom "$disc.cue"

    $verdict = if (-not $ok) { "FAIL" }
    elseif ($null -eq $pool) { "no map" }
    elseif ($pool -lt $POOL_FLOOR_KB) { "POOL<floor" }
    elseif ($cue -ne "ok") { "cue $cue" }
    else { "ok" }

    if ($EXPECT_FAIL -contains $name -and $verdict -ne "ok") { $verdict = "$verdict (expected)" }

    $rows += [pscustomobject]@{
        wad = $name; build = $verdict
        pool = if ($null -ne $pool) { "{0:N2}" -f $pool } else { "" }
        bin = if ($null -ne $bin) { "{0:N0}" -f $bin } else { "" }
        cue = $cue; note = $w.note
    }
    Write-Host ("  {0}  pool {1} KB" -f $verdict, $pool)
    if (-not $ok) { $log | Select-Object -Last 12 | ForEach-Object { Write-Host "    $_" } }

    # Delete as we go -- see the disk note at the top.  Order matters: $pool, $bin and $cue are
    # already captured above, so nothing measured is lost.  The loose build/Mimas-<w>.{iso,bin}
    # pair goes ALWAYS (the .iso is a pure intermediate and the loose .bin duplicates the stash);
    # the stash pair goes too unless -Keep, and the report then carries the rebuild line instead
    # of a launch line, so it can never claim a disc that is not on disk.
    $freed = 0
    $doomed = @("build/Mimas-$name.iso", "build/Mimas-$name.bin")
    if (-not $Keep) { $doomed += @("$disc.bin", "$disc.cue") }
    foreach ($p in $doomed) {
        if (Test-Path $p) {
            $freed += (Get-Item $p).Length
            Remove-Item $p -Force -ErrorAction SilentlyContinue
        }
    }
    if ($freed -gt 0) { Write-Host ("    cleaned {0:N0} MB" -f ($freed / 1MB)) }
}

$elapsed = [math]::Round(((Get-Date) - $t0).TotalMinutes, 1)

$md = New-Object System.Text.StringBuilder
[void]$md.AppendLine("# Boot matrix -- " + (Get-Date -Format "yyyy-MM-dd HH:mm"))
[void]$md.AppendLine()
[void]$md.AppendLine("Generated by ``tools/boot_matrix.ps1``. **This is a BUILD + PRE-FLIGHT matrix, not a boot test**:")
[void]$md.AppendLine("nothing here proves a disc reaches the title screen. The launch checklist at the bottom is the")
[void]$md.AppendLine("human half, and it is not optional before a ``core/`` commit.")
[void]$md.AppendLine()
[void]$md.AppendLine("Pool floor: **$POOL_FLOOR_KB KB** (below it, SGL's boot allocation returns null and the game loops).")
[void]$md.AppendLine("Elapsed: $elapsed min.")
if ($Keep) {
    [void]$md.AppendLine("Discs KEPT under ``build/wads/<wad>/`` (-Keep).")
} else {
    [void]$md.AppendLine("**Discs were DELETED as each row was measured** (default; pass ``-Keep`` to retain them).")
    [void]$md.AppendLine("The byte sizes below are what was measured before deletion. To launch one, rebuild it.")
}
if (-not $Repack) {
    [void]$md.AppendLine("⚠ Built WITHOUT ``-Repack``: any disc kept here carries a **stale DRP** and is not shippable.")
}
[void]$md.AppendLine()
[void]$md.AppendLine("| WAD | build | pool KB | bin bytes | cue | note |")
[void]$md.AppendLine("|---|---|---|---|---|---|")
foreach ($r in $rows) {
    [void]$md.AppendLine("| $($r.wad) | $($r.build) | $($r.pool) | $($r.bin) | $($r.cue) | $($r.note) |")
}
[void]$md.AppendLine()
[void]$md.AppendLine("## The human half -- launch, in this order")
[void]$md.AppendLine()
[void]$md.AppendLine("For each row that built: reach the title screen, start map 1, read row 11 ``LIM``.")
[void]$md.AppendLine("Stop at the first failure -- the list is ordered so a break shows up early.")
[void]$md.AppendLine()
[void]$md.AppendLine('```powershell')
foreach ($r in $rows) {
    if ($r.build -like "ok*") { [void]$md.AppendLine("powershell -File run_ymir.ps1 -Wad $($r.wad)") }
}
[void]$md.AppendLine('```')
[void]$md.AppendLine()
[void]$md.AppendLine("Named heavy cases worth a warp beyond map 1: **Doom2 MAP13** (lazy texture directories),")
[void]$md.AppendLine("**Tnt MAP20** (the 1p tic-bound ledger spot), **SCYTHE MAP30** (zone exhaustion),")
[void]$md.AppendLine("**grid1212** (visplane pool -- read ``vp<peak>.<ovf>``, the second digit must stay 0).")

$dir = Split-Path -Parent $OutFile
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
[System.IO.File]::WriteAllText((Join-Path $root $OutFile), $md.ToString(), (New-Object System.Text.UTF8Encoding $false))

Write-Host ""
Write-Host ("Report -> {0}   ({1} min)" -f $OutFile, $elapsed)
$rows | Format-Table -AutoSize
