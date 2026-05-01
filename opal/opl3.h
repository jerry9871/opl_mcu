/*
    opl3.h — header-only adapter that exposes the Opal OPL3 emulator
    under the same `OPL3_*` symbol set used by sequencer/seq_player.c.
    Every wrapper is `static inline`, so no separate .c shim is needed
    and the calls fold away at -O2.

    Opal is by Reality (released into the public domain) and ported to
    pure C by Wohlstand (libADLMIDI).  See opal/LICENSE.txt and
    opal/opal.h for the original notes.

    Trade-offs versus nuked/:
      - ~12 KB of code instead of ~50 KB.
      - ~3-4x cheaper per generated stereo frame on Cortex-M.
      - Not bit-exact: percussion / rhythm mode is unimplemented and
        envelope shapes differ subtly from a real YMF262.  Most
        general FM material plays correctly; songs that lean on the
        BD/SD/TT/TC/HH percussion mode will be missing those drums.
*/
#ifndef OPL_OPL3_OPAL_SHIM_H
#define OPL_OPL3_OPAL_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "opal.h"

/*  The seq_player works against an opaque `opl3_chip` type.  Make it
    a synonym for Opal so the same call sites compile against either
    backend.                                                        */
typedef Opal opl3_chip;

/*  Buffered writes are a Nuked feature (a tiny FIFO that delays
    OPL3 register writes by `OPL_WRITEBUF_DELAY` samples to model
    the chip's internal latency).  Opal applies writes immediately;
    for our DRO-driven workload that is indistinguishable.          */
#define OPL_WRITEBUF_SIZE   1024
#define OPL_WRITEBUF_DELAY  2

static inline void
OPL3_Reset(opl3_chip* chip, uint32_t samplerate)
{
	Opal_Init(chip, (int)samplerate);
}

static inline void
OPL3_WriteReg(opl3_chip* chip, uint16_t reg, uint8_t v)
{
	Opal_Port(chip, reg, v);
}

static inline void
OPL3_WriteRegBuffered(opl3_chip* chip, uint16_t reg, uint8_t v)
{
	/*  Opal applies writes synchronously; no buffering needed. */
	Opal_Port(chip, reg, v);
}

static inline void
OPL3_GenerateResampled(opl3_chip* chip, int16_t* stereo)
{
	Opal_Sample(chip, &stereo[0], &stereo[1]);
}

static inline void
OPL3_Generate(opl3_chip* chip, int16_t* stereo)
{
	Opal_Sample(chip, &stereo[0], &stereo[1]);
}

#ifdef __cplusplus
}
#endif

#endif
