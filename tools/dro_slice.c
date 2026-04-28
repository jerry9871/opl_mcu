/*
    dro_slice.c — extract a time window from a DRO v2 capture and write
    it out as a new DRO v2 file.

    Usage:
       dro_slice  input.dro  output.dro  start_ms  end_ms

    The output preserves the original hardware-type bytes; the codemap
    is regenerated from the events that survive the slice.

    Notes:
        Only events firing strictly before end_ms and at-or-after
        start_ms are kept.
        The state at start_ms is reconstructed: every OPL register
        whose last write happened before start_ms is re-emitted as a
        zero-delay "preface" so the slice plays correctly even though
        you're jumping into the middle of the song.

    Offline tool — not part of the MCU build.
*/
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int
read_u32(FILE* f, uint32_t* o)
{
	uint8_t b[4];

	if (fread(b, 1, 4, f) != 4)
		return -1;

	*o = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	return 0;
}

static void
write_u32(FILE* f, uint32_t v)
{
	uint8_t b[4] = { v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF };
	fwrite(b, 1, 4, f);
}

typedef struct {
	uint32_t t_ms;
	uint16_t reg;
	uint8_t val;
} Ev;

int
main(int argc, char** argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: %s in.dro out.dro start_ms end_ms\n", argv[0]);
		return 1;
	}

	const char* in_path  = argv[1];
	const char* out_path = argv[2];
	uint32_t t_start = (uint32_t)strtoul(argv[3], NULL, 10);
	uint32_t t_end   = (uint32_t)strtoul(argv[4], NULL, 10);

	if (t_end <= t_start) {
		fprintf(stderr, "end_ms must be > start_ms\n");
		return 1;
	}

	FILE* f = fopen(in_path, "rb");

	if (!f) {
		perror(in_path);
		return 1;
	}

	char m[8];
	uint32_t v, lp, lm;
	uint8_t hdr[5], cmLen, cm[256] = {0};

	if (fread(m, 1, 8, f) != 8 || memcmp(m, "DBRAWOPL", 8) ||
		read_u32(f, &v) || read_u32(f, &lp) || read_u32(f, &lm) || v != 2) {
		fprintf(stderr, "%s: not DRO v2\n", in_path);
		return 2;
	}

	if (fread(hdr, 1, 5, f) != 5)
		return 3;

	uint8_t sc = hdr[3], lc = hdr[4];

	if (fread(&cmLen, 1, 1, f) != 1)
		return 4;

	if (cmLen && fread(cm, 1, cmLen, f) != cmLen)
		return 5;

	uint8_t* raw = malloc((size_t)lp * 2);

	if (fread(raw, 1, (size_t)lp * 2, f) != (size_t)lp * 2)
		return 7;

	fclose(f);

	Ev* ev = malloc(sizeof(Ev) * lp);
	size_t nev = 0;
	uint32_t now = 0;

	for (uint32_t i = 0; i < lp; i++) {
		uint8_t c = raw[i * 2], val = raw[i * 2 + 1];

		if (c == sc)
			now += (uint32_t)val + 1;

		else if (c == lc)
			now += ((uint32_t)val + 1) * 256u;

		else {
			uint8_t idx = c & 0x7F;

			if (idx >= cmLen)
				continue;

			ev[nev].t_ms = now;
			ev[nev].reg  = cm[idx] | ((c & 0x80) ? 0x100 : 0);
			ev[nev].val  = val;
			nev++;
		}
	}

	free(raw);

	/*  Reconstruct OPL register state at t_start: for every reg, take the
	    last write strictly before t_start. */
	uint8_t  state_val[512];
	uint8_t  state_set[512];
	memset(state_val, 0, sizeof(state_val));
	memset(state_set, 0, sizeof(state_set));

	for (size_t i = 0; i < nev; i++) {
		if (ev[i].t_ms >= t_start)
			break;

		state_val[ev[i].reg] = ev[i].val;
		state_set[ev[i].reg] = 1;
	}

	/*  Build the output event list:
	    1. Preface = all set state regs at t=0 (zero-delay).
	    2. Sliced events at (t - t_start). */
	Ev* out = malloc(sizeof(Ev) * (nev + 600));
	size_t no = 0;

	for (int r = 0; r < 512; r++) {
		if (state_set[r]) {
			out[no].t_ms = 0;
			out[no].reg  = (uint16_t)r;
			out[no].val  = state_val[r];
			no++;
		}
	}

	for (size_t i = 0; i < nev; i++) {
		if (ev[i].t_ms <  t_start)
			continue;

		if (ev[i].t_ms >= t_end)
			break;

		out[no].t_ms = ev[i].t_ms - t_start;
		out[no].reg  = ev[i].reg;
		out[no].val  = ev[i].val;
		no++;
	}

	free(ev);

	/* Build a fresh codemap from the registers used in `out`. */
	uint8_t  used[512] = {0};

	for (size_t i = 0; i < no; i++)
		used[out[i].reg] = 1;

	uint8_t  newcm[128];
	uint8_t newcmLen = 0;
	int16_t  reg_to_slot[512];

	for (int i = 0; i < 512; i++)
		reg_to_slot[i] = -1;

	for (int r = 0; r < 512; r++) {
		if (used[r]) {
			if (newcmLen >= 128) {
				fprintf(stderr, "too many distinct regs (>128)\n");
				return 9;
			}

			newcm[newcmLen] = (uint8_t)(r & 0xFF);  /* low byte = OPL reg */
			reg_to_slot[r]  = newcmLen;
			newcmLen++;
		}
	}

	/* Encode to (cmd,val) pairs, inserting short/long delay codes. */
	uint8_t* pairs = malloc(no * 4 + 4096);
	size_t   np = 0;
	uint32_t last_t = 0;

	/*  Choose short/long-delay command codes that don't collide with the
	    indexed range [0..newcmLen-1] OR their |0x80 high-bank counterparts.
	    The high-bank flag is bit 7, so any code >= 128 with low bits >=
	    newcmLen is free, and any code in [newcmLen..127] is free too. */
	uint8_t out_sc = newcmLen;             /* first free low-bank index */
	uint8_t out_lc = newcmLen + 1;

	if (out_lc > 127) {
		fprintf(stderr, "codemap full\n");
		return 10;
	}

	for (size_t i = 0; i < no; i++) {
		uint32_t dt = (i == 0) ? out[i].t_ms : (out[i].t_ms - last_t);
		last_t = out[i].t_ms;

		while (dt) {
			if (dt >= 256) {
				uint32_t units = dt / 256;

				if (units > 256)
					units = 256;

				pairs[np++] = out_lc;
				pairs[np++] = (uint8_t)(units - 1);
				dt -= units * 256;

			} else {
				pairs[np++] = out_sc;
				pairs[np++] = (uint8_t)(dt - 1);
				dt = 0;
			}
		}

		int slot = reg_to_slot[out[i].reg];
		uint8_t hi = (out[i].reg & 0x100) ? 0x80 : 0x00;
		pairs[np++] = (uint8_t)(slot | hi);
		pairs[np++] = out[i].val;
	}

	uint32_t out_lp = (uint32_t)(np / 2);
	uint32_t out_lm = (no ? out[no - 1].t_ms : 0);

	/* Write the output DRO. */
	FILE* g = fopen(out_path, "wb");

	if (!g) {
		perror(out_path);
		return 11;
	}

	fwrite("DBRAWOPL", 1, 8, g);
	write_u32(g, 2);                       /* DOSBox 0.74 single-uint32 ver */
	write_u32(g, out_lp);
	write_u32(g, out_lm);
	/* Preserve original hardware bytes 0..2; override sc/lc. */
	uint8_t out_hdr[5] = { hdr[0], hdr[1], hdr[2], out_sc, out_lc };
	fwrite(out_hdr, 1, 5, g);
	fwrite(&newcmLen, 1, 1, g);
	fwrite(newcm, 1, newcmLen, g);
	fwrite(pairs, 1, np, g);
	fclose(g);

	printf("Wrote %s: %u pairs, %u ms (%.2fs), %u regs in codemap.\n",
		   out_path, out_lp, out_lm, out_lm / 1000.0, newcmLen);
	printf("  preface events: %d state regs re-emitted at t=0\n",
		   (int)(no - (size_t)((nev > 0) ? 1 : 0)));
	free(out);
	free(pairs);
	return 0;
}
