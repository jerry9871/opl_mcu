/*
    dro_load.c — runtime DRO v2 loader.

    Two paths, both PC-only:

      1. dro_load()    — plain ".dro" file ? opl_song (flat buffer).
                          Used by the demo's seq_play_song() path.

      2. dro_load_hs() — heatshrink-packed ".dro_hs<W>[l<L>]" file ?
                          opl_song_hs descriptor + owned compressed
                          buffer.  The caller then drives playback via
                          hs_stream + seq_play_stream(), exactly as
                          the MCU does for flash-embedded songs.

    There is intentionally no eager full-decompress helper here:
    embedded songs go through the streaming path, file-loaded
    ".dro_hs*" songs go through the streaming path, so the demo
    exercises the same code that ships on the MCU.
*/
#include "dro_load.h"

#include "../heatshrink/heatshrink_decoder.h"
#include "../heatshrink/hs_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------- */
/*  Slurp a file into a malloc'd buffer.                           */
/* --------------------------------------------------------------- */
static int
slurp(const char* path, uint8_t** out, size_t* out_n)
{
	FILE* f = fopen(path, "rb");

	if (!f) {
		perror(path);
		return -1;
	}

	fseek(f, 0, SEEK_END);
	size_t n = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);

	uint8_t* b = malloc(n ? n : 1);

	if (fread(b, 1, n, f) != n) {
		fclose(f);
		free(b);
		return -1;
	}

	fclose(f);
	*out   = b;
	*out_n = n;
	return 0;
}

/* --------------------------------------------------------------- */
/*  Detect ".dro_hs<W>[l<L>]" extension.  Sets *wbits and *lbits.  */
/*  Returns 0 on match, -1 otherwise.                              */
/* --------------------------------------------------------------- */
static int
parse_hs_ext(const char* path, int* wbits, int* lbits)
{
	const char* p = strrchr(path, '.');

	if (!p || strncmp(p, ".dro_hs", 7) != 0)
		return -1;

	p += 7;

	int w = 0, any = 0;

	while (*p >= '0' && *p <= '9') {
		w = w * 10 + (*p - '0');
		p++;
		any = 1;
	}

	if (!any)
		return -1;

	int l = 4;

	if (*p == 'l' || *p == 'L') {
		p++;
		int la = 0, lany = 0;

		while (*p >= '0' && *p <= '9') {
			la = la * 10 + (*p - '0');
			p++;
			lany = 1;
		}

		if (!lany)
			return -1;

		l = la;
	}

	if (*p != '\0')
		return -1;

	*wbits = w;
	*lbits = l;
	return 0;
}

/* --------------------------------------------------------------- */
/*  Parse a DRO v2 byte blob into an opl_song (caller owns mem).   */
/* --------------------------------------------------------------- */
static int
dro_parse(const uint8_t* d, size_t n, opl_song* out)
{
	if (n < 26 || memcmp(d, "DBRAWOPL", 8) != 0)
		return -1;

	uint32_t v  = (uint32_t)d[8]  | ((uint32_t)d[9]  << 8)  |
				  ((uint32_t)d[10] << 16) | ((uint32_t)d[11] << 24);
	uint32_t lp = (uint32_t)d[12] | ((uint32_t)d[13] << 8)  |
				  ((uint32_t)d[14] << 16) | ((uint32_t)d[15] << 24);
	uint32_t lm = (uint32_t)d[16] | ((uint32_t)d[17] << 8)  |
				  ((uint32_t)d[18] << 16) | ((uint32_t)d[19] << 24);

	if (v != 2)
		return -2;

	uint8_t sc    = d[23];
	uint8_t lc    = d[24];
	uint8_t cmLen = d[25];

	size_t need = 26u + (size_t)cmLen + (size_t)lp * 2u;

	if (n < need)
		return -3;

	size_t   total = (size_t)cmLen + (size_t)lp * 2u;
	uint8_t* buf   = malloc(total);

	memcpy(buf, d + 26, total);

	out->data        = buf;
	out->data_len    = (uint32_t)total;
	out->codemap_len = cmLen;
	out->short_code  = sc;
	out->long_code   = lc;
	out->total_ms    = lm;
	return 0;
}

int
dro_load(const char* path, opl_song* out)
{
	memset(out, 0, sizeof(*out));

	int wbits, lbits;

	if (parse_hs_ext(path, &wbits, &lbits) == 0) {
		fprintf(stderr,
				"%s: heatshrink-packed file -- use dro_load_hs()\n", path);
		return 4;
	}

	uint8_t* file;
	size_t   file_len;

	if (slurp(path, &file, &file_len) != 0)
		return 1;

	int rc = dro_parse(file, file_len, out);
	free(file);

	if (rc != 0) {
		fprintf(stderr, "%s: not a DRO v2 file (rc=%d)\n", path, rc);
		return 2;
	}

	return 0;
}

void
dro_song_free(opl_song* song)
{
	if (song && song->data) {
		free((void*)song->data);
		song->data = NULL;
	}
}

/* --------------------------------------------------------------- */
/*  Load a heatshrink-packed DRO file as an opl_song_hs descriptor.*/
/*                                                                 */
/* --------------------------------------------------------------- */
/*  Load a heatshrink-packed ".dro_hs<W>[l<L>]" file as an         */
/*  opl_song.  We fully decompress the file into RAM, parse the    */
/*  26-byte DRO v2 header to recover metadata, and return the      */
/*  trailing codemap+stream bytes as a flat opl_song.              */
/*                                                                 */
/*  This is the PC-side runtime path; the embedded build instead   */
/*  consumes opl_song_hs descriptors (emitted by dro2hdr --hs)     */
/*  whose payload is *just* the codemap+stream -- no DRO header,   */
/*  since the metadata is baked into the descriptor at compile     */
/*  time.  Keeping the runtime path eager keeps the file format    */
/*  self-describing while letting the streaming code stay simple.  */
/* --------------------------------------------------------------- */
int
dro_load_hs(const char* path, opl_song* out_song)
{
	memset(out_song, 0, sizeof(*out_song));

	int wbits, lbits;

	if (parse_hs_ext(path, &wbits, &lbits) != 0) {
		fprintf(stderr, "%s: not a .dro_hs<W>[l<L>] file\n", path);
		return 1;
	}

	uint8_t* file;
	size_t   file_len;

	if (slurp(path, &file, &file_len) != 0)
		return 2;

	heatshrink_decoder* dec =
		heatshrink_decoder_alloc(64, (uint8_t)wbits, (uint8_t)lbits);

	if (!dec) {
		free(file);
		return 3;
	}

	/*  Decompress everything to a growable buffer.  PC-side, so a
	    plain doubling realloc is fine. */
	hs_stream st;
	hs_stream_init(&st, file, (uint32_t)file_len, dec);

	size_t   cap = file_len * 4u + 64u;
	size_t   n   = 0;
	uint8_t* buf = malloc(cap);

	for (;;) {
		int b = hs_stream_next(&st);

		if (b < 0)
			break;

		if (n >= cap) {
			cap *= 2;
			buf  = realloc(buf, cap);
		}

		buf[n++] = (uint8_t)b;
	}

	heatshrink_decoder_free(dec);
	free(file);

	if (n < 26 || memcmp(buf, "DBRAWOPL", 8) != 0) {
		fprintf(stderr, "%s: not a DRO v2 file inside\n", path);
		free(buf);
		return 4;
	}

	uint32_t v = (uint32_t)buf[8]  | ((uint32_t)buf[9]  << 8)
				 | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);

	if (v != 2) {
		fprintf(stderr, "%s: DRO version %u, expected 2\n", path, v);
		free(buf);
		return 5;
	}

	uint32_t lm = (uint32_t)buf[16] | ((uint32_t)buf[17] << 8)
				  | ((uint32_t)buf[18] << 16) | ((uint32_t)buf[19] << 24);

	uint8_t  sc    = buf[23];
	uint8_t  lc    = buf[24];
	uint8_t  cmLen = buf[25];

	if (n < 26u + cmLen) {
		fprintf(stderr, "%s: truncated payload\n", path);
		free(buf);
		return 6;
	}

	/*  Move codemap+stream to the front, then shrink the allocation. */
	size_t payload = n - 26u;
	memmove(buf, buf + 26, payload);
	buf = realloc(buf, payload ? payload : 1);

	out_song->data        = buf;
	out_song->data_len    = (uint32_t)payload;
	out_song->codemap_len = cmLen;
	out_song->short_code  = sc;
	out_song->long_code   = lc;
	out_song->total_ms    = lm;
	return 0;
}
