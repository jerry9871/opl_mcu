# Top-level Makefile for the OPL3 demo (MinGW gcc on Windows).
#
# Project layout:
#   core/       - portable synth + sequence player (this is the MCU build)
#   songs/      - embedded songs (static const opl_event arrays)
#   demo_win/   - Windows demo wrapper (main + WinMM audio sink)
#   tools/      - offline DRO -> .h converter and analysis utilities
#   capture/    - DRO captures (DOSBox output)
#
# Build:    make            (or:  build.bat)
# Run:      make run
# Clean:    make clean

CC      = gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99 -Icore -Isongs -Idemo_win
LDLIBS  ?= -lwinmm

# Portable layer (the only files that go on the MCU):
CORE_SRC = core/opl3.c core/seq_player.c songs/test_melody.c
# Windows-only wrapper:
PC_SRC   = demo_win/main.c demo_win/audio_win.c demo_win/dro_load.c

opl_demo.exe: $(CORE_SRC) $(PC_SRC) \
              core/opl3.h core/seq_player.h demo_win/audio.h \
              demo_win/dro_load.h songs/pre2_loop_song.h \
              songs/ww_intro_song.h songs/ww_theme_song.h
	$(CC) $(CFLAGS) -o $@ $(CORE_SRC) $(PC_SRC) $(LDLIBS)

run: opl_demo.exe
	./opl_demo.exe

clean:
	-del /Q opl_demo.exe 2>nul

.PHONY: run clean
