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
    Original sources kept at c:/silixcon-devel/opl/core/ for diffing.
    See opl3.c top-of-file banner for the full optimization catalog.  This
    header only documents the parts that are visible in the public API:

     Compile-time switches (override before including this header)
     --------------------------------------------------------------
     OPL_MONO              (default 1)
         Skip the right channel (chb/chd) mix loop.  Halves the channel-mix
         cost when only one PWM output is wired.  Set to 0 for stereo.

     OPL_MAX_CHANNELS      (default 9, range 1..18)
         Cap on simultaneously audible channels.  Slots on channels
         with index >= OPL_MAX_CHANNELS refuse to wake on key-on so they
         cost zero in the mix loop and the active-slot walker.  Register
         writes still go through normally so sequencer state stays
         consistent.  Set to 18 to disable the cap.

     OPL_FORCE_OPL2        (default 1)
         Pin chip->newm to 0 — no 4-op pairing, no waveforms 4-7.  Lets
         the compiler strip wf 4-7 cases from SlotGenerate and simplifies
         the channel-mix silent-skip predicate.  Lossy for songs that
         depend on OPL3 features.  OPL2-era songs (Prehistorik 2, WW
         theme) are unaffected.  Set to 0 for full OPL3.

     OPL_WRITEBUF_SIZE     (must be a power of two; default 1024)
         Used as a bitwise mask in the writebuf wrap so the audio path
         avoids a hardware divide.

     Struct extensions vs. upstream Nuked
     -------------------------------------
     _opl3_slot:    is_silent, active_idx, eg_static, phase_step,
                    phase_step_valid, is_modulator
     _opl3_channel: active_slots
     _opl3_chip:    eg_timer narrowed uint64_t?uint32_t,
                    active_slot_count, active_slot_idx[36]
    ============================================================================ */

#ifndef OPL_OPL3_H
#define OPL_OPL3_H

#ifdef __cplusplus
extern "C" {
#endif

#include <inttypes.h>

//#ifndef OPL_ENABLE_STEREOEXT
//#define OPL_ENABLE_STEREOEXT 0
//#endif

/* OPT: mono mix path \u2014 skip the right (chb/chd) mix loop entirely. */
#ifndef OPL_MONO
#define OPL_MONO 1
#endif

/*  Compile-time channel cap for performance experiments.  Range 1..18.
    Slots belonging to channels with index >= OPL_MAX_CHANNELS will simply
    refuse to wake on key-on, so they cost zero in the mix loop and
    active-slot walker.  Register writes still go through normally so the
    sequencer state stays consistent.  Set to 18 to disable the cap. */
#ifndef OPL_MAX_CHANNELS
#define OPL_MAX_CHANNELS 9
#endif

/*  OPT: OPL2-only build.  Forces chip->newm=0 (no 4-op pairing, only
    waveforms 0-3) and lets the compiler strip wf 4-7 from SlotGenerate
    + simplifies the channel-mix silent-skip predicate.  Songs that rely
    on OPL3 features will sound degraded.  Both Prehistorik 2 and the WW
    theme are OPL2-era so are safe.  Set to 0 for full OPL3. */
#ifndef OPL_FORCE_OPL2
#define OPL_FORCE_OPL2 1
#endif
#define OPL_WRITEBUF_SIZE   1024
#define OPL_WRITEBUF_DELAY  2

typedef struct _opl3_slot opl3_slot;
typedef struct _opl3_channel opl3_channel;
typedef struct _opl3_chip opl3_chip;

struct _opl3_slot {
	opl3_channel* channel;
	opl3_chip* chip;
	int16_t out;
	int16_t fbmod;
	int16_t* mod;
	int16_t prout;
	uint16_t eg_rout;
	uint16_t eg_out;
	uint8_t eg_inc;
	uint8_t eg_gen;
	uint8_t eg_rate;
	uint8_t eg_ksl;
	uint8_t* trem;
	uint8_t reg_vib;
	uint8_t reg_type;
	uint8_t reg_ksr;
	uint8_t reg_mult;
	uint8_t reg_ksl;
	uint8_t reg_tl;
	uint8_t reg_ar;
	uint8_t reg_dr;
	uint8_t reg_sl;
	uint8_t reg_rr;
	uint8_t reg_wf;
	uint8_t key;
	uint8_t is_silent;   /* OPT: 1 = slot is fully released and producing 0 */
	uint8_t is_modulator;/* OPT: 1 if slot == channel->slotz[0] (only slot whose fbmod is read) */
	uint8_t active_idx;  /* OPT: position in chip->active_slot_idx[] when !is_silent */
	uint16_t eg_static;  /* OPT: cached (reg_tl<<2) + (eg_ksl >> kslshift[reg_ksl]) */
	uint32_t phase_step; /* OPT: cached pg_phase increment (valid only when phase_step_valid && !reg_vib) */
	uint8_t  phase_step_valid;
	uint32_t pg_reset;
	uint32_t pg_phase;
	uint16_t pg_phase_out;
	uint8_t slot_num;
};

struct _opl3_channel {
	opl3_slot* slotz[2];/*Don't use "slots" keyword to avoid conflict with Qt applications*/
	opl3_channel* pair;
	opl3_chip* chip;
	int16_t* out[4];

	#if OPL_ENABLE_STEREOEXT
	int32_t leftpan;
	int32_t rightpan;
	#endif

	uint8_t chtype;
	uint8_t active_slots; /* OPT: count of own (slotz[0..1]) slots that are not is_silent */
	uint16_t f_num;
	uint8_t block;
	uint8_t fb;
	uint8_t con;
	uint8_t alg;
	uint8_t ksv;
	uint16_t cha, chb;
	uint16_t chc, chd;
	uint8_t ch_num;
};

typedef struct _opl3_writebuf {
	uint64_t time;
	uint16_t reg;
	uint8_t data;
} opl3_writebuf;

struct _opl3_chip {
	opl3_channel channel[18];
	opl3_slot slot[36];
	uint16_t timer;
	uint32_t eg_timer;       /* OPT: was uint64_t; only low 13 bits used */
	uint8_t eg_timerrem;
	uint8_t eg_state;
	uint8_t eg_add;
	uint8_t eg_timer_lo;
	uint8_t newm;
	uint8_t nts;
	uint8_t rhy;
	uint8_t vibpos;
	uint8_t vibshift;
	uint8_t tremolo;
	uint8_t tremolopos;
	uint8_t tremoloshift;
	uint16_t active_slot_count;  /* OPT: # slots not in fully-silent release */
	uint8_t  active_slot_idx[36]; /* OPT: indices of active slots, compact list */
	uint32_t noise;
	int16_t zeromod;
	int32_t mixbuff[4];
	uint8_t rm_hh_bit2;
	uint8_t rm_hh_bit3;
	uint8_t rm_hh_bit7;
	uint8_t rm_hh_bit8;
	uint8_t rm_tc_bit3;
	uint8_t rm_tc_bit5;

	#if OPL_ENABLE_STEREOEXT
	uint8_t stereoext;
	#endif

	/* OPL3L */
	int32_t rateratio;
	int32_t samplecnt;
	int16_t oldsamples[4];
	int16_t samples[4];

	uint64_t writebuf_samplecnt;
	uint32_t writebuf_cur;
	uint32_t writebuf_last;
	uint64_t writebuf_lasttime;
	opl3_writebuf writebuf[OPL_WRITEBUF_SIZE];
};

void OPL3_Generate(opl3_chip* chip, int16_t* buf);
void OPL3_GenerateResampled(opl3_chip* chip, int16_t* buf);
void OPL3_Reset(opl3_chip* chip, uint32_t samplerate);
void OPL3_WriteReg(opl3_chip* chip, uint16_t reg, uint8_t v);
void OPL3_WriteRegBuffered(opl3_chip* chip, uint16_t reg, uint8_t v);
void OPL3_GenerateStream(opl3_chip* chip, int16_t* sndptr, uint32_t numsamples);

/*  OPT: OPL3_Generate4Ch is now `static inline always_inline` inside opl3.c so it
    fully dissolves into its callers; the external prototype is removed. */
void OPL3_Generate4ChResampled(opl3_chip* chip, int16_t* buf4);
void OPL3_Generate4ChStream(opl3_chip* chip, int16_t* sndptr1, int16_t* sndptr2, uint32_t numsamples);

#ifdef __cplusplus
}
#endif

#endif
