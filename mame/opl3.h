/*
    opl3.h — header-only adapter that exposes MAME's ymf262.c (Jarek
    Burczynski / Tatsuyuki Satoh, vendored from FBNeo) under the
    same `OPL3_*` symbol set used by sequencer/seq_player.c.  Every
    wrapper is `static inline`, so no separate .c shim is needed
    and the calls fold away at -O2.

    Trade-offs versus nuked/:
      - Pure C99, no C++ dependencies; ~70 KB code (~50 KB at -Os).
      - Cheaper per generated stereo frame than Nuked.
      - Full OPL3: 18 channels, 4-op mode, rhythm mode (BD/SD/TT/TC/HH),
        4-channel pan.  The downstream adapter folds the 4 panned
        outputs into stereo.
      - Not bit-exact: pre-Nuked envelope/phase model; comparable in
        accuracy to dbopl, well-validated through years of MAME use.

    Each opl3_chip is an opaque pointer to a heap-allocated OPL3
    state.  ymf262_init() does the allocation; ymf262_shutdown()
    frees it.  We wrap that behind a one-time-init scheme keyed off
    the `chip` pointer so the API matches the chip-by-value style
    used elsewhere.
*/
#ifndef OPL_OPL3_MAME_ADAPTER_H
#define OPL_OPL3_MAME_ADAPTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "ymf262.h"

/*  ymf262_init returns a heap-allocated OPL3* (opaque to us).  We
    keep that pointer inside opl3_chip so callers can keep treating
    the chip as a value-type, matching the nuked/ and opal/ APIs.  */
typedef struct {
	void* impl;     /* OPL3* from ymf262_init() */
} opl3_chip;

/*  Buffered writes are a Nuked feature; ymf262 applies writes
    synchronously.  Defines kept for ABI parity with nuked/.        */
#define OPL_WRITEBUF_SIZE   1024
#define OPL_WRITEBUF_DELAY  2

/*  Standard YMF262 master clock = 14.318 MHz (NTSC colorburst x 4). */
#ifndef YMF262_MASTER_CLOCK_HZ
#define YMF262_MASTER_CLOCK_HZ  14318180
#endif

static inline void
OPL3_Reset(opl3_chip* chip, uint32_t samplerate)
{
	if (chip->impl) {
		ymf262_shutdown(chip->impl);
		chip->impl = 0;
	}

	chip->impl = ymf262_init((int)YMF262_MASTER_CLOCK_HZ,
							 (int)samplerate, 0, 0);
	ymf262_reset_chip(chip->impl);
}

static inline void
OPL3_WriteReg(opl3_chip* chip, uint16_t reg, uint8_t v)
{
	/*  YMF262 has two 8-bit ports (address/data) per bank, with the
	    bank selected by the high bit of the register address.
	    ymf262_write address layout: 0=bank0 addr, 1=bank0 data,
	    2=bank1 addr, 3=bank1 data.                                */
	int bank = (reg & 0x100) ? 2 : 0;
	ymf262_write(chip->impl, bank + 0, (int)(reg & 0xff));
	ymf262_write(chip->impl, bank + 1, (int)v);
}

static inline void
OPL3_WriteRegBuffered(opl3_chip* chip, uint16_t reg, uint8_t v)
{
	OPL3_WriteReg(chip, reg, v);
}

static inline void
OPL3_Generate(opl3_chip* chip, int16_t* stereo)
{
	/*  ymf262_update_one wants two int16_t buffers (CH.A, CH.B);
	    those are already the front-stereo pair after the chip
	    folds its 4 panned outputs.                                */
	int16_t* bufs[2] = { &stereo[0], &stereo[1] };
	ymf262_update_one(chip->impl, bufs, 1);
}

static inline void
OPL3_GenerateResampled(opl3_chip* chip, int16_t* stereo)
{
	OPL3_Generate(chip, stereo);
}

#ifdef __cplusplus
}
#endif

#endif
