/*
    dro_load.c — runtime DRO v2 loader.  Reads a DOSBox 0.74 DRO file
    and produces an `opl_song` whose `data` buffer mirrors the on-disk
    payload (codemap + (code,val) opcode stream), exactly as
    tools/dro2hdr writes.

    PC-only.  Not built on the MCU.
*/
#include "dro_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
read_u32(FILE* f, uint32_t* o)
{
	uint8_t b[4];

	if (fread(b, 1, 4, f) != 4)
		return -1;

	*o = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
		 ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	return 0;
}

int
dro_load(const char* path, opl_song* out)
{
	memset(out, 0, sizeof(*out));

	FILE* f = fopen(path, "rb");

	if (!f) {
		perror(path);
		return 1;
	}

	char m[8];
	uint32_t v, lp, lm;
	uint8_t hdr[5], cmLen;

	if (fread(m, 1, 8, f) != 8 || memcmp(m, "DBRAWOPL", 8) ||
		read_u32(f, &v) || read_u32(f, &lp) || read_u32(f, &lm) || v != 2) {
		fprintf(stderr, "%s: not a DRO v2 file\n", path);
		fclose(f);
		return 2;
	}

	if (fread(hdr, 1, 5, f) != 5 || fread(&cmLen, 1, 1, f) != 1) {
		fclose(f);
		return 3;
	}

	size_t total = (size_t)cmLen + (size_t)lp * 2;
	uint8_t* buf = malloc(total);

	if (!buf) {
		fclose(f);
		return 4;
	}

	if ((cmLen && fread(buf, 1, cmLen, f) != cmLen) ||
		fread(buf + cmLen, 1, (size_t)lp * 2, f) != (size_t)lp * 2) {
		fprintf(stderr, "%s: short read\n", path);
		free(buf);
		fclose(f);
		return 5;
	}

	fclose(f);

	out->data        = buf;
	out->data_len    = (uint32_t)total;
	out->codemap_len = cmLen;
	out->short_code  = hdr[3];
	out->long_code   = hdr[4];
	out->total_ms    = lm;
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
