# Top-level Makefile for the OPL3 demo (MinGW gcc on Windows).
#
# Project layout:
#   nuked/             - Nuked-OPL3 reference port (portable, bit-exact)
#   nuked_optimized/   - MCU-optimized variant of the Nuked port
#   songs/             - embedded songs (packed opl_song format)
#   demo_win/          - Windows demo wrapper (main + WinMM audio sink)
#   tools/             - offline DRO -> .h converter and analysis utilities
#   capture/           - DRO captures (DOSBox output)
#
# Select which OPL core to build with by setting CORE on the command line:
#   make                 (default: CORE=nuked)
#   make CORE=nuked_optimized
#
# Build:    make            (or:  build.bat)
# Run:      make run
# Clean:    make clean

CORE    ?= nuked
CC      = gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99 -I$(CORE) -Isongs -Idemo_win -Iheatshrink -DHEATSHRINK_DYNAMIC_ALLOC=1
LDLIBS  ?= -lwinmm

# Portable layer (the only files that go on the MCU):
CORE_SRC = $(CORE)/opl3.c $(CORE)/seq_player.c \
           heatshrink/heatshrink_decoder.c heatshrink/hs_stream.c
# Windows-only wrapper:
PC_SRC   = demo_win/main.c demo_win/audio_win.c demo_win/dro_load.c

# All embedded songs are auto-discovered: regenerate via
#   powershell -File tools/regen_songs.ps1
SONG_HEADERS = $(wildcard songs/*_song.h)

opl_demo.exe: $(CORE_SRC) $(PC_SRC) \
              $(CORE)/opl3.h $(CORE)/seq_player.h demo_win/audio.h \
              demo_win/dro_load.h songs/opl_song_hs.h \
              heatshrink/hs_stream.h heatshrink/heatshrink_decoder.h \
              $(SONG_HEADERS)
	$(CC) $(CFLAGS) -o $@ $(CORE_SRC) $(PC_SRC) $(LDLIBS)

run: opl_demo.exe
	./opl_demo.exe

songs: ; powershell -ExecutionPolicy Bypass -File tools/regen_songs.ps1

clean:
	-del /Q opl_demo.exe 2>nul

.PHONY: run clean songs
