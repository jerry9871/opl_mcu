@echo off
REM Direct one-shot build for users without GNU make installed.
REM Produces one binary per available OPL3 core so you can A/B them
REM with the same song corpus and audio path:
REM
REM   opl_demo_nuked.exe   cores\nuked\         decap-accurate, ~50 KB code
REM   opl_demo_opal.exe    cores\opal\          public-domain Opal, ~12 KB code
REM   opl_demo_mame.exe    cores\mame\          MAME OPL3 (Burczynski/Satoh), full 18-ch + rhythm + 4-op
REM   opl_demo_adlibemu.exe cores\adlibemu\    DOSBox legacy OPL3 (Ken Silverman lineage, LGPL2.1+)
REM   opl_demo_dbopl.exe   cores\dbopl\         DOSBox dbopl, C port (GPLv2 -- NOT for closed firmware)
REM
REM Add more cores by copying one of the gcc invocations below and
REM swapping the include path, source files, and -DCORE_NAME tag.

setlocal
set CFLAGS=-O2 -Wall -std=c99 -Isongs -Idemo_win -Iheatshrink -Isequencer -DHEATSHRINK_DYNAMIC_ALLOC=1
set COMMON=sequencer\seq_player.c demo_win\main.c demo_win\audio_win.c demo_win\dro_load.c heatshrink\hs_stream.c heatshrink\heatshrink_decoder.c
set LIBS=-lwinmm

echo [1/5] nuked  -^> opl_demo_nuked.exe
gcc %CFLAGS% -Icores\nuked -DCORE_NAME=\"nuked\" ^
    -o opl_demo_nuked.exe ^
    cores\nuked\opl3.c %COMMON% %LIBS%
if errorlevel 1 goto :error

echo [2/5] opal   -^> opl_demo_opal.exe
gcc %CFLAGS% -Icores\opal -DCORE_NAME=\"opal\" ^
    -o opl_demo_opal.exe ^
    cores\opal\opal.c %COMMON% %LIBS%
if errorlevel 1 goto :error

echo [3/5] mame   -^> opl_demo_mame.exe
REM Vendored from FBNeo (byte-identical to MAME's ymf262.c).  -w silences the
REM upstream's pre-C99 stylistic warnings; the audio path is unmodified.
gcc %CFLAGS% -Icores\mame -DCORE_NAME=\"mame\" -w ^
    -o opl_demo_mame.exe ^
    cores\mame\ymf262.c %COMMON% %LIBS%
if errorlevel 1 goto :error

echo [4/5] adlibemu -^> opl_demo_adlibemu.exe
REM Vendored from ValleyBell/libvgm (LGPL 2.1+).  -w silences a few harmless
REM signed/unsigned and unused-result warnings in the upstream code.
gcc %CFLAGS% -Icores\adlibemu -DCORE_NAME=\"adlibemu\" -DOPLTYPE_IS_OPL3 -w ^
    -o opl_demo_adlibemu.exe ^
    cores\adlibemu\adlibemu_opl3.c %COMMON% %LIBS%
if errorlevel 1 goto :error

echo [5/5] dbopl  -^> opl_demo_dbopl.exe
REM C port of DOSBox dbopl. GPLv2 -- only build this binary if your downstream
REM target accepts GPL. Other 4 cores remain available for proprietary use.
gcc %CFLAGS% -Icores\dbopl -DCORE_NAME=\"dbopl\" ^
    -o opl_demo_dbopl.exe ^
    cores\dbopl\dbopl.c %COMMON% %LIBS%
if errorlevel 1 goto :error

echo.
echo Done.  Built:
dir /B opl_demo*.exe
exit /b 0

:error
echo.
echo Build failed.
exit /b 1
