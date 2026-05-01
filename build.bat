@echo off
REM Direct one-shot build for users without GNU make installed.
REM Produces one binary per available OPL3 core so you can A/B them
REM with the same song corpus and audio path:
REM
REM   opl_demo_nuked.exe   nuked\         decap-accurate, ~50 KB code
REM   opl_demo_opal.exe    opal\          public-domain Opal, ~12 KB code
REM   opl_demo_ymf262.exe  mame_ymf262\   MAME OPL3 (Burczynski/Satoh), full 18-ch + rhythm + 4-op
REM
REM Add more cores by copying one of the gcc invocations below and
REM swapping the include path, source files, and -DCORE_NAME tag.

setlocal
set CFLAGS=-O2 -Wall -std=c99 -Isongs -Idemo_win -Iheatshrink -Isequencer -DHEATSHRINK_DYNAMIC_ALLOC=1
set COMMON=sequencer\seq_player.c demo_win\main.c demo_win\audio_win.c demo_win\dro_load.c heatshrink\hs_stream.c heatshrink\heatshrink_decoder.c
set LIBS=-lwinmm

echo [1/3] nuked  -^> opl_demo_nuked.exe
gcc %CFLAGS% -Inuked -DCORE_NAME=\"nuked\" ^
    -o opl_demo_nuked.exe ^
    nuked\opl3.c %COMMON% %LIBS%
if errorlevel 1 goto :error

echo [2/3] opal   -^> opl_demo_opal.exe
gcc %CFLAGS% -Iopal -DCORE_NAME=\"opal\" ^
    -o opl_demo_opal.exe ^
    opal\opal.c %COMMON% %LIBS%
if errorlevel 1 goto :error

echo [3/3] ymf262 -^> opl_demo_ymf262.exe
REM Vendored from FBNeo (byte-identical to MAME's ymf262.c).  -w silences the
REM upstream's pre-C99 stylistic warnings; the audio path is unmodified.
gcc %CFLAGS% -Imame_ymf262 -DCORE_NAME=\"ymf262\" -w ^
    -o opl_demo_ymf262.exe ^
    mame_ymf262\ymf262.c %COMMON% %LIBS%
if errorlevel 1 goto :error

REM Keep an unsuffixed default for backward compatibility.
copy /Y opl_demo_nuked.exe opl_demo.exe >nul

echo.
echo Done.  Built:
dir /B opl_demo*.exe
exit /b 0

:error
echo.
echo Build failed.
exit /b 1
