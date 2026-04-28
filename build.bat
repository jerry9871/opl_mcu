@echo off
REM Direct one-shot build for users without GNU make installed.
gcc -O2 -Wall -Wextra -std=c99 ^
    -Icore -Isongs -Idemo_win ^
    -o opl_demo.exe ^
    core\opl3.c core\seq_player.c songs\test_melody.c ^
    demo_win\main.c demo_win\audio_win.c demo_win\dro_load.c ^
    -lwinmm
