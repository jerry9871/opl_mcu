/*
    dbopl.c -- OPL3 emulator, C port of DOSBox's dbopl.cpp.

    Original C++:
      Copyright (C) 2002-2021  The DOSBox Team
    C port:
      Copyright (C) 2026  silixcon

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    ===========================================================================
    Port notes
    ===========================================================================
    - The upstream lets you pick one of three wave generators
      (WAVE_HANDLER / WAVE_TABLELOG / WAVE_TABLEMUL). DOSBox itself ships
      with WAVE_TABLEMUL (the fastest one); this port only carries that
      path. The other two are not used by anyone in practice, and dropping
      them removes ~150 lines of dead code.

    - C++ classes (Chip / Channel / Operator) become plain structs with the
      same field layout. Member functions become free functions taking the
      "this" pointer as the first argument. Cross-channel pointer math
      (`this + 1`, `Op(i)` reaching into the next channel) is preserved
      verbatim because the channel array is contiguous in both C and C++.

    - Member function pointers (VolumeHandler, SynthHandler) become regular
      function pointers to free functions with matching signatures. There
      are 5 volume handlers (one per envelope state) and 10 synth handlers
      (one per SynthMode value).

    - Templates (`TemplateVolume<state>`, `BlockTemplate<mode>`,
      `GeneratePercussion<opl3Mode>`) are lowered by writing one inline
      body and calling it from N thin wrappers with the parameter passed
      as a constant. -O2 inlines and dead-code-eliminates exactly like
      template instantiation, so the generated code is equivalent.

    - Default member initializers and constructors become explicit
      `op_init()` / `ch_init()` / `chip_init()` calls.

    - DOSBox-specific bits (Adlib::Handler base class, MixerChannel,
      SaveState/LoadState, GCC_LIKELY/UNLIKELY, Bits/Bitu typedefs,
      DB_FASTCALL) are removed or replaced with stdint types.
*/

/* Force gcc to specialise these hot helpers per call site even at -O2.
 * Without this, block_body (called via SynthHandler function pointer) is
 * inlined but its inner helpers stay out-of-line and the per-sample
 * indirect-call overhead dominates -- adds ~30%% runtime vs the C++
 * template original. With it, -O2 matches or beats the C++ baseline. */
#if defined(__GNUC__) || defined(__clang__)
#  define HOT_INLINE static inline __attribute__((always_inline))
#else
#  define HOT_INLINE static inline
#endif

#include "opl3.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ===== upstream typedefs / config ======================================== */

typedef uintptr_t Bitu;
typedef intptr_t  Bits;

#ifndef PI
	#define PI 3.14159265358979323846
#endif

#define OPLRATE        ((double)(14318180.0 / 288.0))
#define TREMOLO_TABLE  52

/* Wave precision: 10.22 fixed point at the top of a 32-bit phase counter. */
#define WAVE_BITS  10
#define WAVE_SH    (32 - WAVE_BITS)
#define WAVE_MASK  ((1 << WAVE_SH) - 1)

#define LFO_SH  (WAVE_SH - 10)
#define LFO_MAX (256 << LFO_SH)

/*  Envelope precision. WAVE_TABLEMUL uses the value directly so ENV_BITS=9
    and ENV_EXTRA=0 -- kept as macros to match the upstream literally. */
#define ENV_BITS    9
#define ENV_MIN     0
#define ENV_EXTRA   (ENV_BITS - 9)
#define ENV_MAX     (511 << ENV_EXTRA)
#define ENV_LIMIT   ((12 * 256) >> (3 - ENV_EXTRA))
#define ENV_SILENT(_X_) ((_X_) >= ENV_LIMIT)

#define RATE_SH    24
#define RATE_MASK  ((1 << RATE_SH) - 1)
#define MUL_SH     16

/*  Bits in the chanData word -- low 16 are raw register bits, high bits
    are the precomputed key code and KSL base used by the operators. */
enum {
	SHIFT_KSLBASE = 16,
	SHIFT_KEYCODE = 24
};

/* Operator reg20 mask bits. */
enum {
	MASK_KSR     = 0x10,
	MASK_SUSTAIN = 0x20,
	MASK_VIBRATO = 0x40,
	MASK_TREMOLO = 0x80
};

/* Envelope state -- order matters: indexes VolumeHandlerTable[]. */
typedef enum {
	OFF = 0,
	RELEASE,
	SUSTAIN,
	DECAY,
	ATTACK
} OpState;

/* Synth mode -- order matters for sm4Start / sm6Start grouping. */
typedef enum {
	sm2AM = 0,
	sm2FM,
	sm3AM,
	sm3FM,
	sm4Start,
	sm3FMFM,
	sm3AMFM,
	sm3FMAM,
	sm3AMAM,
	sm6Start,
	sm2Percussion,
	sm3Percussion
} SynthMode;

/* ===== forward decls ===================================================== */

struct Chip;
struct Channel;
struct Operator;

typedef int32_t (*VolumeHandler)(struct Operator* op);
typedef struct Channel* (*SynthHandler)(struct Channel* ch, struct Chip* chip,
										uint32_t samples, int32_t* output);

/* ===== struct layouts (must match the C++ original) ====================== */

typedef struct Operator {
	VolumeHandler volHandler;

	int16_t* waveBase;
	uint32_t waveMask;
	uint32_t waveStart;

	uint32_t waveIndex;
	uint32_t waveAdd;
	uint32_t waveCurrent;

	uint32_t chanData;
	uint32_t freqMul;
	uint32_t vibrato;
	int32_t  sustainLevel;
	int32_t  totalLevel;
	uint32_t currentLevel;
	int32_t  volume;

	uint32_t attackAdd;
	uint32_t decayAdd;
	uint32_t releaseAdd;
	uint32_t rateIndex;

	uint8_t rateZero;
	uint8_t keyOn;
	uint8_t reg20, reg40, reg60, reg80, regE0;
	uint8_t state;
	uint8_t tremoloMask;
	uint8_t vibStrength;
	uint8_t ksr;
} Operator;

typedef struct Channel {
	Operator op[2];   /* must be first -- (this+n)->op[i] walks neighbours */
	SynthHandler synthHandler;
	uint32_t chanData;
	int32_t  old[2];

	uint8_t feedback;
	uint8_t regB0;
	uint8_t regC0;
	uint8_t fourMask;
	int8_t  maskLeft;
	int8_t  maskRight;
} Channel;

typedef struct Chip {
	Channel chan[18];

	uint32_t lfoCounter;
	uint32_t lfoAdd;

	uint32_t noiseCounter;
	uint32_t noiseAdd;
	uint32_t noiseValue;

	uint32_t freqMul[16];
	uint32_t linearRates[76];
	uint32_t attackRates[76];

	uint8_t reg104;
	uint8_t reg08;
	uint8_t reg04;
	uint8_t regBD;
	uint8_t vibratoIndex;
	uint8_t tremoloIndex;
	int8_t  vibratoSign;
	uint8_t vibratoShift;
	uint8_t tremoloValue;
	uint8_t vibratoStrength;
	uint8_t tremoloStrength;
	uint8_t waveFormMask;
	int8_t  opl3Active;
	uint8_t opl3Mode;
} Chip;

/* ===== static tables ===================================================== */

static const uint8_t KslCreateTable[16] = {
	64, 32, 24, 19,
	16, 12, 11, 10,
	8,  6,  5,  4,
	3,  2,  1,  0
};

#define M(_X_) ((uint8_t)((_X_) * 2))
static const uint8_t FreqCreateTable[16] = {
	M(0.5), M(1), M(2), M(3), M(4), M(5), M(6), M(7),
	M(8), M(9), M(10), M(10), M(12), M(12), M(15), M(15)
};
#undef M

static const uint8_t AttackSamplesTable[13] = {
	69, 55, 46, 40,
	35, 29, 23, 20,
	19, 15, 11, 10,
	9
};

static const uint8_t EnvelopeIncreaseTable[13] = {
	4,  5,  6,  7,
	8, 10, 12, 14,
	16, 20, 24, 28,
	32
};

/* WAVE_TABLEMUL only -- log2(sin) table is folded into a multiply table. */
static int16_t  WaveTable[8 * 512];
static uint16_t MulTable[384];

static const uint16_t WaveBaseTable[8] = {
	0x000, 0x200, 0x200, 0x800,
	0xa00, 0xc00, 0x100, 0x400
};
static const uint16_t WaveMaskTable[8] = {
	1023, 1023, 511, 511,
	1023, 1023, 512, 1023
};
static const uint16_t WaveStartTable[8] = {
	512, 0, 0, 0,
	0, 512, 512, 256
};

static uint8_t  KslTable[8 * 16];
static uint8_t  TremoloTable[TREMOLO_TABLE];
/* 1-based offsets so 0 means "unused" -- matches upstream layout. */
static uint16_t ChanOffsetTable[32];
static uint16_t OpOffsetTable[64];

static const int8_t VibratoTable[8] = {
	1 - 0x00, 0 - 0x00, 1 - 0x00, 30 - 0x00,
	1 - 0x80, 0 - 0x80, 1 - 0x80, 30 - 0x80
};

static const uint8_t KslShiftTable[4] = { 31, 1, 2, 0 };

static int doneTables = 0;

/* ===== small helpers ===================================================== */

static void
EnvelopeSelect(uint8_t val, uint8_t* index, uint8_t* shift)
{
	if (val < 13 * 4) {           /* rate 0-12 */
		*shift = 12 - (val >> 2);
		*index = val & 3;

	} else if (val < 15 * 4) {    /* rate 13-14 */
		*shift = 0;
		*index = val - 12 * 4;

	} else {                       /* rate 15+ */
		*shift = 0;
		*index = 12;
	}
}

HOT_INLINE Operator*
chan_op(Channel* ch, unsigned i)
{
	/*  (this + (i>>1))->op[i & 1] -- walks across adjacent Channel structs
	    (used by 4-op modes). Layout-critical: works because chan[18] in
	    Chip is one contiguous array and op[2] is the first member of
	    Channel. Identical trick to the C++ original. */
	return &(ch + (i >> 1))->op[i & 1];
}

/* ===== Operator: rate / attenuation / frequency ========================== */

static void
op_update_attack(Operator* op, const Chip* chip)
{
	uint8_t rate = op->reg60 >> 4;

	if (rate) {
		uint8_t val = (rate << 2) + op->ksr;
		op->attackAdd = chip->attackRates[val];
		op->rateZero &= ~(1u << ATTACK);

	} else {
		op->attackAdd = 0;
		op->rateZero |= (1u << ATTACK);
	}
}

static void
op_update_decay(Operator* op, const Chip* chip)
{
	uint8_t rate = op->reg60 & 0xf;

	if (rate) {
		uint8_t val = (rate << 2) + op->ksr;
		op->decayAdd = chip->linearRates[val];
		op->rateZero &= ~(1u << DECAY);

	} else {
		op->decayAdd = 0;
		op->rateZero |= (1u << DECAY);
	}
}

static void
op_update_release(Operator* op, const Chip* chip)
{
	uint8_t rate = op->reg80 & 0xf;

	if (rate) {
		uint8_t val = (rate << 2) + op->ksr;
		op->releaseAdd = chip->linearRates[val];
		op->rateZero &= ~(1u << RELEASE);

		if (!(op->reg20 & MASK_SUSTAIN))
			op->rateZero &= ~(1u << SUSTAIN);

	} else {
		op->rateZero |= (1u << RELEASE);
		op->releaseAdd = 0;

		if (!(op->reg20 & MASK_SUSTAIN))
			op->rateZero |= (1u << SUSTAIN);
	}
}

static void
op_update_attenuation(Operator* op)
{
	uint8_t  kslBase  = (uint8_t)((op->chanData >> SHIFT_KSLBASE) & 0xff);
	uint32_t tl       = op->reg40 & 0x3f;
	uint8_t  kslShift = KslShiftTable[op->reg40 >> 6];
	op->totalLevel  = (int32_t)(tl << (ENV_BITS - 7));
	op->totalLevel += (kslBase << ENV_EXTRA) >> kslShift;
}

static void
op_update_frequency(Operator* op)
{
	uint32_t freq  =  op->chanData        & ((1 << 10) - 1);
	uint32_t block = (op->chanData >> 10) & 0xff;
	op->waveAdd = (freq << block) * op->freqMul;

	if (op->reg20 & MASK_VIBRATO) {
		op->vibStrength = (uint8_t)(freq >> 7u);
		op->vibrato = ((Bitu)op->vibStrength << block) * op->freqMul;

	} else {
		op->vibStrength = 0;
		op->vibrato = 0;
	}
}

static void
op_update_rates(Operator* op, const Chip* chip)
{
	uint8_t newKsr = (uint8_t)((op->chanData >> SHIFT_KEYCODE) & 0xff);

	if (!(op->reg20 & MASK_KSR))
		newKsr >>= 2;

	if (op->ksr == newKsr)
		return;

	op->ksr = newKsr;
	op_update_attack(op, chip);
	op_update_decay(op, chip);
	op_update_release(op, chip);
}

HOT_INLINE int32_t
op_rate_forward(Operator* op, uint32_t add)
{
	op->rateIndex += add;
	int32_t ret = (int32_t)(op->rateIndex >> RATE_SH);
	op->rateIndex = op->rateIndex & RATE_MASK;
	return ret;
}

/* ===== Operator: volume handlers (TemplateVolume<state> instantiations) == */

static void
op_set_state(Operator* op, uint8_t s); /* fwd */

static int32_t
vol_off(Operator* op)
{
	(void)op;
	return ENV_MAX;
}

static int32_t
vol_attack(Operator* op)
{
	int32_t vol = op->volume;
	int32_t change = op_rate_forward(op, op->attackAdd);

	if (!change)
		return vol;

	vol += ((~vol) * change) >> 3;

	if (vol < ENV_MIN) {
		op->volume = ENV_MIN;
		op->rateIndex = 0;
		op_set_state(op, DECAY);
		return ENV_MIN;
	}

	op->volume = vol;
	return vol;
}

static int32_t
vol_decay(Operator* op)
{
	int32_t vol = op->volume;
	vol += op_rate_forward(op, op->decayAdd);

	if (vol >= op->sustainLevel) {
		if (vol >= ENV_MAX) {
			op->volume = ENV_MAX;
			op_set_state(op, OFF);
			return ENV_MAX;
		}

		op->rateIndex = 0;
		op_set_state(op, SUSTAIN);
	}

	op->volume = vol;
	return vol;
}

static int32_t
vol_release(Operator* op)
{
	int32_t vol = op->volume;
	vol += op_rate_forward(op, op->releaseAdd);

	if (vol >= ENV_MAX) {
		op->volume = ENV_MAX;
		op_set_state(op, OFF);
		return ENV_MAX;
	}

	op->volume = vol;
	return vol;
}

static int32_t
vol_sustain(Operator* op)
{
	if (op->reg20 & MASK_SUSTAIN)
		return op->volume;

	/* Not really sustaining -- fall through to release behaviour. */
	return vol_release(op);
}

/* Indexed by OpState: OFF, RELEASE, SUSTAIN, DECAY, ATTACK. */
static const VolumeHandler VolumeHandlerTable[5] = {
	vol_off, vol_release, vol_sustain, vol_decay, vol_attack
};

static void
op_set_state(Operator* op, uint8_t s)
{
	op->state = s;
	op->volHandler = VolumeHandlerTable[s];
}

HOT_INLINE Bitu
op_forward_volume(Operator* op)
{
	return (Bitu)(op->currentLevel + op->volHandler(op));
}

HOT_INLINE Bitu
op_forward_wave(Operator* op)
{
	op->waveIndex += op->waveCurrent;
	return op->waveIndex >> WAVE_SH;
}

/* ===== Operator: register writes ========================================= */

static void
op_write20(Operator* op, const Chip* chip, uint8_t val)
{
	uint8_t change = op->reg20 ^ val;

	if (!change)
		return;

	op->reg20 = val;
	op->tremoloMask = (int8_t)(val) >> 7;
	op->tremoloMask &= ~((1 << ENV_EXTRA) - 1);

	if (change & MASK_KSR)
		op_update_rates(op, chip);

	if ((op->reg20 & MASK_SUSTAIN) || !op->releaseAdd)
		op->rateZero |= (1u << SUSTAIN);

	else
		op->rateZero &= ~(1u << SUSTAIN);

	if (change & (0xf | MASK_VIBRATO)) {
		op->freqMul = chip->freqMul[val & 0xf];
		op_update_frequency(op);
	}
}

static void
op_write40(Operator* op, const Chip* chip, uint8_t val)
{
	(void)chip;

	if (!(op->reg40 ^ val))
		return;

	op->reg40 = val;
	op_update_attenuation(op);
}

static void
op_write60(Operator* op, const Chip* chip, uint8_t val)
{
	uint8_t change = op->reg60 ^ val;
	op->reg60 = val;

	if (change & 0x0f)
		op_update_decay(op, chip);

	if (change & 0xf0)
		op_update_attack(op, chip);
}

static void
op_write80(Operator* op, const Chip* chip, uint8_t val)
{
	uint8_t change = op->reg80 ^ val;

	if (!change)
		return;

	op->reg80 = val;
	uint8_t sustain = val >> 4;
	sustain |= (sustain + 1) & 0x10;
	op->sustainLevel = sustain << (ENV_BITS - 5);

	if (change & 0x0f)
		op_update_release(op, chip);
}

static void
op_write_e0(Operator* op, const Chip* chip, uint8_t val)
{
	if (!(op->regE0 ^ val))
		return;

	const uint8_t waveForm =
		val & ((0x3 & chip->waveFormMask) | (0x7 & chip->opl3Active));
	op->regE0 = val;
	op->waveBase  = WaveTable + WaveBaseTable[waveForm];
	op->waveStart = (Bitu)WaveStartTable[waveForm] << WAVE_SH;
	op->waveMask  = WaveMaskTable[waveForm];
}

static int
op_silent(const Operator* op)
{
	if (!ENV_SILENT(op->totalLevel + op->volume))
		return 0;

	if (!(op->rateZero & (1u << op->state)))
		return 0;

	return 1;
}

static void
op_prepare(Operator* op, const Chip* chip)
{
	op->currentLevel = (uint32_t)(op->totalLevel + (int32_t)(chip->tremoloValue & op->tremoloMask));
	op->waveCurrent = op->waveAdd;

	if (op->vibStrength >> chip->vibratoShift) {
		int32_t add = (int32_t)(op->vibrato >> chip->vibratoShift);
		int32_t neg = chip->vibratoSign;
		add = (add ^ neg) - neg;
		op->waveCurrent += (Bitu)add;
	}
}

static void
op_key_on(Operator* op, uint8_t mask)
{
	if (!op->keyOn) {
		op->waveIndex = op->waveStart;
		op->rateIndex = 0;
		op_set_state(op, ATTACK);
	}

	op->keyOn |= mask;
}

static void
op_key_off(Operator* op, uint8_t mask)
{
	op->keyOn &= ~mask;

	if (!op->keyOn) {
		if (op->state != OFF)
			op_set_state(op, RELEASE);
	}
}

HOT_INLINE Bits
op_get_wave(Operator* op, Bitu index, Bitu vol)
{
	/* WAVE_TABLEMUL path only. */
	return (op->waveBase[index & op->waveMask] * MulTable[vol >> ENV_EXTRA]) >> MUL_SH;
}

HOT_INLINE Bits
op_get_sample(Operator* op, Bits modulation)
{
	Bitu vol = op_forward_volume(op);

	if (ENV_SILENT(vol)) {
		op->waveIndex += op->waveCurrent;
		return 0;

	} else {
		Bitu index = op_forward_wave(op);
		index += (unsigned long)modulation;
		return op_get_wave(op, index, vol);
	}
}

static void
op_init(Operator* op)
{
	memset(op, 0, sizeof(*op));
	op_set_state(op, OFF);
	op->rateZero     = (1u << OFF);
	op->sustainLevel = ENV_MAX;
	op->currentLevel = ENV_MAX;
	op->totalLevel   = ENV_MAX;
	op->volume       = ENV_MAX;
}

/* ===== Channel =========================================================== */

/* Synth handler forward decls -- defined after the body template. */
static Channel*
synth_2am(Channel* ch, Chip* chip, uint32_t samples, int32_t* out);
static Channel*
synth_2fm(Channel* ch, Chip* chip, uint32_t samples, int32_t* out);
static Channel*
synth_3am(Channel* ch, Chip* chip, uint32_t samples, int32_t* out);
static Channel*
synth_3fm(Channel* ch, Chip* chip, uint32_t samples, int32_t* out);
static Channel*
synth_3fmfm(Channel* ch, Chip* chip, uint32_t samples, int32_t* out);
static Channel*
synth_3amfm(Channel* ch, Chip* chip, uint32_t samples, int32_t* out);
static Channel*
synth_3fmam(Channel* ch, Chip* chip, uint32_t samples, int32_t* out);
static Channel*
synth_3amam(Channel* ch, Chip* chip, uint32_t samples, int32_t* out);
static Channel*
synth_2perc(Channel* ch, Chip* chip, uint32_t samples, int32_t* out);
static Channel*
synth_3perc(Channel* ch, Chip* chip, uint32_t samples, int32_t* out);

static void
ch_init(Channel* ch)
{
	memset(ch, 0, sizeof(*ch));
	op_init(&ch->op[0]);
	op_init(&ch->op[1]);
	ch->maskLeft     = -1;
	ch->maskRight    = -1;
	ch->feedback     = 31;
	ch->synthHandler = synth_2fm;
}

static void
ch_set_chan_data(Channel* ch, const Chip* chip, uint32_t data)
{
	uint32_t change = ch->chanData ^ data;
	ch->chanData = data;
	chan_op(ch, 0)->chanData = data;
	chan_op(ch, 1)->chanData = data;
	op_update_frequency(chan_op(ch, 0));
	op_update_frequency(chan_op(ch, 1));

	if (change & (0xffu << SHIFT_KSLBASE)) {
		op_update_attenuation(chan_op(ch, 0));
		op_update_attenuation(chan_op(ch, 1));
	}

	if (change & (0xffu << SHIFT_KEYCODE)) {
		op_update_rates(chan_op(ch, 0), chip);
		op_update_rates(chan_op(ch, 1), chip);
	}
}

static void
ch_update_frequency(Channel* ch, const Chip* chip, uint8_t fourOp)
{
	uint32_t data    = ch->chanData & 0xffff;
	uint32_t kslBase = KslTable[data >> 6];
	uint32_t keyCode = (data & 0x1c00) >> 9;

	if (chip->reg08 & 0x40) {
		keyCode |= (data & 0x100) >> 8;   /* notesel == 1 */

	} else {
		keyCode |= (data & 0x200) >> 9;   /* notesel == 0 */
	}

	data |= (keyCode << SHIFT_KEYCODE) | (kslBase << SHIFT_KSLBASE);
	ch_set_chan_data(ch + 0, chip, data);

	if (fourOp & 0x3f)
		ch_set_chan_data(ch + 1, chip, data);
}

static void
ch_update_synth(Channel* ch, const Chip* chip);

static void
ch_write_a0(Channel* ch, const Chip* chip, uint8_t val)
{
	uint8_t fourOp = chip->reg104 & chip->opl3Active & ch->fourMask;

	if (fourOp > 0x80)
		return;

	uint32_t change = (ch->chanData ^ val) & 0xff;

	if (change) {
		ch->chanData ^= change;
		ch_update_frequency(ch, chip, fourOp);
	}
}

static void
ch_write_b0(Channel* ch, const Chip* chip, uint8_t val)
{
	uint8_t fourOp = chip->reg104 & chip->opl3Active & ch->fourMask;

	if (fourOp > 0x80)
		return;

	Bitu change = (ch->chanData ^ ((unsigned int)val << 8u)) & 0x1f00u;

	if (change) {
		ch->chanData ^= change;
		ch_update_frequency(ch, chip, fourOp);
	}

	if (!((val ^ ch->regB0) & 0x20))
		return;

	ch->regB0 = val;

	if (val & 0x20) {
		op_key_on(chan_op(ch, 0), 0x1);
		op_key_on(chan_op(ch, 1), 0x1);

		if (fourOp & 0x3f) {
			op_key_on(chan_op(ch + 1, 0), 1);
			op_key_on(chan_op(ch + 1, 1), 1);
		}

	} else {
		op_key_off(chan_op(ch, 0), 0x1);
		op_key_off(chan_op(ch, 1), 0x1);

		if (fourOp & 0x3f) {
			op_key_off(chan_op(ch + 1, 0), 1);
			op_key_off(chan_op(ch + 1, 1), 1);
		}
	}
}

static void
ch_write_c0(Channel* ch, const Chip* chip, uint8_t val)
{
	uint8_t change = val ^ ch->regC0;

	if (!change)
		return;

	ch->regC0 = val;
	ch->feedback = (ch->regC0 >> 1) & 7;

	if (ch->feedback)
		ch->feedback = 9 - ch->feedback;

	else
		ch->feedback = 31;

	ch_update_synth(ch, chip);
}

static void
ch_update_synth(Channel* ch, const Chip* chip)
{
	if (chip->opl3Active) {
		if ((chip->reg104 & ch->fourMask) & 0x3f) {
			Channel* chan0, *chan1;

			if (!(ch->fourMask & 0x80)) {
				chan0 = ch;
				chan1 = ch + 1;

			} else {
				chan0 = ch - 1;
				chan1 = ch;
			}

			uint8_t synth = ((chan0->regC0 & 1) << 0) | ((chan1->regC0 & 1) << 1);

			switch (synth) {
				case 0:
					chan0->synthHandler = synth_3fmfm;
					break;

				case 1:
					chan0->synthHandler = synth_3amfm;
					break;

				case 2:
					chan0->synthHandler = synth_3fmam;
					break;

				case 3:
					chan0->synthHandler = synth_3amam;
					break;
			}

		} else if ((ch->fourMask & 0x40) && (chip->regBD & 0x20)) {
			/* percussion -- handler set elsewhere */
		} else if (ch->regC0 & 1)
			ch->synthHandler = synth_3am;

		else
			ch->synthHandler = synth_3fm;

		ch->maskLeft  = (ch->regC0 & 0x10) ? -1 : 0;
		ch->maskRight = (ch->regC0 & 0x20) ? -1 : 0;

	} else {
		if ((ch->fourMask & 0x40) && (chip->regBD & 0x20)) {
			/* percussion -- handler set elsewhere */
		} else if (ch->regC0 & 1)
			ch->synthHandler = synth_2am;

		else
			ch->synthHandler = synth_2fm;
	}
}

/* ===== Chip: noise / LFO ================================================= */

HOT_INLINE uint32_t
chip_forward_noise(Chip* chip)
{
	chip->noiseCounter += chip->noiseAdd;
	Bitu count = chip->noiseCounter >> LFO_SH;
	chip->noiseCounter &= ((1 << LFO_SH) - 1);

	for (; count > 0; --count) {
		chip->noiseValue ^= 0x800302 & (0 - (chip->noiseValue & 1));
		chip->noiseValue >>= 1;
	}

	return chip->noiseValue;
}

HOT_INLINE uint32_t
chip_forward_lfo(Chip* chip, uint32_t samples)
{
	chip->vibratoSign  = (VibratoTable[chip->vibratoIndex >> 2]) >> 7;
	chip->vibratoShift = (VibratoTable[chip->vibratoIndex >> 2] & 7) + chip->vibratoStrength;
	chip->tremoloValue = TremoloTable[chip->tremoloIndex] >> chip->tremoloStrength;

	uint32_t todo  = LFO_MAX - chip->lfoCounter;
	uint32_t count = (todo + chip->lfoAdd - 1) / chip->lfoAdd;

	if (count > samples) {
		count = samples;
		chip->lfoCounter += count * chip->lfoAdd;

	} else {
		chip->lfoCounter += count * chip->lfoAdd;
		chip->lfoCounter &= (LFO_MAX - 1);
		chip->vibratoIndex = (chip->vibratoIndex + 1) & 31;

		if (chip->tremoloIndex + 1 < TREMOLO_TABLE)
			++chip->tremoloIndex;

		else
			chip->tremoloIndex = 0;
	}

	return count;
}

/* ===== Percussion (GeneratePercussion<opl3Mode>) ========================= */

HOT_INLINE void
gen_percussion(Channel* ch, Chip* chip, int32_t* output, int opl3Mode)
{
	/* Bass drum (channel 6 ops 0/1) */
	int32_t mod = (int32_t)((uint32_t)(ch->old[0] + ch->old[1]) >> ch->feedback);
	ch->old[0] = ch->old[1];
	ch->old[1] = (int32_t)op_get_sample(chan_op(ch, 0), mod);

	if (ch->regC0 & 1)
		mod = 0;

	else
		mod = ch->old[0];

	int32_t sample = (int32_t)op_get_sample(chan_op(ch, 1), mod);

	uint32_t noiseBit = chip_forward_noise(chip) & 0x1;
	uint32_t c2 = (uint32_t)op_forward_wave(chan_op(ch, 2));
	uint32_t c5 = (uint32_t)op_forward_wave(chan_op(ch, 5));
	uint32_t phaseBit =
		(((c2 & 0x88) ^ ((c2 << 5) & 0x80)) | ((c5 ^ (c5 << 2)) & 0x20)) ? 0x02 : 0x00;

	uint32_t hhVol = (uint32_t)op_forward_volume(chan_op(ch, 2));

	if (!ENV_SILENT(hhVol)) {
		uint32_t hhIndex = (phaseBit << 8) | (0x34 << (phaseBit ^ (noiseBit << 1)));
		sample += (int32_t)op_get_wave(chan_op(ch, 2), hhIndex, hhVol);
	}

	uint32_t sdVol = (uint32_t)op_forward_volume(chan_op(ch, 3));

	if (!ENV_SILENT(sdVol)) {
		uint32_t sdIndex = (0x100 + (c2 & 0x100)) ^ (noiseBit << 8);
		sample += (int32_t)op_get_wave(chan_op(ch, 3), sdIndex, sdVol);
	}

	sample += (int32_t)op_get_sample(chan_op(ch, 4), 0);

	uint32_t tcVol = (uint32_t)op_forward_volume(chan_op(ch, 5));

	if (!ENV_SILENT(tcVol)) {
		uint32_t tcIndex = (1 + phaseBit) << 8;
		sample += (int32_t)op_get_wave(chan_op(ch, 5), tcIndex, tcVol);
	}

	sample <<= 1;

	if (opl3Mode) {
		output[0] += sample;
		output[1] += sample;

	} else
		output[0] += sample;
}

/* ===== BlockTemplate<mode> -- one inlined body, 10 wrappers ============== */

HOT_INLINE Channel*
block_body(Channel* ch, Chip* chip, uint32_t samples,
		   int32_t* output, const SynthMode mode)
{
	/*  Early-out for fully silent operator combinations -- one branch
	    survives per `mode` thanks to constant propagation. */
	switch (mode) {
		case sm2AM:
		case sm3AM:
			if (op_silent(chan_op(ch, 0)) && op_silent(chan_op(ch, 1))) {
				ch->old[0] = ch->old[1] = 0;
				return ch + 1;
			}

			break;

		case sm2FM:
		case sm3FM:
			if (op_silent(chan_op(ch, 1))) {
				ch->old[0] = ch->old[1] = 0;
				return ch + 1;
			}

			break;

		case sm3FMFM:
			if (op_silent(chan_op(ch, 3))) {
				ch->old[0] = ch->old[1] = 0;
				return ch + 2;
			}

			break;

		case sm3AMFM:
			if (op_silent(chan_op(ch, 0)) && op_silent(chan_op(ch, 3))) {
				ch->old[0] = ch->old[1] = 0;
				return ch + 2;
			}

			break;

		case sm3FMAM:
			if (op_silent(chan_op(ch, 1)) && op_silent(chan_op(ch, 3))) {
				ch->old[0] = ch->old[1] = 0;
				return ch + 2;
			}

			break;

		case sm3AMAM:
			if (op_silent(chan_op(ch, 0)) && op_silent(chan_op(ch, 2)) && op_silent(chan_op(ch, 3))) {
				ch->old[0] = ch->old[1] = 0;
				return ch + 2;
			}

			break;

		default:
			break;
	}

	op_prepare(chan_op(ch, 0), chip);
	op_prepare(chan_op(ch, 1), chip);

	if (mode > sm4Start) {
		op_prepare(chan_op(ch, 2), chip);
		op_prepare(chan_op(ch, 3), chip);
	}

	if (mode > sm6Start) {
		op_prepare(chan_op(ch, 4), chip);
		op_prepare(chan_op(ch, 5), chip);
	}

	for (Bitu i = 0; i < samples; i++) {
		if (mode == sm2Percussion) {
			gen_percussion(ch, chip, output + i, 0);
			continue;

		} else if (mode == sm3Percussion) {
			gen_percussion(ch, chip, output + i * 2, 1);
			continue;
		}

		int32_t mod = (int32_t)((uint32_t)((ch->old[0] + ch->old[1])) >> ch->feedback);
		ch->old[0] = ch->old[1];
		ch->old[1] = (int32_t)op_get_sample(chan_op(ch, 0), mod);
		int32_t sample = 0;
		int32_t out0   = ch->old[0];

		if (mode == sm2AM || mode == sm3AM)
			sample = (int32_t)(out0 + op_get_sample(chan_op(ch, 1), 0));

		else if (mode == sm2FM || mode == sm3FM)
			sample = (int32_t)op_get_sample(chan_op(ch, 1), out0);

		else if (mode == sm3FMFM) {
			Bits next = op_get_sample(chan_op(ch, 1), out0);
			next      = op_get_sample(chan_op(ch, 2), next);
			sample    = (int32_t)op_get_sample(chan_op(ch, 3), next);

		} else if (mode == sm3AMFM) {
			sample    = out0;
			Bits next = op_get_sample(chan_op(ch, 1), 0);
			next      = op_get_sample(chan_op(ch, 2), next);
			sample   += (int32_t)op_get_sample(chan_op(ch, 3), next);

		} else if (mode == sm3FMAM) {
			sample    = (int32_t)op_get_sample(chan_op(ch, 1), out0);
			Bits next = op_get_sample(chan_op(ch, 2), 0);
			sample   += (int32_t)op_get_sample(chan_op(ch, 3), next);

		} else if (mode == sm3AMAM) {
			sample    = out0;
			Bits next = op_get_sample(chan_op(ch, 1), 0);
			sample   += (int32_t)op_get_sample(chan_op(ch, 2), next);
			sample   += (int32_t)op_get_sample(chan_op(ch, 3), 0);
		}

		switch (mode) {
			case sm2AM:
			case sm2FM:
				output[i] += sample;
				break;

			case sm3AM:
			case sm3FM:
			case sm3FMFM:
			case sm3AMFM:
			case sm3FMAM:
			case sm3AMAM:
				output[i * 2 + 0] += sample & ch->maskLeft;
				output[i * 2 + 1] += sample & ch->maskRight;
				break;

			default:
				break;
		}
	}

	switch (mode) {
		case sm2AM:
		case sm2FM:
		case sm3AM:
		case sm3FM:
			return ch + 1;

		case sm3FMFM:
		case sm3AMFM:
		case sm3FMAM:
		case sm3AMAM:
			return ch + 2;

		case sm2Percussion:
		case sm3Percussion:
			return ch + 3;

		default:
			break;
	}

	return NULL;
}

/*  Thin wrappers -- one per SynthMode, gcc -O2 specialises block_body() per
    caller, equivalent to BlockTemplate<mode> in the C++ original. */
static Channel*
synth_2am(Channel* ch, Chip* chip, uint32_t s, int32_t* o)
{
	return block_body(ch, chip, s, o, sm2AM);
}
static Channel*
synth_2fm(Channel* ch, Chip* chip, uint32_t s, int32_t* o)
{
	return block_body(ch, chip, s, o, sm2FM);
}
static Channel*
synth_3am(Channel* ch, Chip* chip, uint32_t s, int32_t* o)
{
	return block_body(ch, chip, s, o, sm3AM);
}
static Channel*
synth_3fm(Channel* ch, Chip* chip, uint32_t s, int32_t* o)
{
	return block_body(ch, chip, s, o, sm3FM);
}
static Channel*
synth_3fmfm(Channel* ch, Chip* chip, uint32_t s, int32_t* o)
{
	return block_body(ch, chip, s, o, sm3FMFM);
}
static Channel*
synth_3amfm(Channel* ch, Chip* chip, uint32_t s, int32_t* o)
{
	return block_body(ch, chip, s, o, sm3AMFM);
}
static Channel*
synth_3fmam(Channel* ch, Chip* chip, uint32_t s, int32_t* o)
{
	return block_body(ch, chip, s, o, sm3FMAM);
}
static Channel*
synth_3amam(Channel* ch, Chip* chip, uint32_t s, int32_t* o)
{
	return block_body(ch, chip, s, o, sm3AMAM);
}
static Channel*
synth_2perc(Channel* ch, Chip* chip, uint32_t s, int32_t* o)
{
	return block_body(ch, chip, s, o, sm2Percussion);
}
static Channel*
synth_3perc(Channel* ch, Chip* chip, uint32_t s, int32_t* o)
{
	return block_body(ch, chip, s, o, sm3Percussion);
}

/* ===== Chip: register dispatch =========================================== */

static void
chip_write_reg(Chip* chip, uint32_t reg, uint8_t val);

static void
chip_update_synths(Chip* chip)
{
	for (int i = 0; i < 18; i++)
		ch_update_synth(&chip->chan[i], chip);
}

static void
chip_write_bd(Chip* chip, uint8_t val)
{
	uint8_t change = chip->regBD ^ val;

	if (!change)
		return;

	chip->regBD = val;
	chip->vibratoStrength = (val & 0x40) ? 0x00 : 0x01;
	chip->tremoloStrength = (val & 0x80) ? 0x00 : 0x02;

	if (val & 0x20) {
		if (change & 0x20) {
			if (chip->opl3Active)
				chip->chan[6].synthHandler = synth_3perc;

			else
				chip->chan[6].synthHandler = synth_2perc;
		}

		/* Bass drum */
		if (val & 0x10) {
			op_key_on(&chip->chan[6].op[0], 0x2);
			op_key_on(&chip->chan[6].op[1], 0x2);

		} else            {
			op_key_off(&chip->chan[6].op[0], 0x2);
			op_key_off(&chip->chan[6].op[1], 0x2);
		}

		/* Hi-hat */
		if (val & 0x1)
			op_key_on(&chip->chan[7].op[0], 0x2);

		else
			op_key_off(&chip->chan[7].op[0], 0x2);

		/* Snare */
		if (val & 0x8)
			op_key_on(&chip->chan[7].op[1], 0x2);

		else
			op_key_off(&chip->chan[7].op[1], 0x2);

		/* Tom */
		if (val & 0x4)
			op_key_on(&chip->chan[8].op[0], 0x2);

		else
			op_key_off(&chip->chan[8].op[0], 0x2);

		/* Top cymbal */
		if (val & 0x2)
			op_key_on(&chip->chan[8].op[1], 0x2);

		else
			op_key_off(&chip->chan[8].op[1], 0x2);

	} else if (change & 0x20) {
		ch_update_synth(&chip->chan[6], chip);
		op_key_off(&chip->chan[6].op[0], 0x2);
		op_key_off(&chip->chan[6].op[1], 0x2);
		op_key_off(&chip->chan[7].op[0], 0x2);
		op_key_off(&chip->chan[7].op[1], 0x2);
		op_key_off(&chip->chan[8].op[0], 0x2);
		op_key_off(&chip->chan[8].op[1], 0x2);
	}
}

#define REGOP(_FUNC_) do {                                                  \
		index = ((reg >> 3) & 0x20) | (reg & 0x1f);                         \
		if (OpOffsetTable[index]) {                                         \
			Operator* regOp = (Operator*)((char*)chip + OpOffsetTable[index] - 1); \
			_FUNC_(regOp, chip, val);                                       \
		}                                                                   \
	} while (0)

#define REGCHAN(_FUNC_) do {                                                \
		index = ((reg >> 4) & 0x10) | (reg & 0xf);                          \
		if (ChanOffsetTable[index]) {                                       \
			Channel* regChan = (Channel*)((char*)chip + ChanOffsetTable[index] - 1); \
			_FUNC_(regChan, chip, val);                                     \
		}                                                                   \
	} while (0)

static void
chip_write_reg(Chip* chip, uint32_t reg, uint8_t val)
{
	Bitu index;

	switch ((reg & 0xf0) >> 4) {
		case 0x00 >> 4:
			if (reg == 0x01)
				chip->waveFormMask = ((val & 0x20) || chip->opl3Mode) ? 0x7 : 0x0;

			else if (reg == 0x104) {
				if (!((chip->reg104 ^ val) & 0x3f))
					return;

				chip->reg104 = 0x80 | (val & 0x3f);
				chip_update_synths(chip);

			} else if (reg == 0x105) {
				if (!((chip->opl3Active ^ val) & 1))
					return;

				chip->opl3Active = (val & 1) ? (int8_t)0xff : 0;
				chip_update_synths(chip);

			} else if (reg == 0x08)
				chip->reg08 = val;

		/* fallthrough -- matches upstream which has no break here */
		case 0x10 >> 4:
			break;

		case 0x20 >> 4:
		case 0x30 >> 4:
			REGOP(op_write20);
			break;

		case 0x40 >> 4:
		case 0x50 >> 4:
			REGOP(op_write40);
			break;

		case 0x60 >> 4:
		case 0x70 >> 4:
			REGOP(op_write60);
			break;

		case 0x80 >> 4:
		case 0x90 >> 4:
			REGOP(op_write80);
			break;

		case 0xa0 >> 4:
			REGCHAN(ch_write_a0);
			break;

		case 0xb0 >> 4:
			if (reg == 0xbd)
				chip_write_bd(chip, val);

			else
				REGCHAN(ch_write_b0);

			break;

		case 0xc0 >> 4:
			REGCHAN(ch_write_c0);

		/* fallthrough -- matches upstream */
		case 0xd0 >> 4:
			break;

		case 0xe0 >> 4:
		case 0xf0 >> 4:
			REGOP(op_write_e0);
			break;
	}
}

/*  Kept for completeness / future OPL2-mono build path; OPL3 mode uses
    chip_generate_block3() exclusively. */
__attribute__((unused))
static void chip_generate_block2(Chip* chip, Bitu total, int32_t* output)
{
	while (total > 0) {
		uint32_t samples = chip_forward_lfo(chip, (uint32_t)total);
		memset(output, 0, sizeof(int32_t) * samples);

		for (Channel* ch = chip->chan; ch < chip->chan + 9;)
			ch = ch->synthHandler(ch, chip, samples, output);

		total -= samples;
		output += samples;
	}
}

static void
chip_generate_block3(Chip* chip, Bitu total, int32_t* output)
{
	while (total > 0) {
		uint32_t samples = chip_forward_lfo(chip, (uint32_t)total);
		memset(output, 0, sizeof(int32_t) * samples * 2);

		for (Channel* ch = chip->chan; ch < chip->chan + 18;)
			ch = ch->synthHandler(ch, chip, samples, output);

		total -= samples;
		output += samples * 2;
	}
}

/* ===== Table init (one-shot, double-precision) =========================== */

static void
init_tables(void)
{
	if (doneTables)
		return;

	doneTables = 1;

	/*  MulTable: precomputed log2 -> linear lookup for the WAVE_TABLEMUL fast
	    path (this is what makes dbopl avoid runtime exp/log on every sample). */
	for (int i = 0; i < 384; i++) {
		int s = i * 8;
		double val = 0.5 + (pow(2.0, -1.0 + (255 - s) * (1.0 / 256))) * (1 << MUL_SH);
		MulTable[i] = (uint16_t)val;
	}

	/* Sine wave base */
	for (int i = 0; i < 512; i++) {
		WaveTable[0x0200 + i] = (int16_t)(sin((i + 0.5) * (PI / 512.0)) * 4084);
		WaveTable[0x0000 + i] = -WaveTable[0x200 + i];
	}

	/* Exponential wave (waveform 7) */
	for (int i = 0; i < 256; i++) {
		WaveTable[0x700 + i] = (int16_t)(0.5 + (pow(2.0, -1.0 + (255 - i * 8) * (1.0 / 256))) * 4085);
		WaveTable[0x6ff - i] = -WaveTable[0x700 + i];
	}

	/* Silence and replicated waves */
	for (int i = 0; i < 256; i++) {
		WaveTable[0x400 + i] = WaveTable[0];
		WaveTable[0x500 + i] = WaveTable[0];
		WaveTable[0x900 + i] = WaveTable[0];
		WaveTable[0xc00 + i] = WaveTable[0];
		WaveTable[0xd00 + i] = WaveTable[0];
		WaveTable[0x800 + i] = WaveTable[0x200 + i];
		WaveTable[0xa00 + i] = WaveTable[0x200 + i * 2];
		WaveTable[0xb00 + i] = WaveTable[0x000 + i * 2];
		WaveTable[0xe00 + i] = WaveTable[0x200 + i * 2];
		WaveTable[0xf00 + i] = WaveTable[0x200 + i * 2];
	}

	/* KSL */
	for (int oct = 0; oct < 8; oct++) {
		int base = oct * 8;

		for (int i = 0; i < 16; i++) {
			int val = base - KslCreateTable[i];

			if (val < 0)
				val = 0;

			KslTable[oct * 16 + i] = val * 4;
		}
	}

	/* Tremolo (triangle) */
	for (uint8_t i = 0; i < TREMOLO_TABLE / 2; i++) {
		uint8_t v = i << ENV_EXTRA;
		TremoloTable[i] = v;
		TremoloTable[TREMOLO_TABLE - 1 - i] = v;
	}

	/*  Channel/operator offset tables -- 1-based byte offsets from chip
	    start, used by the REGCHAN/REGOP macros. Same layout as upstream
	    (and dependent on the Channel/Operator field layout above). */
	for (Bitu i = 0; i < 32; i++) {
		Bitu index = i & 0xf;

		if (index >= 9) {
			ChanOffsetTable[i] = 0;
			continue;
		}

		if (index < 6)
			index = (index % 3) * 2 + (index / 3);

		if (i >= 16)
			index += 9;

		ChanOffsetTable[i] = 1 + (uint16_t)(index * sizeof(Channel));
	}

	for (Bitu i = 0; i < 64; i++) {
		if (i % 8 >= 6 || ((i / 8) % 4 == 3)) {
			OpOffsetTable[i] = 0;
			continue;
		}

		Bitu chNum = (i / 8) * 3 + (i % 8) % 3;

		if (chNum >= 12)
			chNum += 16 - 12;

		Bitu opNum = (i % 8) / 3;
		OpOffsetTable[i] = ChanOffsetTable[chNum] + (uint16_t)(opNum * sizeof(Operator));
	}
}

/* ===== Chip setup ======================================================== */

static void
chip_setup(Chip* chip, uint32_t rate)
{
	double original = OPLRATE;
	double scale    = original / (double)rate;

	chip->noiseAdd     = (uint32_t)(0.5 + scale * (1 << LFO_SH));
	chip->noiseCounter = 0;
	chip->noiseValue   = 1;
	chip->lfoAdd       = (uint32_t)(0.5 + scale * (1 << LFO_SH));
	chip->lfoCounter   = 0;
	chip->vibratoIndex = 0;
	chip->tremoloIndex = 0;

	uint32_t freqScale = (uint32_t)(0.5 + scale * (1 << (WAVE_SH - 1 - 10)));

	for (int i = 0; i < 16; i++)
		chip->freqMul[i] = freqScale * FreqCreateTable[i];

	for (uint8_t i = 0; i < 76; i++) {
		uint8_t index, shift;
		EnvelopeSelect(i, &index, &shift);
		chip->linearRates[i] =
			(uint32_t)(scale * (EnvelopeIncreaseTable[index] << (RATE_SH + ENV_EXTRA - shift - 3)));
	}

	/*  Best-match attack-rate search -- one pass of binary search per rate.
	    Only runs at chip setup, never per sample. */
	for (uint8_t i = 0; i < 62; i++) {
		uint8_t index, shift;
		EnvelopeSelect(i, &index, &shift);
		int32_t originalAmount = (int32_t)((uint32_t)((AttackSamplesTable[index] << shift) / scale));
		int32_t guessAdd = (int32_t)((uint32_t)(scale * (EnvelopeIncreaseTable[index] << (RATE_SH - shift - 3))));
		int32_t bestAdd  = guessAdd;
		uint32_t bestDiff = 1u << 30;

		for (uint32_t passes = 0; passes < 16; passes++) {
			int32_t volume  = ENV_MAX;
			int32_t samples = 0;
			uint32_t count  = 0;

			while (volume > 0 && samples < originalAmount * 2) {
				count += (uint32_t)guessAdd;
				int32_t change = (int32_t)(count >> RATE_SH);
				count &= RATE_MASK;

				if (change)
					volume += (~volume * change) >> 3;

				samples++;
			}

			int32_t diff   = originalAmount - samples;
			uint32_t lDiff = (uint32_t)labs(diff);

			if (lDiff < bestDiff) {
				bestDiff = lDiff;
				bestAdd  = guessAdd;

				if (!bestDiff)
					break;
			}

			double correct = (originalAmount - diff) / (double)originalAmount;
			guessAdd = (int32_t)(guessAdd * correct);

			if (diff < 0)
				guessAdd++;
		}

		chip->attackRates[i] = (uint32_t)bestAdd;
	}

	for (uint8_t i = 62; i < 76; i++)
		chip->attackRates[i] = 8u << RATE_SH;

	/* Four-op channel mask layout (matches the OPL3 register 0x104 bits) */
	chip->chan[ 0].fourMask = 0x00 | (1 << 0);
	chip->chan[ 1].fourMask = 0x80 | (1 << 0);
	chip->chan[ 2].fourMask = 0x00 | (1 << 1);
	chip->chan[ 3].fourMask = 0x80 | (1 << 1);
	chip->chan[ 4].fourMask = 0x00 | (1 << 2);
	chip->chan[ 5].fourMask = 0x80 | (1 << 2);

	chip->chan[ 9].fourMask = 0x00 | (1 << 3);
	chip->chan[10].fourMask = 0x80 | (1 << 3);
	chip->chan[11].fourMask = 0x00 | (1 << 4);
	chip->chan[12].fourMask = 0x80 | (1 << 4);
	chip->chan[13].fourMask = 0x00 | (1 << 5);
	chip->chan[14].fourMask = 0x80 | (1 << 5);

	chip->chan[ 6].fourMask = 0x40;
	chip->chan[ 7].fourMask = 0x40;
	chip->chan[ 8].fourMask = 0x40;

	/* Reset register file -- once in OPL3 mode, once in OPL2 mode. */
	chip_write_reg(chip, 0x105, 0x1);

	for (unsigned i = 0; i < 512; i++) {
		if (i == 0x105)
			continue;

		chip_write_reg(chip, i, 0xff);
		chip_write_reg(chip, i,  0x0);
	}

	chip_write_reg(chip, 0x105, 0x0);

	for (unsigned i = 0; i < 255; i++) {
		chip_write_reg(chip, i, 0xff);
		chip_write_reg(chip, i,  0x0);
	}
}

static void
chip_init(Chip* chip, int opl3Mode)
{
	memset(chip, 0, sizeof(*chip));
	chip->opl3Mode = (uint8_t)(opl3Mode != 0);

	for (int i = 0; i < 18; i++)
		ch_init(&chip->chan[i]);
}

/* ===== Public OPL3 facade (matches cores/<other>/opl3.h API) ============= */

static int16_t
clip_i16(int32_t s)
{
	if (s >  32767)
		return  32767;

	if (s < -32768)
		return -32768;

	return (int16_t)s;
}

void
OPL3_Reset(opl3_chip* chip, uint32_t samplerate)
{
	if (chip->impl) {
		free(chip->impl);
		chip->impl = NULL;
	}

	init_tables();
	Chip* c = (Chip*)malloc(sizeof(Chip));
	chip_init(c, /*opl3Mode=*/1);
	chip_setup(c, samplerate);
	/* Enable OPL3 mode (NEW bit). */
	chip_write_reg(c, 0x105, 0x01);
	chip->impl = c;
}

void
OPL3_WriteReg(opl3_chip* chip, uint16_t reg, uint8_t val)
{
	chip_write_reg((Chip*)chip->impl, reg, val);
}

void
OPL3_WriteRegBuffered(opl3_chip* chip, uint16_t reg, uint8_t val)
{
	chip_write_reg((Chip*)chip->impl, reg, val);
}

void
OPL3_Generate(opl3_chip* chip, int16_t out[2])
{
	int32_t buf[2] = { 0, 0 };
	chip_generate_block3((Chip*)chip->impl, 1, buf);
	out[0] = clip_i16(buf[0]);
	out[1] = clip_i16(buf[1]);
}

void
OPL3_GenerateResampled(opl3_chip* chip, int16_t out[2])
{
	OPL3_Generate(chip, out);
}



