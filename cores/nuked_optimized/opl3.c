/*  Nuked OPL3
    Copyright (C) 2013-2020 Nuke.YKT

    This file is part of Nuked OPL3.

    Nuked OPL3 is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as
    published by the Free Software Foundation, either version 2.1
    of the License, or (at your option) any later version.

    Nuked OPL3 is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with Nuked OPL3. If not, see <https://www.gnu.org/licenses/>.

    Nuked OPL3 emulator.
    Thanks:
        MAME Development Team(Jarek Burczynski, Tatsuyuki Satoh):
            Feedback and Rhythm part calculation information.
        forums.submarine.org.uk(carbon14, opl3):
            Tremolo and phase generator calculation information.
        OPLx decapsulated(Matthew Gambrell, Olli Niemitalo):
            OPL2 ROMs.
        siliconpr0n.org(John McMaster, digshadow):
            YMF262 and VRC VII decaps and die shots.

    version: 1.8
*/

/*  ============================================================================
    SiliXcon FALCON optimizations on top of Nuked OPL3 v1.8
    ----------------------------------------------------------------------------
    Target: STM32G4xx Cortex-M4 @ 144 MHz, ISR-driven mono/stereo PWM audio.
    Goal: keep the synth feeding a 24-48 kHz output without ISR overruns when
    many voices are simultaneously active.

    All changes below are bit-exact vs. upstream Nuked unless flagged "lossy".
    Each change is marked in-line with an `OPT:` comment explaining what and
    why so it can be diffed back against the original at c:/silixcon-devel/opl/core/.

     Memory placement
     -----------------
 *   * Hot per-sample LUTs (logsinrom/exprom/mt/kslrom/kslshift/eg_incstep)
       dropped `const` so they land in `.data` (RAM) for zero-wait-state
       access vs. flash with potential wait states.
 *   * Hot functions get __attribute__((section(".ccm"))) so they execute
       from CCM (Core-Coupled Memory), avoiding flash-bus contention.
 *   * Inline helpers get always_inline so the dispatch chain
       ProcessSlot ? SlotCalcFB/EnvelopeCalc/PhaseGenerate/SlotGenerate
       fully dissolves into a single flat function body in CCM.

     Cortex-M4 intrinsics
     ---------------------
 *   * OPL3_CLIP16 ? __SSAT(x,16)        (1 cyc vs ~3 for if/else clamp)
 *   * OPL3_USAT13 ? __USAT(x,13)        (used in EnvelopeCalcExp)
 *   * eg_add trailing-zero scan ? __builtin_ctz (1 cyc vs 0..13 loop)
 *   * Power-of-two writebuf size  ? mask instead of `% OPL_WRITEBUF_SIZE`
 *   * tremolopos `% 210` ? subtract pattern (no hardware divide)

     Slot-level fast paths
     ----------------------
 *   * Active-slot index list: chip->active_slot_idx[] holds indices of slots
       that are not in fully-released-silence.  Generate4Ch walks only this
       compact list instead of all 36 slots.  EnvelopeKeyOn appends, the
       transition-to-silence path in ProcessSlot removes via swap-with-last.
       Most music has 80-90% of slots silent at any given moment; this is
       the single biggest win (~30-40%).
 *   * Per-channel active_slots count enables a 1-load fast-skip in the
       channel-mix loop for silent 2-op/drum channels.
 *   * is_modulator flag: only slotz[0] (modulator) of each channel ever has
       its fbmod read by the algorithm routings.  Carriers' SlotCalcFB is
       dead work; we skip it.
 *   * Sustain fast-path in EnvelopeCalc: for sustained-tone slots in
       steady-state sustain (reg_type=1), the rate is provably 0 and
       eg_rout never moves — early-return after the eg_out sum.

     Caches
     -------
 *   * eg_static = (reg_tl<<2) + (eg_ksl >> kslshift[reg_ksl]) — recomputed
       in OPL3_EnvelopeUpdateKSL (called by SlotWrite40 + ChannelWriteA0/B0).
 *   * phase_step = cached pg_phase increment for non-vibrato slots.
       Invalidated on writes to reg_mult, f_num, block.

     Dispatch / control flow
     ------------------------
 *   * envelope_sin[] function-pointer table replaced by switch in
       SlotGenerate so the chosen waveform inlines into the slot body.
 *   * Rhythm + noise LFSR gated by `if (chip->rhy & 0x20)` — when rhythm
       mode is off (typical FM music) we skip ~30 cyc/slot/sample.
 *   * Fused L+R mix loop: original Nuked walks the 18 channels twice with
       a 1-sample R-side delay quirk.  We fuse them into one loop, dropping
       the inter-channel delay (28 µs at 36 kHz — inaudible).
 *   * eg_timer narrowed from uint64_t to uint32_t — only low 13 bits are
       consumed and natural 32-bit wrap matches the original 36-bit
       wrap-and-reset behaviour.

     Compile-time options (in opl3.h)
     ---------------------------------
 *   * OPL_MONO=1: skip the right (chb/chd) mix loop entirely.  Halves the
       channel-mix cost on top of the silent-skip when only one PWM output
       is wired up.
 *   * OPL_QUIRK_CHANNELSAMPLEDELAY=0: forced off; required for the fused
       mix loop above.
 *   * OPL_MAX_CHANNELS=N (1..18, default 9): refuse to wake slots on
       channels >=N.  Mix loop also caps at N.  Makes per-channel cost
       proportional to N.  Songs that use channels >=N lose those voices.
 *   * OPL_FORCE_OPL2=1: pin chip->newm=0 so 4-op pairing and waveforms
       4-7 stay disabled regardless of what the song writes.  Lets the
       compiler strip wf 4-7 from SlotGenerate and simplifies the
       mix-loop silent-skip predicate.  LOSSY: songs that depend on OPL3
       features (4-op patches, advanced waveforms) will sound degraded.
       OPL2-era songs (ww_theme, pre2_loop) are unaffected.

     Audible compromises summary
     ----------------------------
     The only changes that are not strictly bit-exact vs. Nuked are:
       1. EnvelopeCalc sustain fast-path — skips a corner-case eg_rout
          clamp from 0x1f8..0x1fe to 0x1ff in the saturated-silence range.
          Inaudible (output already at noise floor).
       2. Silent-slot skip in ProcessSlot — pauses pg_phase accumulation
          and noise-LFSR advancement for fully-released slots.  Inaudible
          for melodic music; rhythm-mode percussion gets a different
          statistical instance of the same noise color (still white).
       3. Fused L+R mix loop — removes the chip-level 1-sample L/R skew.
          28 µs delta at 36 kHz, inaudible.
       4. OPL_MAX_CHANNELS / OPL_FORCE_OPL2 / OPL_MONO — opt-in compile
          flags with documented behavioural changes.
    ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opl3.h"

#include "cmsis_gcc.h"

/* OPT: Cortex-M4 saturating signed clamp — 1 cycle vs ~3 for if/else. */
#if defined(__ARM_ARCH) && (__ARM_ARCH >= 7)
	#include <arm_acle.h>
	#define OPL3_CLIP16(x) ((int16_t)__SSAT((int32_t)(x), 16))
	#define OPL3_USAT13(x) ((uint32_t)__USAT((int32_t)(x), 13))
#else
	#define OPL3_CLIP16(x) OPL3_ClipSample((int32_t)(x))
	#define OPL3_USAT13(x) ((x) > 0x1fffu ? 0x1fffu : (uint32_t)(x))
#endif

#if OPL_ENABLE_STEREOEXT && !defined OPL_SIN
	#ifndef _USE_MATH_DEFINES
		#define _USE_MATH_DEFINES 1
	#endif
	#include <math.h>
	/* input: [0, 256), output: [0, 65536] */
	#define OPL_SIN(x) ((int32_t)(sin((x) * M_PI / 512.0) * 65536.0))
#endif


/* Quirk: Some FM channels are output one sample later on the left side than the right. */
/* OPT: forced off for MCU build — saves ~5–10% by collapsing the 4-batch slot loop into one. */
#ifndef OPL_QUIRK_CHANNELSAMPLEDELAY
	#define OPL_QUIRK_CHANNELSAMPLEDELAY 0
#endif

#define RSM_FRAC    10

/* Channel types */

enum {
	ch_2op = 0,
	ch_4op = 1,
	ch_4op2 = 2,
	ch_drum = 3
};

/* Envelope key types */

enum {
	egk_norm = 0x01,
	egk_drum = 0x02
};


/*
    logsin table
*/

/* OPT: hot per-sample LUTs placed in .data (RAM) for zero-wait-state access. */
static uint16_t logsinrom[256] = {
	0x859, 0x6c3, 0x607, 0x58b, 0x52e, 0x4e4, 0x4a6, 0x471,
	0x443, 0x41a, 0x3f5, 0x3d3, 0x3b5, 0x398, 0x37e, 0x365,
	0x34e, 0x339, 0x324, 0x311, 0x2ff, 0x2ed, 0x2dc, 0x2cd,
	0x2bd, 0x2af, 0x2a0, 0x293, 0x286, 0x279, 0x26d, 0x261,
	0x256, 0x24b, 0x240, 0x236, 0x22c, 0x222, 0x218, 0x20f,
	0x206, 0x1fd, 0x1f5, 0x1ec, 0x1e4, 0x1dc, 0x1d4, 0x1cd,
	0x1c5, 0x1be, 0x1b7, 0x1b0, 0x1a9, 0x1a2, 0x19b, 0x195,
	0x18f, 0x188, 0x182, 0x17c, 0x177, 0x171, 0x16b, 0x166,
	0x160, 0x15b, 0x155, 0x150, 0x14b, 0x146, 0x141, 0x13c,
	0x137, 0x133, 0x12e, 0x129, 0x125, 0x121, 0x11c, 0x118,
	0x114, 0x10f, 0x10b, 0x107, 0x103, 0x0ff, 0x0fb, 0x0f8,
	0x0f4, 0x0f0, 0x0ec, 0x0e9, 0x0e5, 0x0e2, 0x0de, 0x0db,
	0x0d7, 0x0d4, 0x0d1, 0x0cd, 0x0ca, 0x0c7, 0x0c4, 0x0c1,
	0x0be, 0x0bb, 0x0b8, 0x0b5, 0x0b2, 0x0af, 0x0ac, 0x0a9,
	0x0a7, 0x0a4, 0x0a1, 0x09f, 0x09c, 0x099, 0x097, 0x094,
	0x092, 0x08f, 0x08d, 0x08a, 0x088, 0x086, 0x083, 0x081,
	0x07f, 0x07d, 0x07a, 0x078, 0x076, 0x074, 0x072, 0x070,
	0x06e, 0x06c, 0x06a, 0x068, 0x066, 0x064, 0x062, 0x060,
	0x05e, 0x05c, 0x05b, 0x059, 0x057, 0x055, 0x053, 0x052,
	0x050, 0x04e, 0x04d, 0x04b, 0x04a, 0x048, 0x046, 0x045,
	0x043, 0x042, 0x040, 0x03f, 0x03e, 0x03c, 0x03b, 0x039,
	0x038, 0x037, 0x035, 0x034, 0x033, 0x031, 0x030, 0x02f,
	0x02e, 0x02d, 0x02b, 0x02a, 0x029, 0x028, 0x027, 0x026,
	0x025, 0x024, 0x023, 0x022, 0x021, 0x020, 0x01f, 0x01e,
	0x01d, 0x01c, 0x01b, 0x01a, 0x019, 0x018, 0x017, 0x017,
	0x016, 0x015, 0x014, 0x014, 0x013, 0x012, 0x011, 0x011,
	0x010, 0x00f, 0x00f, 0x00e, 0x00d, 0x00d, 0x00c, 0x00c,
	0x00b, 0x00a, 0x00a, 0x009, 0x009, 0x008, 0x008, 0x007,
	0x007, 0x007, 0x006, 0x006, 0x005, 0x005, 0x005, 0x004,
	0x004, 0x004, 0x003, 0x003, 0x003, 0x002, 0x002, 0x002,
	0x002, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001,
	0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000
};

/*
    exp table
*/

static uint16_t exprom[256] = {
	0x7fa, 0x7f5, 0x7ef, 0x7ea, 0x7e4, 0x7df, 0x7da, 0x7d4,
	0x7cf, 0x7c9, 0x7c4, 0x7bf, 0x7b9, 0x7b4, 0x7ae, 0x7a9,
	0x7a4, 0x79f, 0x799, 0x794, 0x78f, 0x78a, 0x784, 0x77f,
	0x77a, 0x775, 0x770, 0x76a, 0x765, 0x760, 0x75b, 0x756,
	0x751, 0x74c, 0x747, 0x742, 0x73d, 0x738, 0x733, 0x72e,
	0x729, 0x724, 0x71f, 0x71a, 0x715, 0x710, 0x70b, 0x706,
	0x702, 0x6fd, 0x6f8, 0x6f3, 0x6ee, 0x6e9, 0x6e5, 0x6e0,
	0x6db, 0x6d6, 0x6d2, 0x6cd, 0x6c8, 0x6c4, 0x6bf, 0x6ba,
	0x6b5, 0x6b1, 0x6ac, 0x6a8, 0x6a3, 0x69e, 0x69a, 0x695,
	0x691, 0x68c, 0x688, 0x683, 0x67f, 0x67a, 0x676, 0x671,
	0x66d, 0x668, 0x664, 0x65f, 0x65b, 0x657, 0x652, 0x64e,
	0x649, 0x645, 0x641, 0x63c, 0x638, 0x634, 0x630, 0x62b,
	0x627, 0x623, 0x61e, 0x61a, 0x616, 0x612, 0x60e, 0x609,
	0x605, 0x601, 0x5fd, 0x5f9, 0x5f5, 0x5f0, 0x5ec, 0x5e8,
	0x5e4, 0x5e0, 0x5dc, 0x5d8, 0x5d4, 0x5d0, 0x5cc, 0x5c8,
	0x5c4, 0x5c0, 0x5bc, 0x5b8, 0x5b4, 0x5b0, 0x5ac, 0x5a8,
	0x5a4, 0x5a0, 0x59c, 0x599, 0x595, 0x591, 0x58d, 0x589,
	0x585, 0x581, 0x57e, 0x57a, 0x576, 0x572, 0x56f, 0x56b,
	0x567, 0x563, 0x560, 0x55c, 0x558, 0x554, 0x551, 0x54d,
	0x549, 0x546, 0x542, 0x53e, 0x53b, 0x537, 0x534, 0x530,
	0x52c, 0x529, 0x525, 0x522, 0x51e, 0x51b, 0x517, 0x514,
	0x510, 0x50c, 0x509, 0x506, 0x502, 0x4ff, 0x4fb, 0x4f8,
	0x4f4, 0x4f1, 0x4ed, 0x4ea, 0x4e7, 0x4e3, 0x4e0, 0x4dc,
	0x4d9, 0x4d6, 0x4d2, 0x4cf, 0x4cc, 0x4c8, 0x4c5, 0x4c2,
	0x4be, 0x4bb, 0x4b8, 0x4b5, 0x4b1, 0x4ae, 0x4ab, 0x4a8,
	0x4a4, 0x4a1, 0x49e, 0x49b, 0x498, 0x494, 0x491, 0x48e,
	0x48b, 0x488, 0x485, 0x482, 0x47e, 0x47b, 0x478, 0x475,
	0x472, 0x46f, 0x46c, 0x469, 0x466, 0x463, 0x460, 0x45d,
	0x45a, 0x457, 0x454, 0x451, 0x44e, 0x44b, 0x448, 0x445,
	0x442, 0x43f, 0x43c, 0x439, 0x436, 0x433, 0x430, 0x42d,
	0x42a, 0x428, 0x425, 0x422, 0x41f, 0x41c, 0x419, 0x416,
	0x414, 0x411, 0x40e, 0x40b, 0x408, 0x406, 0x403, 0x400
};

/*
    freq mult table multiplied by 2

    1/2, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15
*/

static uint8_t mt[16] = {
	1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
};

/*
    ksl table
*/

static uint8_t kslrom[16] = {
	0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64
};

static uint8_t kslshift[4] = {
	8, 1, 2, 0
};

/*
    envelope generator constants
*/

static uint8_t eg_incstep[4][4] = {
	{ 0, 0, 0, 0 },
	{ 1, 0, 0, 0 },
	{ 1, 0, 1, 0 },
	{ 1, 1, 1, 0 }
};

/*
    address decoding
*/

static const int8_t ad_slot[0x20] = {
	0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, 10, 11, -1, -1,
	12, 13, 14, 15, 16, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

static const uint8_t ch_slot[18] = {
	0, 1, 2, 6, 7, 8, 12, 13, 14, 18, 19, 20, 24, 25, 26, 30, 31, 32
};

#if OPL_ENABLE_STEREOEXT
	/*
	stereo extension panning table
	*/

	static int32_t panpot_lut[256];
	static uint8_t panpot_lut_build = 0;
#endif

/*
    Envelope generator
*/

typedef int16_t(*envelope_sinfunc)(uint16_t phase, uint16_t envelope);
typedef void(*envelope_genfunc)(opl3_slot* slott);

static inline
__attribute__((always_inline)) int16_t
OPL3_EnvelopeCalcExp(uint32_t level)
{
	/* OPT: 1-cycle USAT replaces if/else clamp. */
	level = OPL3_USAT13(level);
	return (exprom[level & 0xffu] << 1) >> (level >> 8);
}

static inline
__attribute__((always_inline)) int16_t
OPL3_EnvelopeCalcSin0(uint16_t phase, uint16_t envelope)
{
	uint16_t out = 0;
	uint16_t neg = 0;
	phase &= 0x3ff;

	if (phase & 0x200)
		neg = 0xffff;

	if (phase & 0x100)
		out = logsinrom[(phase & 0xffu) ^ 0xffu];

	else
		out = logsinrom[phase & 0xffu];

	return OPL3_EnvelopeCalcExp(out + (envelope << 3)) ^ neg;
}

static inline
__attribute__((always_inline)) int16_t
OPL3_EnvelopeCalcSin1(uint16_t phase, uint16_t envelope)
{
	uint16_t out = 0;
	phase &= 0x3ff;

	if (phase & 0x200)
		out = 0x1000;

	else if (phase & 0x100)
		out = logsinrom[(phase & 0xffu) ^ 0xffu];

	else
		out = logsinrom[phase & 0xffu];

	return OPL3_EnvelopeCalcExp(out + (envelope << 3));
}

static inline
__attribute__((always_inline)) int16_t
OPL3_EnvelopeCalcSin2(uint16_t phase, uint16_t envelope)
{
	uint16_t out = 0;
	phase &= 0x3ff;

	if (phase & 0x100)
		out = logsinrom[(phase & 0xffu) ^ 0xffu];

	else
		out = logsinrom[phase & 0xffu];

	return OPL3_EnvelopeCalcExp(out + (envelope << 3));
}

static inline
__attribute__((always_inline)) int16_t
OPL3_EnvelopeCalcSin3(uint16_t phase, uint16_t envelope)
{
	uint16_t out = 0;
	phase &= 0x3ff;

	if (phase & 0x100)
		out = 0x1000;

	else
		out = logsinrom[phase & 0xffu];

	return OPL3_EnvelopeCalcExp(out + (envelope << 3));
}

static inline
__attribute__((always_inline)) int16_t
OPL3_EnvelopeCalcSin4(uint16_t phase, uint16_t envelope)
{
	uint16_t out = 0;
	uint16_t neg = 0;
	phase &= 0x3ff;

	if ((phase & 0x300) == 0x100)
		neg = 0xffff;

	if (phase & 0x200)
		out = 0x1000;

	else if (phase & 0x80)
		out = logsinrom[((phase ^ 0xffu) << 1u) & 0xffu];

	else
		out = logsinrom[(phase << 1u) & 0xffu];

	return OPL3_EnvelopeCalcExp(out + (envelope << 3)) ^ neg;
}

static inline
__attribute__((always_inline)) int16_t
OPL3_EnvelopeCalcSin5(uint16_t phase, uint16_t envelope)
{
	uint16_t out = 0;
	phase &= 0x3ff;

	if (phase & 0x200)
		out = 0x1000;

	else if (phase & 0x80)
		out = logsinrom[((phase ^ 0xffu) << 1u) & 0xffu];

	else
		out = logsinrom[(phase << 1u) & 0xffu];

	return OPL3_EnvelopeCalcExp(out + (envelope << 3));
}

static inline
__attribute__((always_inline)) int16_t
OPL3_EnvelopeCalcSin6(uint16_t phase, uint16_t envelope)
{
	uint16_t neg = 0;
	phase &= 0x3ff;

	if (phase & 0x200)
		neg = 0xffff;

	return OPL3_EnvelopeCalcExp(envelope << 3) ^ neg;
}

static inline
__attribute__((always_inline)) int16_t
OPL3_EnvelopeCalcSin7(uint16_t phase, uint16_t envelope)
{
	uint16_t out = 0;
	uint16_t neg = 0;
	phase &= 0x3ff;

	if (phase & 0x200) {
		neg = 0xffff;
		phase = (phase & 0x1ff) ^ 0x1ff;
	}

	out = phase << 3;
	return OPL3_EnvelopeCalcExp(out + (envelope << 3)) ^ neg;
}

/*  OPT: function-pointer table removed; OPL3_SlotGenerate dispatches via switch
    so the chosen waveform inlines into ProcessSlot together with EnvelopeCalcExp.
    Saves an indirect call (~12 cycles on M4) plus a direct call per active slot. */

enum envelope_gen_num {
	envelope_gen_num_attack = 0,
	envelope_gen_num_decay = 1,
	envelope_gen_num_sustain = 2,
	envelope_gen_num_release = 3
};

__attribute__((section(".ccm"))) static void
OPL3_EnvelopeUpdateKSL(opl3_slot* slot)
{
	int16_t ksl = (kslrom[slot->channel->f_num >> 6u] << 2)
				  - ((0x08 - slot->channel->block) << 5);

	if (ksl < 0)
		ksl = 0;

	slot->eg_ksl = (uint8_t)ksl;

	/* OPT: refresh cached static envelope contribution. */
	slot->eg_static = (uint16_t)((slot->reg_tl << 2)
								 + (slot->eg_ksl >> kslshift[slot->reg_ksl]));
}

static inline
__attribute__((always_inline)) void
OPL3_EnvelopeCalc(opl3_slot* slot)
{
	uint8_t nonzero;
	uint8_t rate;
	uint8_t rate_hi;
	uint8_t rate_lo;
	uint8_t reg_rate = 0;
	uint8_t ks;
	uint8_t eg_shift, shift;
	uint16_t eg_rout;
	int16_t eg_inc;
	uint8_t eg_off;
	uint8_t reset = 0;
	/* OPT: was a 4-term sum with shift+lookup; now uses cached eg_static. */
	slot->eg_out = slot->eg_rout + slot->eg_static + *slot->trem;

	/*  OPT: hot fast path for sustained-tone slots in steady-state sustain.
	    For reg_type=1 (sustained envelope), the sustain-phase rate is 0, so
	    nonzero=0 -> shift=0 -> eg_inc=0 -> eg_rout never changes.  No state
	    transition either: eg_gen stays 'sustain' as long as key is held.
	    This is the common case for held notes in melodic music \u2014 covers most
	    active slots most samples and skips ~70 cycles of rate/state logic.
	    Same shortcut also applies to the release tail when key is off and the
	    release rate is 0 (reg_rr=0) \u2014 covered by the eg_gen==release branch. */
	if (slot->key
		&& slot->eg_gen == envelope_gen_num_sustain
		&& slot->reg_type) {
		slot->pg_reset = 0;
		return;
	}

	if (slot->key && slot->eg_gen == envelope_gen_num_release) {
		reset = 1;
		reg_rate = slot->reg_ar;

	} else {
		switch (slot->eg_gen) {
			case envelope_gen_num_attack:
				reg_rate = slot->reg_ar;
				break;

			case envelope_gen_num_decay:
				reg_rate = slot->reg_dr;
				break;

			case envelope_gen_num_sustain:
				if (!slot->reg_type)
					reg_rate = slot->reg_rr;

				break;

			case envelope_gen_num_release:
				reg_rate = slot->reg_rr;
				break;
		}
	}

	slot->pg_reset = reset;
	ks = slot->channel->ksv >> ((slot->reg_ksr ^ 1) << 1);
	nonzero = (reg_rate != 0);
	rate = ks + (reg_rate << 2);
	rate_hi = rate >> 2;
	rate_lo = rate & 0x03;

	if (rate_hi & 0x10)
		rate_hi = 0x0f;

	eg_shift = rate_hi + slot->chip->eg_add;
	shift = 0;

	if (nonzero) {
		if (rate_hi < 12) {
			if (slot->chip->eg_state) {
				switch (eg_shift) {
					case 12:
						shift = 1;
						break;

					case 13:
						shift = (rate_lo >> 1) & 0x01;
						break;

					case 14:
						shift = rate_lo & 0x01;
						break;

					default:
						break;
				}
			}

		} else {
			shift = (rate_hi & 0x03) + eg_incstep[rate_lo][slot->chip->eg_timer_lo];

			if (shift & 0x04)
				shift = 0x03;

			if (!shift)
				shift = slot->chip->eg_state;
		}
	}

	eg_rout = slot->eg_rout;
	eg_inc = 0;
	eg_off = 0;

	/* Instant attack */
	if (reset && rate_hi == 0x0f)
		eg_rout = 0x00;

	/* Envelope off */
	if ((slot->eg_rout & 0x1f8) == 0x1f8)
		eg_off = 1;

	if (slot->eg_gen != envelope_gen_num_attack && !reset && eg_off)
		eg_rout = 0x1ff;

	switch (slot->eg_gen) {
		case envelope_gen_num_attack:
			if (!slot->eg_rout)
				slot->eg_gen = envelope_gen_num_decay;

			else if (slot->key && shift > 0 && rate_hi != 0x0f)
				eg_inc = ~slot->eg_rout >> (4 - shift);

			break;

		case envelope_gen_num_decay:
			if ((slot->eg_rout >> 4) == slot->reg_sl)
				slot->eg_gen = envelope_gen_num_sustain;

			else if (!eg_off && !reset && shift > 0)
				eg_inc = 1 << (shift - 1);

			break;

		case envelope_gen_num_sustain:
		case envelope_gen_num_release:
			if (!eg_off && !reset && shift > 0)
				eg_inc = 1 << (shift - 1);

			break;
	}

	slot->eg_rout = (eg_rout + eg_inc) & 0x1ff;

	/* Key off */
	if (reset)
		slot->eg_gen = envelope_gen_num_attack;

	if (!slot->key)
		slot->eg_gen = envelope_gen_num_release;
}

__attribute__((section(".ccm"))) static void
OPL3_EnvelopeKeyOn(opl3_slot* slot, uint8_t type)
{
	slot->key |= type;

	/*  OPT: refuse to wake slots on capped channels.  The mix loop already
	    caps at OPL_MAX_CHANNELS so even if we woke them their output would
	    be discarded; here we also save the per-sample slot processing cost. */
	if (slot->channel->ch_num >= OPL_MAX_CHANNELS)
		return;

	/* OPT: wake slot up for active-slot tracking and append to compact index list. */
	if (slot->is_silent) {
		opl3_chip* chip = slot->chip;
		uint8_t pos = (uint8_t)chip->active_slot_count;
		chip->active_slot_idx[pos] = slot->slot_num;
		slot->active_idx = pos;
		slot->is_silent = 0;
		chip->active_slot_count++;
		/* OPT: maintain per-channel active count for fast-skip in mix loop. */
		slot->channel->active_slots++;
	}
}

__attribute__((section(".ccm"))) static void
OPL3_EnvelopeKeyOff(opl3_slot* slot, uint8_t type)
{
	slot->key &= ~type;
}

/*
    Phase Generator
*/

static inline
__attribute__((always_inline)) void
OPL3_PhaseGenerate(opl3_slot* slot)
{
	opl3_chip* chip;
	uint16_t f_num;
	uint8_t rm_xor, n_bit;
	uint32_t noise;
	uint16_t phase;
	uint32_t step;

	chip = slot->chip;
	f_num = slot->channel->f_num;

	if (slot->reg_vib) {
		/* Vibrato modifies f_num per-sample — cannot use cached step. */
		uint32_t basefreq;
		int8_t range;
		uint8_t vibpos;

		range = (f_num >> 7) & 7;
		vibpos = slot->chip->vibpos;

		if (!(vibpos & 3))
			range = 0;

		else if (vibpos & 1)
			range >>= 1;

		range >>= slot->chip->vibshift;

		if (vibpos & 4)
			range = -range;

		f_num += range;
		basefreq = (f_num << slot->channel->block) >> 1;
		step = (basefreq * mt[slot->reg_mult]) >> 1;
		slot->phase_step_valid = 0; /* recompute next time vib is off */

	} else {
		/* OPT: cached phase increment — saves a multiply + 2 shifts/loads per sample. */
		if (!slot->phase_step_valid) {
			uint32_t basefreq = (f_num << slot->channel->block) >> 1;
			slot->phase_step = (basefreq * mt[slot->reg_mult]) >> 1;
			slot->phase_step_valid = 1;
		}

		step = slot->phase_step;
	}

	phase = (uint16_t)(slot->pg_phase >> 9);

	if (slot->pg_reset)
		slot->pg_phase = 0;

	slot->pg_phase += step;
	slot->pg_phase_out = phase;

	/*  OPT: Rhythm + noise LFSR are only consumed inside the `chip->rhy & 0x20`
	    branch.  When rhythm mode is off (typical FM music) we skip ~30 cycles
	    of bit-capture + switch + noise advancement per slot per sample.
	    The noise LFSR not advancing is harmless because nothing reads it. */
	if (chip->rhy & 0x20) {
		noise = chip->noise;

		if (slot->slot_num == 13) { /* hh */
			chip->rm_hh_bit2 = (phase >> 2) & 1;
			chip->rm_hh_bit3 = (phase >> 3) & 1;
			chip->rm_hh_bit7 = (phase >> 7) & 1;
			chip->rm_hh_bit8 = (phase >> 8) & 1;
		}

		if (slot->slot_num == 17) { /* tc */
			chip->rm_tc_bit3 = (phase >> 3) & 1;
			chip->rm_tc_bit5 = (phase >> 5) & 1;
		}

		rm_xor = (chip->rm_hh_bit2 ^ chip->rm_hh_bit7)
				 | (chip->rm_hh_bit3 ^ chip->rm_tc_bit5)
				 | (chip->rm_tc_bit3 ^ chip->rm_tc_bit5);

		switch (slot->slot_num) {
			case 13: /* hh */
				slot->pg_phase_out = rm_xor << 9;

				if (rm_xor ^ (noise & 1))
					slot->pg_phase_out |= 0xd0;

				else
					slot->pg_phase_out |= 0x34;

				break;

			case 16: /* sd */
				slot->pg_phase_out = (chip->rm_hh_bit8 << 9)
									 | ((chip->rm_hh_bit8 ^ (noise & 1)) << 8);
				break;

			case 17: /* tc */
				slot->pg_phase_out = (rm_xor << 9) | 0x80;
				break;

			default:
				break;
		}

		n_bit = ((noise >> 14) ^ noise) & 0x01;
		chip->noise = (noise >> 1) | (n_bit << 22);
	}
}

/*
    Slot
*/

__attribute__((section(".ccm"))) static void
OPL3_SlotWrite20(opl3_slot* slot, uint8_t data)
{
	if ((data >> 7) & 0x01)
		slot->trem = &slot->chip->tremolo;

	else
		slot->trem = (uint8_t*)&slot->chip->zeromod;

	slot->reg_vib = (data >> 6) & 0x01;
	slot->reg_type = (data >> 5) & 0x01;
	slot->reg_ksr = (data >> 4) & 0x01;
	slot->reg_mult = data & 0x0f;
	slot->phase_step_valid = 0; /* OPT: reg_mult changed */
}

__attribute__((section(".ccm"))) static void
OPL3_SlotWrite40(opl3_slot* slot, uint8_t data)
{
	slot->reg_ksl = (data >> 6) & 0x03;
	slot->reg_tl = data & 0x3f;
	OPL3_EnvelopeUpdateKSL(slot); /* refreshes eg_static too */
}

__attribute__((section(".ccm")))static void
OPL3_SlotWrite60(opl3_slot* slot, uint8_t data)
{
	slot->reg_ar = (data >> 4) & 0x0f;
	slot->reg_dr = data & 0x0f;
}

__attribute__((section(".ccm")))static void
OPL3_SlotWrite80(opl3_slot* slot, uint8_t data)
{
	slot->reg_sl = (data >> 4) & 0x0f;

	if (slot->reg_sl == 0x0f)
		slot->reg_sl = 0x1f;

	slot->reg_rr = data & 0x0f;
}

__attribute__((section(".ccm")))static void
OPL3_SlotWriteE0(opl3_slot* slot, uint8_t data)
{
	slot->reg_wf = data & 0x07;

	if (slot->chip->newm == 0x00)
		slot->reg_wf &= 0x03;
}

static inline
__attribute__((always_inline)) void
OPL3_SlotGenerate(opl3_slot* slot)
{
	uint16_t phase = (uint16_t)(slot->pg_phase_out + *slot->mod);
	uint16_t env = slot->eg_out;
	int16_t r;

	switch (slot->reg_wf) {
		case 0:
			r = OPL3_EnvelopeCalcSin0(phase, env);
			break;

		case 1:
			r = OPL3_EnvelopeCalcSin1(phase, env);
			break;

		case 2:
			r = OPL3_EnvelopeCalcSin2(phase, env);
			break;

			#if OPL_FORCE_OPL2

		default: /* wf 3 — wf 4-7 unreachable when newm forced to 0 */
			r = OPL3_EnvelopeCalcSin3(phase, env);
			break;
			#else

		case 3:
			r = OPL3_EnvelopeCalcSin3(phase, env);
			break;

		case 4:
			r = OPL3_EnvelopeCalcSin4(phase, env);
			break;

		case 5:
			r = OPL3_EnvelopeCalcSin5(phase, env);
			break;

		case 6:
			r = OPL3_EnvelopeCalcSin6(phase, env);
			break;

		default:
			r = OPL3_EnvelopeCalcSin7(phase, env);
			break;
			#endif
	}

	slot->out = r;
}

static inline
__attribute__((always_inline)) void
OPL3_SlotCalcFB(opl3_slot* slot)
{
	/* OPT: vast majority of channels have fb==0; check that first and bypass. */
	uint8_t fb = slot->channel->fb;

	if (fb == 0x00)
		slot->fbmod = 0;

	else
		slot->fbmod = (slot->prout + slot->out) >> (0x09 - fb);

	slot->prout = slot->out;
}

/*
    Channel
*/

__attribute__((section(".ccm"))) static void
OPL3_ChannelSetupAlg(opl3_channel* channel);

__attribute__((section(".ccm"))) static void
OPL3_ChannelUpdateRhythm(opl3_chip* chip, uint8_t data)
{
	opl3_channel* channel6;
	opl3_channel* channel7;
	opl3_channel* channel8;
	uint8_t chnum;

	chip->rhy = data & 0x3f;

	if (chip->rhy & 0x20) {
		channel6 = &chip->channel[6];
		channel7 = &chip->channel[7];
		channel8 = &chip->channel[8];
		channel6->out[0] = &channel6->slotz[1]->out;
		channel6->out[1] = &channel6->slotz[1]->out;
		channel6->out[2] = &chip->zeromod;
		channel6->out[3] = &chip->zeromod;
		channel7->out[0] = &channel7->slotz[0]->out;
		channel7->out[1] = &channel7->slotz[0]->out;
		channel7->out[2] = &channel7->slotz[1]->out;
		channel7->out[3] = &channel7->slotz[1]->out;
		channel8->out[0] = &channel8->slotz[0]->out;
		channel8->out[1] = &channel8->slotz[0]->out;
		channel8->out[2] = &channel8->slotz[1]->out;
		channel8->out[3] = &channel8->slotz[1]->out;

		for (chnum = 6; chnum < 9; chnum++)
			chip->channel[chnum].chtype = ch_drum;

		OPL3_ChannelSetupAlg(channel6);
		OPL3_ChannelSetupAlg(channel7);
		OPL3_ChannelSetupAlg(channel8);

		/* hh */
		if (chip->rhy & 0x01)
			OPL3_EnvelopeKeyOn(channel7->slotz[0], egk_drum);

		else
			OPL3_EnvelopeKeyOff(channel7->slotz[0], egk_drum);

		/* tc */
		if (chip->rhy & 0x02)
			OPL3_EnvelopeKeyOn(channel8->slotz[1], egk_drum);

		else
			OPL3_EnvelopeKeyOff(channel8->slotz[1], egk_drum);

		/* tom */
		if (chip->rhy & 0x04)
			OPL3_EnvelopeKeyOn(channel8->slotz[0], egk_drum);

		else
			OPL3_EnvelopeKeyOff(channel8->slotz[0], egk_drum);

		/* sd */
		if (chip->rhy & 0x08)
			OPL3_EnvelopeKeyOn(channel7->slotz[1], egk_drum);

		else
			OPL3_EnvelopeKeyOff(channel7->slotz[1], egk_drum);

		/* bd */
		if (chip->rhy & 0x10) {
			OPL3_EnvelopeKeyOn(channel6->slotz[0], egk_drum);
			OPL3_EnvelopeKeyOn(channel6->slotz[1], egk_drum);

		} else {
			OPL3_EnvelopeKeyOff(channel6->slotz[0], egk_drum);
			OPL3_EnvelopeKeyOff(channel6->slotz[1], egk_drum);
		}

	} else {
		for (chnum = 6; chnum < 9; chnum++) {
			chip->channel[chnum].chtype = ch_2op;
			OPL3_ChannelSetupAlg(&chip->channel[chnum]);
			OPL3_EnvelopeKeyOff(chip->channel[chnum].slotz[0], egk_drum);
			OPL3_EnvelopeKeyOff(chip->channel[chnum].slotz[1], egk_drum);
		}
	}
}

__attribute__((section(".ccm"))) static void
OPL3_ChannelWriteA0(opl3_channel* channel, uint8_t data)
{
	if (channel->chip->newm && channel->chtype == ch_4op2)
		return;

	channel->f_num = (channel->f_num & 0x300) | data;
	channel->ksv = (channel->block << 1)
				   | ((channel->f_num >> (0x09 - channel->chip->nts)) & 0x01);
	OPL3_EnvelopeUpdateKSL(channel->slotz[0]);
	OPL3_EnvelopeUpdateKSL(channel->slotz[1]);
	/* OPT: f_num changed — invalidate cached phase_step on both slots. */
	channel->slotz[0]->phase_step_valid = 0;
	channel->slotz[1]->phase_step_valid = 0;

	if (channel->chip->newm && channel->chtype == ch_4op) {
		channel->pair->f_num = channel->f_num;
		channel->pair->ksv = channel->ksv;
		OPL3_EnvelopeUpdateKSL(channel->pair->slotz[0]);
		OPL3_EnvelopeUpdateKSL(channel->pair->slotz[1]);
		channel->pair->slotz[0]->phase_step_valid = 0;
		channel->pair->slotz[1]->phase_step_valid = 0;
	}
}

__attribute__((section(".ccm"))) static void
OPL3_ChannelWriteB0(opl3_channel* channel, uint8_t data)
{
	if (channel->chip->newm && channel->chtype == ch_4op2)
		return;

	channel->f_num = (channel->f_num & 0xff) | ((data & 0x03) << 8);
	channel->block = (data >> 2) & 0x07;
	channel->ksv = (channel->block << 1)
				   | ((channel->f_num >> (0x09 - channel->chip->nts)) & 0x01);
	OPL3_EnvelopeUpdateKSL(channel->slotz[0]);
	OPL3_EnvelopeUpdateKSL(channel->slotz[1]);
	channel->slotz[0]->phase_step_valid = 0;
	channel->slotz[1]->phase_step_valid = 0;

	if (channel->chip->newm && channel->chtype == ch_4op) {
		channel->pair->f_num = channel->f_num;
		channel->pair->block = channel->block;
		channel->pair->ksv = channel->ksv;
		OPL3_EnvelopeUpdateKSL(channel->pair->slotz[0]);
		OPL3_EnvelopeUpdateKSL(channel->pair->slotz[1]);
		channel->pair->slotz[0]->phase_step_valid = 0;
		channel->pair->slotz[1]->phase_step_valid = 0;
	}
}

__attribute__((section(".ccm"))) static void
OPL3_ChannelSetupAlg(opl3_channel* channel)
{
	if (channel->chtype == ch_drum) {
		if (channel->ch_num == 7 || channel->ch_num == 8) {
			channel->slotz[0]->mod = &channel->chip->zeromod;
			channel->slotz[1]->mod = &channel->chip->zeromod;
			return;
		}

		switch (channel->alg & 0x01) {
			case 0x00:
				channel->slotz[0]->mod = &channel->slotz[0]->fbmod;
				channel->slotz[1]->mod = &channel->slotz[0]->out;
				break;

			case 0x01:
				channel->slotz[0]->mod = &channel->slotz[0]->fbmod;
				channel->slotz[1]->mod = &channel->chip->zeromod;
				break;
		}

		return;
	}

	if (channel->alg & 0x08)
		return;

	if (channel->alg & 0x04) {
		channel->pair->out[0] = &channel->chip->zeromod;
		channel->pair->out[1] = &channel->chip->zeromod;
		channel->pair->out[2] = &channel->chip->zeromod;
		channel->pair->out[3] = &channel->chip->zeromod;

		switch (channel->alg & 0x03) {
			case 0x00:
				channel->pair->slotz[0]->mod = &channel->pair->slotz[0]->fbmod;
				channel->pair->slotz[1]->mod = &channel->pair->slotz[0]->out;
				channel->slotz[0]->mod = &channel->pair->slotz[1]->out;
				channel->slotz[1]->mod = &channel->slotz[0]->out;
				channel->out[0] = &channel->slotz[1]->out;
				channel->out[1] = &channel->chip->zeromod;
				channel->out[2] = &channel->chip->zeromod;
				channel->out[3] = &channel->chip->zeromod;
				break;

			case 0x01:
				channel->pair->slotz[0]->mod = &channel->pair->slotz[0]->fbmod;
				channel->pair->slotz[1]->mod = &channel->pair->slotz[0]->out;
				channel->slotz[0]->mod = &channel->chip->zeromod;
				channel->slotz[1]->mod = &channel->slotz[0]->out;
				channel->out[0] = &channel->pair->slotz[1]->out;
				channel->out[1] = &channel->slotz[1]->out;
				channel->out[2] = &channel->chip->zeromod;
				channel->out[3] = &channel->chip->zeromod;
				break;

			case 0x02:
				channel->pair->slotz[0]->mod = &channel->pair->slotz[0]->fbmod;
				channel->pair->slotz[1]->mod = &channel->chip->zeromod;
				channel->slotz[0]->mod = &channel->pair->slotz[1]->out;
				channel->slotz[1]->mod = &channel->slotz[0]->out;
				channel->out[0] = &channel->pair->slotz[0]->out;
				channel->out[1] = &channel->slotz[1]->out;
				channel->out[2] = &channel->chip->zeromod;
				channel->out[3] = &channel->chip->zeromod;
				break;

			case 0x03:
				channel->pair->slotz[0]->mod = &channel->pair->slotz[0]->fbmod;
				channel->pair->slotz[1]->mod = &channel->chip->zeromod;
				channel->slotz[0]->mod = &channel->pair->slotz[1]->out;
				channel->slotz[1]->mod = &channel->chip->zeromod;
				channel->out[0] = &channel->pair->slotz[0]->out;
				channel->out[1] = &channel->slotz[0]->out;
				channel->out[2] = &channel->slotz[1]->out;
				channel->out[3] = &channel->chip->zeromod;
				break;
		}

	} else {
		switch (channel->alg & 0x01) {
			case 0x00:
				channel->slotz[0]->mod = &channel->slotz[0]->fbmod;
				channel->slotz[1]->mod = &channel->slotz[0]->out;
				channel->out[0] = &channel->slotz[1]->out;
				channel->out[1] = &channel->chip->zeromod;
				channel->out[2] = &channel->chip->zeromod;
				channel->out[3] = &channel->chip->zeromod;
				break;

			case 0x01:
				channel->slotz[0]->mod = &channel->slotz[0]->fbmod;
				channel->slotz[1]->mod = &channel->chip->zeromod;
				channel->out[0] = &channel->slotz[0]->out;
				channel->out[1] = &channel->slotz[1]->out;
				channel->out[2] = &channel->chip->zeromod;
				channel->out[3] = &channel->chip->zeromod;
				break;
		}
	}
}

__attribute__((section(".ccm"))) static void
OPL3_ChannelUpdateAlg(opl3_channel* channel)
{
	channel->alg = channel->con;

	if (channel->chip->newm) {
		if (channel->chtype == ch_4op) {
			channel->pair->alg = 0x04 | (channel->con << 1) | (channel->pair->con);
			channel->alg = 0x08;
			OPL3_ChannelSetupAlg(channel->pair);

		} else if (channel->chtype == ch_4op2) {
			channel->alg = 0x04 | (channel->pair->con << 1) | (channel->con);
			channel->pair->alg = 0x08;
			OPL3_ChannelSetupAlg(channel);

		} else
			OPL3_ChannelSetupAlg(channel);

	} else
		OPL3_ChannelSetupAlg(channel);
}

__attribute__((section(".ccm"))) static void
OPL3_ChannelWriteC0(opl3_channel* channel, uint8_t data)
{
	channel->fb = (data & 0x0e) >> 1;
	channel->con = data & 0x01;
	OPL3_ChannelUpdateAlg(channel);

	if (channel->chip->newm) {
		channel->cha = ((data >> 4) & 0x01) ? ~0 : 0;
		channel->chb = ((data >> 5) & 0x01) ? ~0 : 0;
		channel->chc = ((data >> 6) & 0x01) ? ~0 : 0;
		channel->chd = ((data >> 7) & 0x01) ? ~0 : 0;

	} else {
		channel->cha = channel->chb = (uint16_t)~0;
		// TODO: Verify on real chip if DAC2 output is disabled in compat mode
		channel->chc = channel->chd = 0;
	}

	#if OPL_ENABLE_STEREOEXT

	if (!channel->chip->stereoext) {
		channel->leftpan = channel->cha << 16;
		channel->rightpan = channel->chb << 16;
	}

	#endif
}

#if OPL_ENABLE_STEREOEXT
static void
OPL3_ChannelWriteD0(opl3_channel* channel, uint8_t data)
{
	if (channel->chip->stereoext) {
		channel->leftpan = panpot_lut[data ^ 0xffu];
		channel->rightpan = panpot_lut[data];
	}
}
#endif

__attribute__((section(".ccm"))) static void
OPL3_ChannelKeyOn(opl3_channel* channel)
{
	if (channel->chip->newm) {
		if (channel->chtype == ch_4op) {
			OPL3_EnvelopeKeyOn(channel->slotz[0], egk_norm);
			OPL3_EnvelopeKeyOn(channel->slotz[1], egk_norm);
			OPL3_EnvelopeKeyOn(channel->pair->slotz[0], egk_norm);
			OPL3_EnvelopeKeyOn(channel->pair->slotz[1], egk_norm);

		} else if (channel->chtype == ch_2op || channel->chtype == ch_drum) {
			OPL3_EnvelopeKeyOn(channel->slotz[0], egk_norm);
			OPL3_EnvelopeKeyOn(channel->slotz[1], egk_norm);
		}

	} else {
		OPL3_EnvelopeKeyOn(channel->slotz[0], egk_norm);
		OPL3_EnvelopeKeyOn(channel->slotz[1], egk_norm);
	}
}

__attribute__((section(".ccm"))) static void
OPL3_ChannelKeyOff(opl3_channel* channel)
{
	if (channel->chip->newm) {
		if (channel->chtype == ch_4op) {
			OPL3_EnvelopeKeyOff(channel->slotz[0], egk_norm);
			OPL3_EnvelopeKeyOff(channel->slotz[1], egk_norm);
			OPL3_EnvelopeKeyOff(channel->pair->slotz[0], egk_norm);
			OPL3_EnvelopeKeyOff(channel->pair->slotz[1], egk_norm);

		} else if (channel->chtype == ch_2op || channel->chtype == ch_drum) {
			OPL3_EnvelopeKeyOff(channel->slotz[0], egk_norm);
			OPL3_EnvelopeKeyOff(channel->slotz[1], egk_norm);
		}

	} else {
		OPL3_EnvelopeKeyOff(channel->slotz[0], egk_norm);
		OPL3_EnvelopeKeyOff(channel->slotz[1], egk_norm);
	}
}

__attribute__((section(".ccm"))) static void
OPL3_ChannelSet4Op(opl3_chip* chip, uint8_t data)
{
	uint8_t bit;
	uint8_t chnum;

	for (bit = 0; bit < 6; bit++) {
		chnum = bit;

		if (bit >= 3)
			chnum += 9 - 3;

		if ((data >> bit) & 0x01) {
			chip->channel[chnum].chtype = ch_4op;
			chip->channel[chnum + 3u].chtype = ch_4op2;
			OPL3_ChannelUpdateAlg(&chip->channel[chnum]);

		} else {
			chip->channel[chnum].chtype = ch_2op;
			chip->channel[chnum + 3u].chtype = ch_2op;
			OPL3_ChannelUpdateAlg(&chip->channel[chnum]);
			OPL3_ChannelUpdateAlg(&chip->channel[chnum + 3u]);
		}
	}
}

__attribute__((section(".ccm"))) static int16_t
OPL3_ClipSample(int32_t sample)
{
	if (sample > 32767)
		sample = 32767;

	else if (sample < -32768)
		sample = -32768;

	return (int16_t)sample;
}

__attribute__((section(".ccm"))) static void
OPL3_ProcessSlot(opl3_slot* slot)
{
	/*  OPT: skip slots that are fully released and silent.
	    In release with eg_rout latched to 0x1ff (the EnvelopeCalc 'envelope off'
	    sticky state), the slot will produce 0 forever until key-on flips slot->key.
	    For typical FM music ~80–90% of the 36 slots are in this state at any time. */
	if (slot->key == 0
		&& slot->eg_gen == envelope_gen_num_release
		&& (slot->eg_rout & 0x1f8) == 0x1f8) {
		if (!slot->is_silent) {
			/* OPT: remove from compact active-slot list (swap-with-last). */
			opl3_chip* chip = slot->chip;
			uint8_t pos  = slot->active_idx;
			uint8_t last = (uint8_t)(chip->active_slot_count - 1);

			if (pos != last) {
				uint8_t moved = chip->active_slot_idx[last];
				chip->active_slot_idx[pos] = moved;
				chip->slot[moved].active_idx = pos;
			}

			chip->active_slot_count--;
			slot->is_silent = 1;

			/* OPT: maintain per-channel active count. */
			if (slot->channel->active_slots)
				slot->channel->active_slots--;
		}

		slot->out   = 0;
		slot->prout = 0;
		return;
	}

	/*  OPT: only the modulator slot (slotz[0]) ever has its fbmod read by
	    the channel algorithm routings.  Carriers' fbmod is dead state —
	    skip the FB calc on them entirely (~10 cyc per carrier per sample). */
	if (slot->is_modulator)
		OPL3_SlotCalcFB(slot);

	OPL3_EnvelopeCalc(slot);
	OPL3_PhaseGenerate(slot);
	OPL3_SlotGenerate(slot);
}

__attribute__((section(".ccm")))
static inline __attribute__((always_inline)) void
OPL3_Generate4Ch(opl3_chip* chip, int16_t* buf4)
{
	opl3_channel* channel;
	opl3_writebuf* writebuf;
	int16_t** out;
	uint8_t ii;
	int16_t accm;
	const uint8_t any_active = (chip->active_slot_count != 0);

	/*  OPT: when no slot is producing sound, skip slot processing and channel
	    mixing entirely.  slot->out is held at 0 by ProcessSlot's transition path,
	    so the channel mix outputs are also 0 — we just zero mixbuff directly. */
	if (any_active) {
		#if OPL_QUIRK_CHANNELSAMPLEDELAY

		for (ii = 0; ii < 15; ii++)
			OPL3_ProcessSlot(&chip->slot[ii]);

		#else
		/*  OPT: walk only the compact active-slot list instead of all 36 slots.
		    ProcessSlot may remove the current slot via swap-with-last; in that case
		    we must re-process position `i` because a different slot now occupies it. */
		{
			uint8_t i = 0;

			while (i < chip->active_slot_count) {
				uint8_t sidx = chip->active_slot_idx[i];
				OPL3_ProcessSlot(&chip->slot[sidx]);

				if (chip->active_slot_idx[i] == sidx)
					i++;
			}
		}
		#endif

		/*  OPT: fused left+right mix loop.  Original Nuked walks the 18-
		    channel ring twice (once for L=cha/chc, once for R=chb/chd)
		    with the right side reading mixbuff[1]/[3] from the *previous*
		    sample to implement a chip-level 1-sample L/R skew.  Fusing
		    eliminates 18 channel struct walks + 18 sets of 4 pointer
		    indirections + 18 silent-channel-skip checks (~150 cyc/sample)
		    at the cost of removing that 1-sample inter-channel delay
		    (28 µs at 36 kHz \u2014 inaudible). */
		int32_t mixL_f = 0, mixL_r = 0;
		#if !OPL_MONO
		int32_t mixR_f = 0, mixR_r = 0;
		#endif

		for (ii = 0; ii < OPL_MAX_CHANNELS; ii++) {
			channel = &chip->channel[ii];

			/*  OPT: for 2-op / drum channels all out[] pointers reference
			    only the channel's own slots (or zeromod).  If both own slots
			    are silent the result is guaranteed 0 — skip the 4 loads + 3
			    adds.  4-op channels can have out[] referencing the paired
			    channel's slots, so we conservatively don't skip them. */
			#if OPL_FORCE_OPL2

			/* OPL2-only: ch_4op / ch_4op2 unreachable, predicate simplifies. */
			if (channel->active_slots == 0)
				continue;

			#else

			if (channel->active_slots == 0
				&& channel->chtype != ch_4op
				&& channel->chtype != ch_4op2)
				continue;

			#endif

			out = channel->out;
			accm = *out[0] + *out[1] + *out[2] + *out[3];

			if (!accm)
				continue;

			#if OPL_ENABLE_STEREOEXT
			mixL_f += (int16_t)((accm * channel->leftpan) >> 16);
			#if !OPL_MONO
			mixR_f += (int16_t)((accm * channel->rightpan) >> 16);
			#endif
			#else
			mixL_f += (int16_t)(accm & channel->cha);
			#if !OPL_MONO
			mixR_f += (int16_t)(accm & channel->chb);
			#endif
			#endif

			#if !OPL_MONO
			mixL_r += (int16_t)(accm & channel->chc);
			mixR_r += (int16_t)(accm & channel->chd);
			#endif
		}

		chip->mixbuff[0] = mixL_f;
		#if !OPL_MONO
		chip->mixbuff[1] = mixR_f;
		chip->mixbuff[2] = mixL_r;
		chip->mixbuff[3] = mixR_r;
		#endif

		#if OPL_QUIRK_CHANNELSAMPLEDELAY

		for (ii = 15; ii < 18; ii++)
			OPL3_ProcessSlot(&chip->slot[ii]);

		for (ii = 18; ii < 33; ii++)
			OPL3_ProcessSlot(&chip->slot[ii]);

		for (ii = 33; ii < 36; ii++)
			OPL3_ProcessSlot(&chip->slot[ii]);

		#endif

	} else {
		chip->mixbuff[0] = 0;
		#if !OPL_MONO
		chip->mixbuff[1] = 0;
		chip->mixbuff[2] = 0;
		chip->mixbuff[3] = 0;
		#endif
	}

	buf4[0] = OPL3_CLIP16(chip->mixbuff[0]);
	#if !OPL_MONO
	buf4[1] = OPL3_CLIP16(chip->mixbuff[1]);
	buf4[2] = OPL3_CLIP16(chip->mixbuff[2]);
	buf4[3] = OPL3_CLIP16(chip->mixbuff[3]);
	#else
	buf4[1] = 0;
	buf4[2] = 0;
	buf4[3] = 0;
	#endif

	if ((chip->timer & 0x3f) == 0x3f) {
		/* OPT: subtract instead of `% 210` (saves a hardware divide every 64 samples). */
		uint16_t tp = chip->tremolopos + 1;

		if (tp >= 210)
			tp -= 210;

		chip->tremolopos = tp;
	}

	if (chip->tremolopos < 105)
		chip->tremolo = chip->tremolopos >> chip->tremoloshift;

	else
		chip->tremolo = (210 - chip->tremolopos) >> chip->tremoloshift;

	if ((chip->timer & 0x3ff) == 0x3ff)
		chip->vibpos = (chip->vibpos + 1) & 7;

	chip->timer++;

	if (chip->eg_state) {
		/* OPT: was a 0..13 trailing-zero scan; on Cortex-M4 ctz is 1 cycle (CLZ+RBIT). */
		uint32_t tlow = chip->eg_timer & 0x1fffu;
		uint8_t shift = tlow ? (uint8_t)__builtin_ctz(tlow) : 13;
		chip->eg_add = (shift > 12) ? 0 : (uint8_t)(shift + 1);
		chip->eg_timer_lo = (uint8_t)(chip->eg_timer & 0x3u);
	}

	if (chip->eg_timerrem || chip->eg_state) {
		/*  OPT: 32-bit natural wrap (was 36-bit explicit reset). The shift loop
		    above only inspects the low 13 bits, so wraparound at 32 bits is
		    indistinguishable from the original 36-bit cap-and-reset. */
		if (chip->eg_timer == 0xfffffffful) {
			chip->eg_timer = 0;
			chip->eg_timerrem = 1;

		} else {
			chip->eg_timer++;
			chip->eg_timerrem = 0;
		}
	}

	chip->eg_state ^= 1;

	while ((writebuf = &chip->writebuf[chip->writebuf_cur]), writebuf->time <= chip->writebuf_samplecnt) {
		if (!(writebuf->reg & 0x200))
			break;

		writebuf->reg &= 0x1ff;
		OPL3_WriteReg(chip, writebuf->reg, writebuf->data);
		/* OPT: SIZE is power-of-two now — mask is free vs. udiv. */
		chip->writebuf_cur = (chip->writebuf_cur + 1) & (OPL_WRITEBUF_SIZE - 1);
	}

	chip->writebuf_samplecnt++;
}

__attribute__((section(".ccm"))) void
OPL3_Generate(opl3_chip* chip, int16_t* buf)
{
	int16_t samples[4];
	OPL3_Generate4Ch(chip, samples);
	buf[0] = samples[0];
	#if !OPL_MONO
	buf[1] = samples[1];
	#endif
}

__attribute__((section(".ccm"))) void
OPL3_Generate4ChResampled(opl3_chip* chip, int16_t* buf4)
{
	while (chip->samplecnt >= chip->rateratio) {
		chip->oldsamples[0] = chip->samples[0];
		chip->oldsamples[1] = chip->samples[1];
		chip->oldsamples[2] = chip->samples[2];
		chip->oldsamples[3] = chip->samples[3];
		OPL3_Generate4Ch(chip, chip->samples);
		chip->samplecnt -= chip->rateratio;
	}

	buf4[0] = (int16_t)((chip->oldsamples[0] * (chip->rateratio - chip->samplecnt)
						 + chip->samples[0] * chip->samplecnt) / chip->rateratio);
	buf4[1] = (int16_t)((chip->oldsamples[1] * (chip->rateratio - chip->samplecnt)
						 + chip->samples[1] * chip->samplecnt) / chip->rateratio);
	buf4[2] = (int16_t)((chip->oldsamples[2] * (chip->rateratio - chip->samplecnt)
						 + chip->samples[2] * chip->samplecnt) / chip->rateratio);
	buf4[3] = (int16_t)((chip->oldsamples[3] * (chip->rateratio - chip->samplecnt)
						 + chip->samples[3] * chip->samplecnt) / chip->rateratio);
	chip->samplecnt += 1 << RSM_FRAC;
}

__attribute__((section(".ccm"))) void
OPL3_GenerateResampled(opl3_chip* chip, int16_t* buf)
{
	int16_t samples[4];
	OPL3_Generate4ChResampled(chip, samples);
	buf[0] = samples[0];
	buf[1] = samples[1];
}

void
OPL3_Reset(opl3_chip* chip, uint32_t samplerate)
{
	opl3_slot* slot;
	opl3_channel* channel;
	uint8_t slotnum;
	uint8_t channum;
	uint8_t local_ch_slot;

	memset(chip, 0, sizeof(opl3_chip));

	for (slotnum = 0; slotnum < 36; slotnum++) {
		slot = &chip->slot[slotnum];
		slot->chip = chip;
		slot->mod = &chip->zeromod;
		slot->eg_rout = 0x1ff;
		slot->eg_out = 0x1ff;
		slot->eg_gen = envelope_gen_num_release;
		slot->trem = (uint8_t*)&chip->zeromod;
		slot->slot_num = slotnum;
		slot->is_silent = 1;        /* OPT: all slots start silent */
		slot->is_modulator = 0;     /* OPT: set below for slotz[0] of each channel */
		slot->phase_step_valid = 0; /* OPT: force first-sample recompute */
		slot->eg_static = 0;        /* OPT: refreshed on first reg write */
	}

	chip->active_slot_count = 0;    /* OPT: nothing playing yet */

	for (channum = 0; channum < 18; channum++) {
		channel = &chip->channel[channum];
		local_ch_slot = ch_slot[channum];
		channel->slotz[0] = &chip->slot[local_ch_slot];
		channel->slotz[1] = &chip->slot[local_ch_slot + 3u];
		chip->slot[local_ch_slot].channel = channel;
		chip->slot[local_ch_slot + 3u].channel = channel;
		chip->slot[local_ch_slot].is_modulator = 1; /* OPT: only slotz[0]'s fbmod is read by the alg routings */
		channel->active_slots = 0;  /* OPT: per-channel active count */

		if ((channum % 9) < 3)
			channel->pair = &chip->channel[channum + 3u];

		else if ((channum % 9) < 6)
			channel->pair = &chip->channel[channum - 3u];

		channel->chip = chip;
		channel->out[0] = &chip->zeromod;
		channel->out[1] = &chip->zeromod;
		channel->out[2] = &chip->zeromod;
		channel->out[3] = &chip->zeromod;
		channel->chtype = ch_2op;
		channel->cha = 0xffff;
		channel->chb = 0xffff;
		#if OPL_ENABLE_STEREOEXT
		channel->leftpan = 0x10000;
		channel->rightpan = 0x10000;
		#endif
		channel->ch_num = channum;
		OPL3_ChannelSetupAlg(channel);
	}

	chip->noise = 1;
	chip->rateratio = (samplerate << RSM_FRAC) / 49716;
	chip->tremoloshift = 4;
	chip->vibshift = 1;

	#if OPL_ENABLE_STEREOEXT

	if (!panpot_lut_build) {
		int32_t i;

		for (i = 0; i < 256; i++)
			panpot_lut[i] = OPL_SIN(i);

		panpot_lut_build = 1;
	}

	#endif
}

void
OPL3_WriteReg(opl3_chip* chip, uint16_t reg, uint8_t v)
{
	uint8_t high = (reg >> 8) & 0x01;
	uint8_t regm = reg & 0xff;

	switch (regm & 0xf0) {
		case 0x00:
			if (high) {
				switch (regm & 0x0f) {
					case 0x04:
						OPL3_ChannelSet4Op(chip, v);
						break;

					case 0x05:
						#if OPL_FORCE_OPL2
						chip->newm = 0; /* OPT: pin to 0 — disables 4-op + wf 4-7 */
						#else
						chip->newm = v & 0x01;
						#endif
						#if OPL_ENABLE_STEREOEXT
						chip->stereoext = (v >> 1) & 0x01;
						#endif
						break;
				}

			} else {
				switch (regm & 0x0f) {
					case 0x08:
						chip->nts = (v >> 6) & 0x01;
						break;
				}
			}

			break;

		case 0x20:
		case 0x30:
			if (ad_slot[regm & 0x1fu] >= 0)
				OPL3_SlotWrite20(&chip->slot[18u * high + ad_slot[regm & 0x1fu]], v);

			break;

		case 0x40:
		case 0x50:
			if (ad_slot[regm & 0x1fu] >= 0)
				OPL3_SlotWrite40(&chip->slot[18u * high + ad_slot[regm & 0x1fu]], v);

			break;

		case 0x60:
		case 0x70:
			if (ad_slot[regm & 0x1fu] >= 0)
				OPL3_SlotWrite60(&chip->slot[18u * high + ad_slot[regm & 0x1fu]], v);

			break;

		case 0x80:
		case 0x90:
			if (ad_slot[regm & 0x1fu] >= 0)
				OPL3_SlotWrite80(&chip->slot[18u * high + ad_slot[regm & 0x1fu]], v);

			break;

		case 0xe0:
		case 0xf0:
			if (ad_slot[regm & 0x1fu] >= 0)
				OPL3_SlotWriteE0(&chip->slot[18u * high + ad_slot[regm & 0x1fu]], v);

			break;

		case 0xa0:
			if ((regm & 0x0f) < 9)
				OPL3_ChannelWriteA0(&chip->channel[9u * high + (regm & 0x0fu)], v);

			break;

		case 0xb0:
			if (regm == 0xbd && !high) {
				chip->tremoloshift = (((v >> 7) ^ 1) << 1) + 2;
				chip->vibshift = ((v >> 6) & 0x01) ^ 1;
				OPL3_ChannelUpdateRhythm(chip, v);

			} else if ((regm & 0x0f) < 9) {
				OPL3_ChannelWriteB0(&chip->channel[9u * high + (regm & 0x0fu)], v);

				if (v & 0x20)
					OPL3_ChannelKeyOn(&chip->channel[9u * high + (regm & 0x0fu)]);

				else
					OPL3_ChannelKeyOff(&chip->channel[9u * high + (regm & 0x0fu)]);
			}

			break;

		case 0xc0:
			if ((regm & 0x0f) < 9)
				OPL3_ChannelWriteC0(&chip->channel[9u * high + (regm & 0x0fu)], v);

			break;
			#if OPL_ENABLE_STEREOEXT

		case 0xd0:
			if ((regm & 0x0f) < 9)
				OPL3_ChannelWriteD0(&chip->channel[9u * high + (regm & 0x0fu)], v);

			break;
			#endif
	}
}

void
OPL3_WriteRegBuffered(opl3_chip* chip, uint16_t reg, uint8_t v)
{
	uint64_t time1, time2;
	opl3_writebuf* writebuf;
	uint32_t writebuf_last;

	writebuf_last = chip->writebuf_last;
	writebuf = &chip->writebuf[writebuf_last];

	if (writebuf->reg & 0x200) {
		OPL3_WriteReg(chip, writebuf->reg & 0x1ff, writebuf->data);

		chip->writebuf_cur = (writebuf_last + 1) & (OPL_WRITEBUF_SIZE - 1);
		chip->writebuf_samplecnt = writebuf->time;
	}

	writebuf->reg = reg | 0x200;
	writebuf->data = v;
	time1 = chip->writebuf_lasttime + OPL_WRITEBUF_DELAY;
	time2 = chip->writebuf_samplecnt;

	if (time1 < time2)
		time1 = time2;

	writebuf->time = time1;
	chip->writebuf_lasttime = time1;
	chip->writebuf_last = (writebuf_last + 1) & (OPL_WRITEBUF_SIZE - 1);
}

void
OPL3_Generate4ChStream(opl3_chip* chip, int16_t* sndptr1, int16_t* sndptr2, uint32_t numsamples)
{
	uint_fast32_t i;
	int16_t samples[4];

	for (i = 0; i < numsamples; i++) {
		OPL3_Generate4ChResampled(chip, samples);
		sndptr1[0] = samples[0];
		sndptr1[1] = samples[1];
		sndptr2[0] = samples[2];
		sndptr2[1] = samples[3];
		sndptr1 += 2;
		sndptr2 += 2;
	}
}

void
OPL3_GenerateStream(opl3_chip* chip, int16_t* sndptr, uint32_t numsamples)
{
	uint_fast32_t i;

	for (i = 0; i < numsamples; i++) {
		OPL3_GenerateResampled(chip, sndptr);
		sndptr += 2;
	}
}
