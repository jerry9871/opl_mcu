/*
    dro_load.c — runtime DRO v2 loader.  Decodes a DOSBox 0.74 DRO
    capture into an array of opl_event suitable for seq_play().

    The decode logic mirrors tools/dro2hdr.c — see that file for the
    DRO v2 format notes (single uint32 version field, 5-byte hardware
    header, codemap, then 2-byte (cmd,val) pairs with short/long delay
    codes).

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
dro_load(const char* path,
		 opl_event** out_events,
		 uint32_t*   out_count,
		 uint32_t*   out_total_ms)
{
	*out_events   = NULL;
	*out_count    = 0;
	*out_total_ms = 0;

	FILE* f = fopen(path, "rb");

	if (!f) {
		perror(path);
		return 1;
	}

	char m[8];
	uint32_t v, lp, lm;
	uint8_t hdr[5], cmLen, cm[256] = {0};

	if (fread(m, 1, 8, f) != 8 || memcmp(m, "DBRAWOPL", 8) ||
		read_u32(f, &v) || read_u32(f, &lp) || read_u32(f, &lm) || v != 2) {
		fprintf(stderr, "%s: not a DRO v2 file\n", path);
		fclose(f);
		return 2;
	}

	if (fread(hdr, 1, 5, f) != 5) {
		fclose(f);
		return 3;
	}

	uint8_t sc = hdr[3], lc = hdr[4];

	if (fread(&cmLen, 1, 1, f) != 1) {
		fclose(f);
		return 4;
	}

	if (cmLen && fread(cm, 1, cmLen, f) != cmLen) {
		fclose(f);
		return 5;
	}

	uint8_t* raw = malloc((size_t)lp * 2);

	if (!raw) {
		fclose(f);
		return 6;
	}

	if (fread(raw, 1, (size_t)lp * 2, f) != (size_t)lp * 2) {
		fprintf(stderr, "%s: short read\n", path);
		free(raw);
		fclose(f);
		return 7;
	}

	fclose(f);

	/*  First pass: decode (reg, val, delay_after_ms) with 32-bit delay. */
	typedef struct {
		uint16_t reg;
		uint8_t  val;
		uint32_t delay_ms;
	} E;
	E* ev = malloc(sizeof(E) * lp);

	if (!ev) {
		free(raw);
		return 8;
	}

	size_t nev = 0;

	for (uint32_t i = 0; i < lp; i++) {
		uint8_t c = raw[i * 2], val = raw[i * 2 + 1];

		if (c == sc) {
			if (nev)
				ev[nev - 1].delay_ms += (uint32_t)val + 1;

		} else if (c == lc) {
			if (nev)
				ev[nev - 1].delay_ms += ((uint32_t)val + 1) * 256u;

		} else {
			uint8_t idx = c & 0x7F;

			if (idx >= cmLen)
				continue;

			ev[nev].reg      = cm[idx] | ((c & 0x80) ? 0x100 : 0);
			ev[nev].val      = val;
			ev[nev].delay_ms = 0;
			nev++;
		}
	}

	free(raw);

	/*  Second pass: split delays > 65535 ms (opl_event.delay_ms is u16). */
	size_t cap = nev * 2 + 16;
	opl_event* out = malloc(sizeof(opl_event) * cap);

	if (!out) {
		free(ev);
		return 9;
	}

	size_t nout = 0;
	uint32_t total = 0;

	for (size_t i = 0; i < nev; i++) {
		E e = ev[i];

		while (e.delay_ms > 65535u) {
			out[nout].reg      = e.reg;
			out[nout].val      = e.val;
			out[nout].delay_ms = 65535;
			nout++;
			total += 65535;
			e.delay_ms -= 65535u;
			/* dummy continuation: re-write same reg/val (no audible effect) */
			out[nout].reg      = e.reg;
			out[nout].val      = e.val;
			out[nout].delay_ms = 0;
			nout++;
		}

		out[nout].reg      = e.reg;
		out[nout].val      = e.val;
		out[nout].delay_ms = (uint16_t)e.delay_ms;
		nout++;
		total += e.delay_ms;
	}

	free(ev);

	*out_events   = out;
	*out_count    = (uint32_t)nout;
	*out_total_ms = total;
	return 0;
}
