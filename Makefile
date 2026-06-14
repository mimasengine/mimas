# DoomSRL -- Doom for the Sega Saturn (Saturn Ring Library build)
#
# Uses SRL's shared.mk which provides:
#   - sh2eb-elf-gcc 14.2 (C2x + C++23)
#   - ISO generation via xorrisofs
#   - Audio conversion via sox
#
# SRL discovers .c and .cxx files under src/ automatically.
# Extra defines are passed via SRL_EXTRA_CFLAGS / SRL_EXTRA_CXXFLAGS.

SRL_PATH = SaturnRingLib

# --- SRL config knobs ---------------------------------------------------
SRL_MAX_TEXTURES           = 16
SRL_MAX_CD_BACKGROUND_JOBS = 1
SRL_MAX_CD_FILES           = 4
SRL_USE_SGL_SOUND_DRIVER   = 1   # keep 68K alive for SCSP DSP / CDDA routing

# Doom compile-time defines (same as SaturnDoom)
DOOM_CFLAGS = -w -fsigned-char \
              -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
              $(CDDA_FLAG)

SRL_EXTRA_CFLAGS   = $(DOOM_CFLAGS)
SRL_EXTRA_CXXFLAGS = $(DOOM_CFLAGS)

# CDDA music: set CDDA_MUSIC=1 on the make command line (build.ps1 does this
# automatically when music/track_*.wav files are present).
CDDA_MUSIC ?= 0
ifeq ($(CDDA_MUSIC),1)
  CDDA_FLAG = -DSATURN_CDDA_MUSIC
endif

# Volume label shown on the Saturn CD browser
SRL_VOLUME_LABEL = DoomSRL

include $(SRL_PATH)/shared.mk
