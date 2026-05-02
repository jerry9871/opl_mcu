/*
    opl3.h - header-only adapter that exposes adlibemu (Ken Silverman /
    DOSBox lineage, vendored from ValleyBell/libvgm) under the same
    OPL3_* symbol set used by sequencer/seq_player.c.

    Trade-offs vs. the other cores:
      - Pure C, LGPL 2.1+ (link-friendly).
      - Floating-point envelope/LFO math (uses pow/sin in init; per-sample
        path uses doubles for envelope and Bit32s fixed-point for waves).
        Fine on M4F/M7 with a single-precision FPU; not viable on M0.
      - 18 channels, 4-op, full rhythm mode, OPL3 panning.
      - DOSBox's *legacy* OPL emulator (kept in DOSBox alongside dbopl
        as the "compat" core); the direct ancestor of dbopl.

    chip-pointer wrapping: OPL_DATA is heap-allocated by adlib_OPL3_init
    and cached behind opl3_chip.impl, matching the mame adapter shape.
*/
#ifndef OPL_ADLIBEMU_ADAPTER_H
#define OPL_ADLIBEMU_ADAPTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>
#include "adlibemu.h"

typedef struct {
    void* impl;     /* OPL_DATA* from adlib_OPL3_init() */
} opl3_chip;

#define OPL_WRITEBUF_SIZE   1024
#define OPL_WRITEBUF_DELAY  2

#ifndef YMF262_MASTER_CLOCK_HZ
#define YMF262_MASTER_CLOCK_HZ  14318180
#endif

/*  adlibemu's INT32 samples nominally peak around ~70000 at full
    volume across 18 active channels.  Dropping the master volume to
    0x8000 (half) yields headroom of ~2x before INT16 saturation,
    which matches the perceived loudness of the other cores on
    typical Adlib music.  Tweak ADLIBEMU_VOL if a song clips.        */
#ifndef ADLIBEMU_VOL
#define ADLIBEMU_VOL  0x8000
#endif

static inline int16_t opl3_adlibemu_clip_i32(int32_t s) {
    if (s >  32767) return  32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
}

static inline void
OPL3_Reset(opl3_chip* chip, uint32_t samplerate)
{
    if (chip->impl) {
        adlib_OPL3_stop(chip->impl);
        chip->impl = 0;
    }
    chip->impl = adlib_OPL3_init((UINT32)YMF262_MASTER_CLOCK_HZ,
                                 (UINT32)samplerate);
    adlib_OPL3_reset(chip->impl);
    adlib_OPL3_set_volume(chip->impl, ADLIBEMU_VOL);
}

static inline void
OPL3_WriteReg(opl3_chip* chip, uint16_t reg, uint8_t v)
{
    /*  adlib_OPL3_writeIO uses a 4-port I/O surface:
          addr=0 -> latch bank-0 register address
          addr=1 -> write data to latched address
          addr=2 -> latch bank-1 register address (OR'd with 0x100)
          addr=3 -> write data to latched address                   */
    UINT8 addr_port = (reg & 0x100) ? 2 : 0;
    adlib_OPL3_writeIO(chip->impl, addr_port,     (UINT8)(reg & 0xff));
    adlib_OPL3_writeIO(chip->impl, addr_port + 1, v);
}

static inline void
OPL3_WriteRegBuffered(opl3_chip* chip, uint16_t reg, uint8_t v)
{
    OPL3_WriteReg(chip, reg, v);
}

static inline void
OPL3_Generate(opl3_chip* chip, int16_t* stereo)
{
    /*  adlib_OPL3_getsample expects DEV_SMPL** = INT32**, one
        accumulator buffer per channel.  We render 1 frame, then
        clip into the caller's int16 stereo pair.                   */
    INT32 l = 0, r = 0;
    INT32* bufs[2] = { &l, &r };
    adlib_OPL3_getsample(chip->impl, 1, bufs);
    stereo[0] = opl3_adlibemu_clip_i32(l);
    stereo[1] = opl3_adlibemu_clip_i32(r);
}

static inline void
OPL3_GenerateResampled(opl3_chip* chip, int16_t* stereo)
{
    OPL3_Generate(chip, stereo);
}

#ifdef __cplusplus
}
#endif

#endif /* OPL_ADLIBEMU_ADAPTER_H */
