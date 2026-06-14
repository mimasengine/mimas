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
DOOM_CSRCS = \
	src/dummy.c src/am_map.c src/doomdef.c src/doomstat.c src/dstrings.c \
	src/d_event.c src/d_items.c src/d_iwad.c src/d_loop.c src/d_main.c \
	src/d_mode.c src/d_net.c src/f_finale.c src/f_wipe.c src/g_game.c \
	src/hu_lib.c src/hu_stuff.c src/info.c src/i_cdmus.c src/i_endoom.c \
	src/i_joystick.c src/i_scale.c src/i_system.c src/i_timer.c src/memio.c \
	src/m_argv.c src/m_bbox.c src/m_cheat.c src/m_config.c src/m_controls.c \
	src/m_fixed.c src/m_menu.c src/m_misc.c src/m_random.c src/p_ceilng.c \
	src/p_doors.c src/p_enemy.c src/p_floor.c src/p_inter.c src/p_lights.c \
	src/p_map.c src/p_maputl.c src/p_mobj.c src/p_plats.c src/p_pspr.c \
	src/p_saveg.c src/p_setup.c src/p_sight.c src/p_spec.c src/p_switch.c \
	src/p_telept.c src/p_tick.c src/p_user.c src/r_bsp.c src/r_data.c \
	src/r_draw.c src/r_main.c src/r_plane.c src/r_segs.c src/r_sky.c \
	src/r_things.c src/sha1.c src/sounds.c src/statdump.c src/st_lib.c \
	src/st_stuff.c src/s_sound.c src/tables.c src/v_video.c src/wi_stuff.c \
	src/w_checksum.c src/w_file.c src/w_main.c src/w_wad.c src/z_zone.c \
	src/i_input.c src/i_video.c src/doomgeneric.c

DOOM_CXXSRCS = \
	src/main.cxx src/dg_saturn.cxx src/w_file_saturn.cxx \
	src/i_sound_saturn.cxx src/r_parallel.cxx

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
