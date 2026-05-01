# Top-level Makefile for the OPL3 demo (MinGW gcc on Windows).
#
# Project layout:
#   nuked/             - Nuked-OPL3 reference port (portable, bit-exact)
#   nuked_optimized/   - MCU-optimized variant of the Nuked port
#   opal/              - Reality's Opal OPL3 (public domain, ~12 KB code)
#   songs/             - embedded songs (packed opl_song format)
#   demo_win/          - Windows demo wrapper (main + WinMM audio sink)
#   tools/             - offline DRO -> .h converter and analysis utilities
#   capture/           - DRO captures (DOSBox output)
#
# Default target builds one binary per core so you can A/B them:
#   make                 -> opl_demo_nuked.exe + opl_demo_opal.exe
#                           (and opl_demo.exe == opl_demo_nuked.exe for
#                            backward compatibility)
#   make nuked           -> opl_demo_nuked.exe only
#   make opal            -> opl_demo_opal.exe only
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
all: opl_demo_nuked.exe opl_demo_opal.exe opl_demo.exe

opl_demo_nuked.exe: nuked/opl3.c nuked/opl3.h \
                    $(COMMON_SRC) $(COMMON_HDR) $(SONG_HEADERS)
	$(CC) $(COMMON_CFLAGS) -Inuked -DCORE_NAME=\"nuked\" -o $@ \
	      nuked/opl3.c $(COMMON_SRC) $(LDLIBS)

opl_demo_opal.exe: opal/opal.c \
                   opal/opal.h opal/opl3.h \
                   $(COMMON_SRC) $(COMMON_HDR) $(SONG_HEADERS)
	$(CC) $(COMMON_CFLAGS) -Iopal -DCORE_NAME=\"opal\" -o $@ \
	      opal/opal.c $(COMMON_SRC) $(LDLIBS)

# Backward-compat default (same bits as opl_demo_nuked.exe).
opl_demo.exe: opl_demo_nuked.exe
	copy /Y opl_demo_nuked.exe opl_demo.exe >nul

# Convenience aliases.
nuked: opl_demo_nuked.exe
opal:  opl_demo_opal.exe

run: opl_demo.exe
	./opl_demo.exe

songs: ; powershell -ExecutionPolicy Bypass -File tools/regen_songs.ps1

clean:
	-del /Q opl_demo*.exe 2>nul

.PHONY: all run clean songs nuked opal
