/*
    dbopl OPL3 emulator -- C port of DOSBox's dbopl.cpp

    Original C++ source:
     Copyright (C) 2002-2021  The DOSBox Team
    C port (this file and dbopl.c):
     Copyright (C) 2026  silixcon

    License: GNU General Public License version 2 or later (same as the
    upstream). See LICENSE.txt in this directory.

    This is the project's standard OPL3 facade -- same API as the other
    cores under cores/<name>/opl3.h so seq_player.c links against any of
    them interchangeably.
*/
#ifndef OPL3_DBOPL_H
#define OPL3_DBOPL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*  All chip state lives directly inside opl3_chip -- no heap, no
    malloc, no init-time allocation.  The internal Chip struct is
    private to dbopl.c; we reserve a byte buffer here that is large
    enough to hold it, with 8-byte alignment for the function pointers
    and 64-bit-aligned tables inside.

    DBOPL_CHIP_BYTES is comfortably larger than the current sizeof(Chip)
    (~4.7 KB on 64-bit, ~4.5 KB on 32-bit) -- a static_assert in dbopl.c
    catches any future struct growth at compile time. */
#define DBOPL_CHIP_BYTES 5120

typedef struct opl3_chip {
	_Alignas(8) unsigned char storage[DBOPL_CHIP_BYTES];
} opl3_chip;

void OPL3_Reset(opl3_chip* chip, uint32_t samplerate);
void OPL3_WriteReg(opl3_chip* chip, uint16_t reg, uint8_t val);
void OPL3_WriteRegBuffered(opl3_chip* chip, uint16_t reg, uint8_t val);
void OPL3_Generate(opl3_chip* chip, int16_t out[2]);
void OPL3_GenerateResampled(opl3_chip* chip, int16_t out[2]);

#ifdef __cplusplus
}
#endif

#endif /* OPL3_DBOPL_H */
