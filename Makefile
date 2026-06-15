# DoomSRL -- Doom for the Sega Saturn (Saturn Ring Library build)
#
# Based on the SRL sample Makefile pattern.
# Run via build.ps1 (MSYS2 MINGW64), or directly: make build
#
# Output: build/DoomSRL.bin + build/DoomSRL.cue

# -----------------------------------------------------------------------
# SRL configuration
# -----------------------------------------------------------------------
SRL_MALLOC_METHOD         = TLSF
SRL_MAX_TEXTURES          = 16      # We don't use VDP1 textures
SRL_MODE                  = NTSC
SRL_HIGH_RES              = 0
SRL_FRAMERATE             = 0
SRL_MAX_CD_BACKGROUND_JOBS = 1
SRL_MAX_CD_FILES          = 8
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
CD_NAME    = DoomSRL
BUILD_DROP = ./build

# SRL location (git submodule)
SRL_INSTALL_ROOT = SaturnRingLib
SDK_ROOT = $(SRL_INSTALL_ROOT)/saturnringlib

# -----------------------------------------------------------------------
# Source files -- explicit list mirrors SaturnDoom/project/makefile SRCS.
# Files not listed here (gusconf.c, etc.) are unused on Saturn.
# -----------------------------------------------------------------------
# Shared Doom sources + dual-SH2 renderer live in the core/ submodule
# (doom-saturn-core), compiled verbatim by both DoomSRL and DoomJo.
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
	p_telept.c p_tick.c p_user.c r_bsp.c r_data.c \
	r_draw.c r_main.c r_plane.c r_segs.c r_sky.c \
	r_things.c sha1.c sounds.c statdump.c st_lib.c \
	st_stuff.c s_sound.c tables.c v_video.c wi_stuff.c \
	w_checksum.c w_file.c w_main.c w_wad.c z_zone.c \
	i_input.c i_video.c doomgeneric.c r_parallel.c
DOOM_CSRCS = $(addprefix core/,$(DOOM_CORE_C))

# Platform layer (SRL/C++), stays in src/.
DOOM_CXXSRCS = \
	src/main.cxx src/dg_saturn.cxx src/w_file_saturn.cxx \
	src/i_sound_saturn.cxx

SOURCES  = src/syscalls.c $(DOOM_CSRCS)
SOURCES += $(DOOM_CXXSRCS)

# -----------------------------------------------------------------------
# Doom-specific compile flags
# -----------------------------------------------------------------------
CDDA_MUSIC ?= 0
ifeq ($(CDDA_MUSIC),1)
  CDDA_FLAG = -DSATURN_CDDA_MUSIC
endif

# SRL puts modules/dummy/ in the path which stubs out stdio.h (no FILE type).
# Put the compiler's real newlib headers first so Doom's uses of FILE* work.
# -Isrc: Doom sources include each other with relative paths.
# -DNDEBUG: silence assert() (no abort() on Saturn anyway).
SRL_CUSTOM_CCFLAGS = -w -fsigned-char \
    -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DNDEBUG \
    -Isaturn_libc \
    -Isrc \
    -Icore \
    $(CDDA_FLAG)

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
	$(CC) $< $(CCFLAGS) -std=gnu11 -o $@

# Shared Doom C sources (core/ submodule) need the same gnu11 override.
core/%.o : core/%.c
	$(CC) $< $(CCFLAGS) -std=gnu11 -o $@
