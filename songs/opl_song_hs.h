/*
    opl_song_hs.h — descriptor for a heatshrink-packed DRO v2 song.

    Header-only, no dependencies beyond <stdint.h>.  Emitted by
    `dro2hdr --hs` and consumed on both PC and MCU.  The packed
    payload is a complete DRO v2 file (26-byte header + codemap +
    opcode stream); the meta fields here mirror that file header so
    a player can stream the payload without first decompressing the
    file header itself.
*/
#ifndef OPL_SONG_HS_H
#define OPL_SONG_HS_H

#include <stdint.h>

typedef struct {
	const uint8_t* hs_data;       /* heatshrink-compressed DRO v2 bytes  */
	uint32_t       hs_len;        /* size of hs_data in bytes            */
	uint32_t       total_ms;      /* nominal song length                 */
	uint16_t       codemap_len;   /* codemap entries (<= 128)            */
	uint8_t        short_code;    /* opcode for short delays             */
	uint8_t        long_code;     /* opcode for long delays              */
	uint8_t        hs_window;     /* heatshrink window bits  (4..15)     */
	uint8_t        hs_lookahead;  /* heatshrink lookahead bits (3..w-1)  */
} opl_song_hs;

#endif
