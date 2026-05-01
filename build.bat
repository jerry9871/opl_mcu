@echo off
REM Direct one-shot build for users without GNU make installed.
gcc -O2 -Wall -Wextra -std=c99 ^
    -Inuked -Isongs -Idemo_win -Iheatshrink ^
    -DHEATSHRINK_DYNAMIC_ALLOC=1 ^
    -o opl_demo.exe ^
    nuked\opl3.c nuked\seq_player.c ^
    demo_win\main.c demo_win\audio_win.c demo_win\dro_load.c ^
    heatshrink\hs_stream.c ^
    heatshrink\heatshrink_decoder.c ^
    -lwinmm
