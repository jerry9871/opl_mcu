# Top-level Makefile for the OPL3 demo (MinGW gcc on Windows).
#
# Project layout:
#   cores/nuked/             - Nuked-OPL3 reference port (portable, bit-exact)
#   cores/nuked_optimized/   - MCU-optimized variant of the Nuked port
#   cores/opal/              - Reality's Opal OPL3 (public domain, ~12 KB code)
#   cores/mame/              - MAME OPL3 (Burczynski/Satoh, vendored from FBNeo)
#   cores/adlibemu/          - DOSBox legacy OPL3 (Ken Silverman lineage, LGPL 2.1+)
#   songs/             - embedded songs (packed opl_song format)
#   demo_win/          - Windows demo wrapper (main + WinMM audio sink)
#   tools/             - offline DRO -> .h converter and analysis utilities
#   capture/           - DRO captures (DOSBox output)
#
# Default target builds one binary per core so you can A/B them:
#   make                       -> opl_demo_nuked.exe + opl_demo_opal.exe
#                                 + opl_demo_mame.exe
#   make nuked                 -> opl_demo_nuked.exe only
#   make opal                  -> opl_demo_opal.exe only
#   make mame                  -> opl_demo_mame.exe only
#   make CORE=nuked_optimized  -> single binary against an explicit core
#
# Run:      make run
# Clean:    make clean

CC       = gcc
COMMON_CFLAGS = -O2 -Wall -std=c99 -Isongs -Idemo_win -Iheatshrink -Isequencer -DHEATSHRINK_DYNAMIC_ALLOC=1
LDLIBS   ?= -lwinmm

# Files that are independent of which OPL core we link against.
# The sequencer is shared across all PC cores (nuked, opal, ...).
# nuked_optimized/ keeps its own MCU-specific seq_player + FIFO and is
# not built on PC.
COMMON_SRC = sequencer/seq_player.c \
             demo_win/main.c demo_win/audio_win.c demo_win/dro_load.c \
             heatshrink/hs_stream.c heatshrink/heatshrink_decoder.c
COMMON_HDR = sequencer/seq_player.h \
             demo_win/audio.h demo_win/dro_load.h songs/opl_song_hs.h \
             heatshrink/hs_stream.h heatshrink/heatshrink_decoder.h

# All embedded songs are auto-discovered: regenerate via
#   powershell -File tools/regen_songs.ps1
SONG_HEADERS = $(wildcard songs/*_song.h)

# ---- per-core binaries ---------------------------------------------------
all: opl_demo_nuked.exe opl_demo_opal.exe opl_demo_mame.exe opl_demo_adlibemu.exe

opl_demo_nuked.exe: cores/nuked/opl3.c cores/nuked/opl3.h \
                    $(COMMON_SRC) $(COMMON_HDR) $(SONG_HEADERS)
	$(CC) $(COMMON_CFLAGS) -Icores/nuked -DCORE_NAME=\"nuked\" -o $@ \
	      cores/nuked/opl3.c $(COMMON_SRC) $(LDLIBS)

opl_demo_opal.exe: cores/opal/opal.c \
                   cores/opal/opal.h cores/opal/opl3.h \
                   $(COMMON_SRC) $(COMMON_HDR) $(SONG_HEADERS)
	$(CC) $(COMMON_CFLAGS) -Icores/opal -DCORE_NAME=\"opal\" -o $@ \
	      cores/opal/opal.c $(COMMON_SRC) $(LDLIBS)

opl_demo_mame.exe: cores/mame/ymf262.c \
                   cores/mame/ymf262.h cores/mame/opl3.h cores/mame/mame_compat.h \
                   $(COMMON_SRC) $(COMMON_HDR) $(SONG_HEADERS)
	$(CC) $(COMMON_CFLAGS) -Icores/mame -DCORE_NAME=\"mame\" -w -o $@ \
	      cores/mame/ymf262.c $(COMMON_SRC) $(LDLIBS)

opl_demo_adlibemu.exe: cores/adlibemu/adlibemu_opl3.c cores/adlibemu/adlibemu_opl_inc.c \
                       cores/adlibemu/adlibemu.h cores/adlibemu/adlibemu_opl_inc.h \
                       cores/adlibemu/opl3.h cores/adlibemu/adlibemu_compat.h \
                       $(COMMON_SRC) $(COMMON_HDR) $(SONG_HEADERS)
	$(CC) $(COMMON_CFLAGS) -Icores/adlibemu -DCORE_NAME=\"adlibemu\" -DOPLTYPE_IS_OPL3 -w -o $@ \
	      cores/adlibemu/adlibemu_opl3.c $(COMMON_SRC) $(LDLIBS)

# Convenience aliases.
nuked:    opl_demo_nuked.exe
opal:     opl_demo_opal.exe
mame:     opl_demo_mame.exe
adlibemu: opl_demo_adlibemu.exe

run: opl_demo_nuked.exe
	./opl_demo_nuked.exe

songs: ; powershell -ExecutionPolicy Bypass -File tools/regen_songs.ps1

clean:
	-del /Q opl_demo*.exe 2>nul

.PHONY: all run clean songs nuked opal mame adlibemu
