# Mimas -- Doom for the Sega Saturn (Saturn Ring Library build)
#
# Based on the SRL sample Makefile pattern.
# Run via build.ps1 (MSYS2 MINGW64), or directly: make build
#
# Output: build/Mimas.bin + build/Mimas.cue

# -----------------------------------------------------------------------
# SRL configuration
# -----------------------------------------------------------------------
SRL_MALLOC_METHOD         = TLSF
SRL_MAX_TEXTURES          = 16      # We don't use VDP1 textures
SRL_MODE                  = NTSC
SRL_HIGH_RES              = 0
SRL_FRAMERATE             = 0
SRL_MAX_CD_BACKGROUND_JOBS = 1   # R2.1 reverted 2026-07-07: the R2.2 persistent WAD handle
                                 # REGRESSED on a fast-seek SD ODE (Phoebe) -- an open GFS handle
                                 # keeps the CD-block read-ahead / virtual drive active = a constant
                                 # timing drag (game-time dilation, not fps) present even with the
                                 # read path toggled off.  R2.2 is now default-off + never opens the
                                 # handle, so open_max=1 is enough again.  Restore =2 only to A/B R2.2
                                 # on a REAL-CD Saturn (where physical seek dominates + read-ahead pays).
SRL_MAX_CD_FILES          = 24   # GFS reads at most this many dir entries (incl. '.'/'..');
                                 # cd/data now holds DOOM1.WAD + DOOMRP.DRP + CDDAMAP.TXT +
                                 # SGL drivers/text (~12 entries). At 8, DOOM1.WAD sorted past
                                 # the cutoff -> "DOOM1.WAD not found on CD". Headroom for more.
SRL_MAX_CD_RETRIES        = 5

# Keep 68K + SGL sound driver alive (required for SRL::Sound::Cdda CDDA routing)
SRL_USE_SGL_SOUND_DRIVER  = 1

# SGL work area -- shrunk: we render no SGL polygons/sprites
SGL_MAX_VERTICES          = 64
SGL_MAX_POLYGONS          = 64
SGL_MAX_EVENTS            = 4
SGL_MAX_WORKS             = 4

# -----------------------------------------------------------------------
# Project settings
# -----------------------------------------------------------------------
CD_NAME    = Mimas
BUILD_DROP = ./build

# SRL location (git submodule)
SRL_INSTALL_ROOT = SaturnRingLib
SDK_ROOT = $(SRL_INSTALL_ROOT)/saturnringlib

# -----------------------------------------------------------------------
# Source files -- explicit list mirrors SaturnDoom/project/makefile SRCS.
# Files not listed here (gusconf.c, etc.) are unused on Saturn.
# -----------------------------------------------------------------------
# Shared Doom sources + dual-SH2 renderer live in the core/ submodule
# (doom-saturn-core), compiled verbatim by both Mimas and DoomJo.
DOOM_CORE_C = \
	dummy.c am_map.c doomdef.c doomstat.c dstrings.c \
	d_event.c d_items.c d_iwad.c d_loop.c d_main.c \
	d_mode.c d_net.c f_finale.c f_wipe.c g_game.c \
	hu_lib.c hu_stuff.c info.c i_cdmus.c i_endoom.c \
	i_joystick.c i_scale.c i_system.c i_timer.c memio.c \
	m_argv.c m_bbox.c m_cheat.c m_config.c m_controls.c \
	m_fixed.c m_menu.c m_misc.c m_random.c p_ceilng.c \
	p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c \
	p_map.c p_maputl.c p_mobj.c p_plats.c p_pspr.c \
	p_saveg.c p_setup.c p_sight.c p_spec.c p_switch.c \
	p_telept.c p_tick.c p_user.c r_bsp.c r_cache.c r_data.c \
	r_draw.c r_main.c r_plane.c r_segs.c r_sky.c \
	r_things.c sha1.c sounds.c statdump.c st_lib.c \
	st_stuff.c s_sound.c tables.c v_video.c wi_stuff.c \
	w_checksum.c w_file.c w_main.c w_wad.c z_zone.c \
	i_input.c i_video.c doomgeneric.c r_parallel.c
DOOM_CSRCS = $(addprefix core/,$(DOOM_CORE_C))

# Platform layer (SRL/C++), stays in src/.
DOOM_CXXSRCS = \
	src/main.cxx src/dg_saturn.cxx src/w_file_saturn.cxx \
	src/i_sound_saturn.cxx src/mp_input.cxx src/w_drp_saturn.cxx

SOURCES  = src/syscalls.c $(DOOM_CSRCS)
SOURCES += $(DOOM_CXXSRCS)

# -----------------------------------------------------------------------
# Doom-specific compile flags
# -----------------------------------------------------------------------
# Default ON: keeps the 68K running + the CDDA sound path active even with no
# music tracks present, so the SFX bug (correlated with 68K-alive) reproduces.
# build.ps1 still passes CDDA_MUSIC=1 explicitly when cd/music has tracks; an
# empty cd/music means the sox audio-append step finds nothing and is a no-op.
CDDA_MUSIC ?= 1
ifeq ($(CDDA_MUSIC),1)
  CDDA_FLAG = -DSATURN_CDDA_MUSIC
endif

# Benchmark warp: boot straight into a map (skips the title menu) for reproducible
# witness measurements -- see docs/REC_BENCHMARKS.md "WADs temoins".  Empty (default)
# = normal menu boot, no behaviour change.
#   Doom II:  make SAT_WARP_MAP=15      (-> MAP15)
#   Doom 1:   make SAT_WARP_MAP="4 2"   (-> E4M2, two single digits)
# SAT_WARP_SKILL = skill 1-5 (4 = Ultra-Violence).
SAT_WARP_MAP   ?=
SAT_WARP_SKILL ?= 4
ifneq ($(SAT_WARP_MAP),)
  WARP_FLAG = -DSAT_WARP_MAP='"$(SAT_WARP_MAP)"' -DSAT_WARP_SKILL='"$(SAT_WARP_SKILL)"'
endif

# QW2 (core/r_plane.c SAT_POTATO_INLINE_SPANS): potato solid-colour floors via inline
# memset instead of the record+execute span path (core source default = 1 = ON).  Only
# active when sat_potato_floors is set (pot1/pot2); pot0/pot0.5 are byte-identical either
# way.  Build-overridable for an A/B:  make SAT_POTATO_INLINE_SPANS=0  reverts to the span
# path so the HW value of QW2 can be measured (read row-Bw 'P' + avg fps at pot1/pot2).
# Empty (default) = leave the source #ifndef default (1) in place -> no behaviour change.
SAT_POTATO_INLINE_SPANS ?=
ifneq ($(SAT_POTATO_INLINE_SPANS),)
  POTATO_SPANS_FLAG = -DSAT_POTATO_INLINE_SPANS=$(SAT_POTATO_INLINE_SPANS)
endif

# M5 BSP-staging order A/B (core/p_setup.c P_StageBSP): empty (default) = source
# default 0 = nodes -> subsectors -> vertexes -> segs (E1M1 32KB arena: st17/40).
# 1 = vertexes -> segs -> subsectors -> nodes: stages everything wall-prep +
# R_AddLine read (~29KB, st29/40) at the price of the nodes staying on LWRAM.
# HW A/B 2026-07-02 (order 0): Bp -4.5/-9.2 ms on the two wall-heavy spots.
SAT_BSP_STAGE_SEGS_FIRST ?=
ifneq ($(SAT_BSP_STAGE_SEGS_FIRST),)
  BSP_STAGE_FLAG = -DSAT_BSP_STAGE_SEGS_FIRST=$(SAT_BSP_STAGE_SEGS_FIRST)
endif

# SRL puts modules/dummy/ in the path which stubs out stdio.h (no FILE type).
# Put the compiler's real newlib headers first so Doom's uses of FILE* work.
# -Isrc: Doom sources include each other with relative paths.
# -DNDEBUG: silence assert() (no abort() on Saturn anyway).
# -DRP_CMD_BUF_SIZE=0x2000: L2-RECLAIM (2026-07-10).  The legacy parity column-renderer cmd ring
#   (top of LWRAM) is INERT in shipping (rp_disabled forced =1 via sat_plane_parallel, r_main.c:1291;
#   the draw funcs r_parallel.c:1715+ render direct + return, never recording).  Shrunk 0x14000->0x2000
#   (keep an 8KB floor so the parity A/B stays revivable) returns ~72KB LWRAM to the Doom zone
#   (DG_ZoneBase = LOW_WORK_RAM_SIZE - RP_CMD_BUF_SIZE) -- endgame RAM for big PWADs.  DoomJo: benign.
SRL_CUSTOM_CCFLAGS = -w -fsigned-char \
    -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DNDEBUG \
    -DMAXVISPLANES=256 \
    -DSAT_VISPLANE_POOL=1 -DVP_POOL_PLANES=96 \
    -DRP_CMD_BUF_SIZE=0x2000 \
    -DTEXCACHE_MARGIN=0x20000 \
    -DSAT_REPACK \
    -DSAT_DEFER_SOUND_INIT \
    -DSAT_SND_PRECACHE \
    -Isaturn_libc \
    -Isrc \
    -Icore \
    $(CDDA_FLAG) \
    $(WARP_FLAG) \
    $(POTATO_SPANS_FLAG) \
    $(BSP_STAGE_FLAG)

# -----------------------------------------------------------------------
# Include SRL shared makefile
# -----------------------------------------------------------------------
include $(SDK_ROOT)/shared.mk

# Remove modules/dummy/ from the include path after shared.mk has set it.
# That dir stubs printf to ((void)0) and omits FILE; the real newlib stdio.h
# (system path) must win so that printf/fprintf route through _write().
CCFLAGS := $(filter-out -I$(SRL_INSTALL_ROOT)/saturnringlib/../modules/dummy,$(CCFLAGS))

# Override C rule for src/ only: shared.mk hardcodes -std=c2x which breaks
# old Doom C code (GCC 14 makes implicit function declarations hard errors in
# C23 mode). gnu11 restores the permissive behaviour. SGL's own files (in
# modules/sgl/SRC/) still use the shared.mk default (-std=c2x, needed for bool
# keyword in sl_def.h), so we restrict this override to src/*.c.
src/%.o : src/%.c
	$(CC) $< $(CCFLAGS) -std=gnu11 -MMD -MP -o $@

# Shared Doom C sources (core/ submodule) need the same gnu11 override.
core/%.o : core/%.c
	$(CC) $< $(CCFLAGS) -std=gnu11 -MMD -MP -o $@

# Header-dependency tracking.  -MMD (above) emits a .d next to each .o listing
# every header that .c included; -include pulls them back in so editing a shared
# header recompiles EVERY object that includes it -- not just the .c whose
# timestamp changed.  Without this, changing e.g. core/i_video.h (SCREENHEIGHT)
# left most core/*.o stale, mixing 200/224 across the binary -> "Bad V_CopyRect".
# -MP adds phony targets for each header so a deleted/renamed header never breaks
# the build.  (Covers core/*.c + src/*.c only; src/*.cxx use shared.mk's C++ rule
# and dg_saturn.cxx is force-touched every build by build.ps1.)
-include $(wildcard core/*.d src/*.d)

# SATURN PERF NOTE: -O3 on the REC files (r_bsp/r_segs/r_main/r_plane/r_things) was
# re-tested on the slave-reliable build (2026-06-16) and showed ZERO change (B sub-
# time identical to -O2).  REC is memory/bus-bound (BSP + visplane pointer-chasing,
# A-bus colormap/texture reads), not compute-bound, so better codegen doesn't help.
# Kept at -O2.  Don't re-add -O3 here without a new measurement.

# SATURN parallel-REC: the renderer DUAL-COMPILE (a full independent slave renderer per CPU)
# was removed -- it duplicated ~117KB of render BSS + a 40KB slave stack + a 332KB visplane
# pool, which OVERFLOWS the Saturn's full 2MB (~96KB HWRAM heap free).  The viable path is the
# d32xr phase-split model (shared render state, ~24-byte per-CPU GBR-TLS, producer/consumer),
# NOT duplication.  See docs/PARALLEL_REC_AUDIT.md.

# -----------------------------------------------------------------------
# Per-level repack (STREAMING_ANALYSIS.md §7.4/7.9-7.10) -- OPT-IN.
# -----------------------------------------------------------------------
# Emit the per-map LZSS container into cd/data so the disc carries BOTH the full WAD
# (raw fallback) AND the repacked blobs.  Build a repacked disc with:
#     make repack build           (or, on Windows: build.ps1 -Repack)
# The Step-3 loader auto-detects DOOMRP.DRP (magic "DRP1" + matching dir_crc32) and
# falls back to raw full-WAD streaming when it is absent/mismatched -- both stay
# loadable.  Needs python on PATH.  ASSETS_DIR (= ./cd/data) comes from shared.mk.
PYTHON  ?= python
DRP_WAD  = $(ASSETS_DIR)/DOOM1.WAD
DRP_OUT  = $(ASSETS_DIR)/DOOMRP.DRP

.PHONY: repack
repack: $(DRP_OUT)

$(DRP_OUT): tools/repack_wad.py $(DRP_WAD) core/info.c
	$(PYTHON) tools/repack_wad.py $(DRP_WAD) core/info.c --emit=$(DRP_OUT)
