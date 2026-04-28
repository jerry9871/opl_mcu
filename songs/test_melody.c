/*
    test_melody.c — built-in royalty-free demo tune (Beethoven, "Ode to Joy",
    1824, public domain).  Plays through a single 2-operator FM voice on
    channel 0.  Small example of the seq_player.h embedded-song format:
    an array of {reg, val, delay_ms_after} triples.
*/
#include "seq_player.h"

#define BLK 4
#define KEYON  (0x20 | (BLK << 2))
#define KEYOFF (         BLK << 2 )

#define FNUM_C  343
#define FNUM_D  385
#define FNUM_E  432
#define FNUM_F  458
#define FNUM_G  514

#define HOLD_MS(ms)  {0xB0, (uint8_t)(KEYOFF), (uint16_t)(ms)}

#define Q_ON  380
#define Q_OFF  80
#define H_ON  780
#define H_OFF 100

#define PLAY(fn, on, off) \
	{0xA0, (uint8_t)((fn) & 0xFF),                     0}, \
	{0xB0, (uint8_t)(KEYON  | ((fn) >> 8)), (uint16_t)(on)}, \
	{0xB0, (uint8_t)(KEYOFF | ((fn) >> 8)), (uint16_t)(off)}

const opl_event opl_demo_melody[] = {
	{0x105, 0x01, 0},
	{0x001, 0x20, 0},
	{0x020, 0x01, 0},
	{0x023, 0x01, 0},
	{0x040, 0x14, 0},
	{0x043, 0x00, 0},
	{0x060, 0xF2, 0},
	{0x063, 0xF2, 0},
	{0x080, 0x57, 0},
	{0x083, 0x57, 0},
	{0x0E0, 0x00, 0},
	{0x0E3, 0x00, 0},
	{0x0C0, 0x38, 0},

	HOLD_MS(150),

	PLAY(FNUM_E, Q_ON, Q_OFF),  PLAY(FNUM_E, Q_ON, Q_OFF),
	PLAY(FNUM_F, Q_ON, Q_OFF),  PLAY(FNUM_G, Q_ON, Q_OFF),
	PLAY(FNUM_G, Q_ON, Q_OFF),  PLAY(FNUM_F, Q_ON, Q_OFF),
	PLAY(FNUM_E, Q_ON, Q_OFF),  PLAY(FNUM_D, Q_ON, Q_OFF),
	PLAY(FNUM_C, Q_ON, Q_OFF),  PLAY(FNUM_C, Q_ON, Q_OFF),
	PLAY(FNUM_D, Q_ON, Q_OFF),  PLAY(FNUM_E, Q_ON, Q_OFF),
	PLAY(FNUM_E, H_ON, H_OFF),
	PLAY(FNUM_D, Q_ON, Q_OFF),
	PLAY(FNUM_D, H_ON, H_OFF),

	HOLD_MS(400),
};

const uint32_t opl_demo_melody_count =
	sizeof(opl_demo_melody) / sizeof(opl_demo_melody[0]);
