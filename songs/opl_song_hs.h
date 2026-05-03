/*
    opl_song_hs.h — descriptor for a heatshrink-packed OPL song.

    Header-only, no dependencies beyond <stdint.h>.  Emitted by
    `dro2hdr --hs` and consumed on both PC and MCU.  The packed
    payload is just the codemap followed by the (code,val) opcode
    stream — the 26-byte DRO v2 file header is *not* included,
    since every field the player needs at runtime is mirrored in
    this descriptor at compile time.
*/
#ifndef OPL_SONG_HS_H
#define OPL_SONG_HS_H

#include <stdint.h>

typedef struct {
	const uint8_t* hs_data;       /* heatshrink-compressed codemap+stream */
	uint32_t       hs_len;        /* size of hs_data in bytes            */
	uint32_t       total_ms;      /* nominal song length                 */
	uint16_t       codemap_len;   /* codemap entries (<= 128)            */
	uint8_t        short_code;    /* opcode for short delays             */
	uint8_t        long_code;     /* opcode for long delays              */
	uint8_t        hs_window;     /* heatshrink window bits  (4..15)     */
	uint8_t        hs_lookahead;  /* heatshrink lookahead bits (3..w-1)  */
} opl_song_hs;

#endif
