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

/*  Opaque -- caller never touches the impl pointer. The actual chip state
    lives on the heap so this header doesn't have to include the full
    internal layout. */
typedef struct opl3_chip {
	void* impl;
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
