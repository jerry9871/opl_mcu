/*
    dro_load.c — runtime DRO v2 loader (PC-only).

    One job: take a ".dro" file path, slurp it into RAM, parse the
    26-byte DRO v2 header, and hand back a flat opl_song that the
    sequencer can play directly.

    Heatshrink-packed songs are *built into the binary* via
    `tools/dro2hdr --hs` (codemap+stream gets compressed and the
    metadata becomes an opl_song_hs struct field).  There is
    intentionally no runtime ".dro_hs*" file format: the demo wraps
    a single representation end-to-end so newcomers don't have to
    pick between two file types.
*/
#include "dro_load.h"

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
