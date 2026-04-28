/*
    dro_loop.c — detect the loop point in a DOSBox DRO v2 capture and
    write a truncated copy that contains the one-time init plus exactly
    one musical loop.

    Usage:
       dro_loop input.dro output.dro

    Standalone offline tool, not part of the MCU build.
*/
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	uint16_t reg;          /* OPL register, bit 9 = 2nd port            */
	uint8_t  val;
	uint32_t delay_after;  /* milliseconds of silence/render after this */
} ev_t;

/* ---- DRO v2 reader ------------------------------------------------- */
static int
read_u32(FILE* f, uint32_t* o)
{
	uint8_t b[4];

	if (fread(b, 1, 4, f) != 4)
		return -1;

	*o = (uint32_t)b[0] | ((uint32_t)b[1] << 8)
		 | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	return 0;
}
static void
write_u32(FILE* f, uint32_t v)
{
	uint8_t b[4] = { v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF };
	fwrite(b, 1, 4, f);
}

int
main(int argc, char** argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s input.dro output.dro\n", argv[0]);
		return 1;
	}

	FILE* f = fopen(argv[1], "rb");

	if (!f) {
		perror(argv[1]);
		return 1;
	}

	char magic[8];
	uint32_t ver, lenPairs, lenMs;

	if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "DBRAWOPL", 8) ||
		read_u32(f, &ver) || read_u32(f, &lenPairs) || read_u32(f, &lenMs) ||
		ver != 2) {
		fprintf(stderr, "%s: not a DRO v2 file\n", argv[1]);
		fclose(f);
		return 2;
	}

	uint8_t hdr[5];

	if (fread(hdr, 1, 5, f) != 5) {
		fclose(f);
		return 3;
	}

	uint8_t hwType    = hdr[0];
	uint8_t format    = hdr[1];
	uint8_t compress  = hdr[2];
	uint8_t shortCode = hdr[3];
	uint8_t longCode  = hdr[4];

	uint8_t cmLen;

	if (fread(&cmLen, 1, 1, f) != 1) {
		fclose(f);
		return 4;
	}

	uint8_t codemap[256] = {0};

	if (cmLen && fread(codemap, 1, cmLen, f) != cmLen) {
		fclose(f);
		return 5;
	}

	uint8_t* raw = malloc((size_t)lenPairs * 2);

	if (!raw) {
		fclose(f);
		return 6;
	}

	if (fread(raw, 1, (size_t)lenPairs * 2, f) != (size_t)lenPairs * 2) {
		fprintf(stderr, "%s: short read\n", argv[1]);
		free(raw);
		fclose(f);
		return 7;
	}

	fclose(f);

	/* ---- decode into ev_t[] ---- */
	ev_t* ev = malloc(sizeof(ev_t) * lenPairs);

	if (!ev) {
		free(raw);
		return 8;
	}

	size_t nev = 0;

	for (uint32_t i = 0; i < lenPairs; i++) {
		uint8_t code = raw[i * 2], val = raw[i * 2 + 1];

		if (code == shortCode) {
			if (nev > 0)
				ev[nev - 1].delay_after += (uint32_t)val + 1;

			/* else: leading delay before any write — ignore */

		} else if (code == longCode) {
			if (nev > 0)
				ev[nev - 1].delay_after += ((uint32_t)val + 1) * 256;

		} else {
			uint8_t idx = code & 0x7F;

			if (idx >= cmLen)
				continue;

			ev[nev].reg = codemap[idx] | ((code & 0x80) ? 0x100 : 0);
			ev[nev].val = val;
			ev[nev].delay_after = 0;
			nev++;
		}
	}

	printf("Decoded %zu register-write events (%u ms total).\n", nev, lenMs);

	/*  ---- find loop period --------------------------------------------
	    Strategy: try periods P from small to large. For a tentative P, the
	    stream is periodic with period P from index S onward iff
	        ev[i] == ev[i+P]    for all S <= i < nev-P
	    We pick the smallest P for which we can find such an S, AND for
	    which the periodic region is "convincingly long" (>= MIN_REPEAT*P
	    events and >= MIN_REPEAT_MS milliseconds, with at least 2 full
	    periods present).
	*/
	static const size_t   MIN_PERIOD_EV = 50;     /* ignore tiny "loops" */
	static const uint32_t MIN_PERIOD_MS = 2000;   /* musical loops are >= 2 s */
	static const int      MIN_REPEAT     = 2;     /* need >= 2 copies */

	size_t   best_S = 0, best_P = 0;
	uint32_t best_P_ms = 0;

	for (size_t P = MIN_PERIOD_EV; P * MIN_REPEAT <= nev; P++) {
		/*  Find the largest S such that ev[i]==ev[i+P] for all i in [S, nev-P).
		    That's: walk from i = nev-P-1 down, find first mismatch; S is one
		    past it. */
		size_t S = 0;

		for (ssize_t i = (ssize_t)nev - (ssize_t)P - 1; i >= 0; i--) {
			const ev_t* a = &ev[i], *b = &ev[i + P];

			if (a->reg != b->reg || a->val != b->val ||
				a->delay_after != b->delay_after) {
				S = (size_t)i + 1;
				break;
			}
		}

		size_t periodic_len = nev - S;     /* events from S to end */

		if (periodic_len < (size_t)MIN_REPEAT * P)
			continue;

		/* Compute period in ms (sum of one P-window of delays). */
		uint32_t P_ms = 0;

		for (size_t i = S; i < S + P; i++)
			P_ms += ev[i].delay_after;

		if (P_ms < MIN_PERIOD_MS)
			continue;

		best_S = S;
		best_P = P;
		best_P_ms = P_ms;
		break;       /* smallest P wins */
	}

	if (!best_P) {
		fprintf(stderr, "No convincing loop found. Writing input as-is.\n");
		best_S = 0;
		best_P = nev;

	} else {
		printf("Loop found: init = %zu events, loop = %zu events (~%u ms),\n"
			   "             %u repetitions captured.\n",
			   best_S, best_P, best_P_ms,
			   (unsigned)((nev - best_S) / best_P));
	}

	size_t out_n = best_S + best_P;     /* keep init + one full loop */

	if (out_n > nev)
		out_n = nev;

	/* Compute total ms of the truncated stream. */
	uint32_t out_ms = 0;

	for (size_t i = 0; i < out_n; i++)
		out_ms += ev[i].delay_after;

	/* Drop the trailing delay on the last event so the loop joins cleanly. */
	if (out_n > 0) {
		out_ms -= ev[out_n - 1].delay_after;
		ev[out_n - 1].delay_after = 0;
	}

	/*  ---- re-emit as DRO v2 -------------------------------------------
	    We reuse the original codemap. To find the code for a register R we
	    search the codemap; bit 0x80 is set iff R has bit 0x100 set.
	*/
	int8_t reg2code[512];
	memset(reg2code, -1, sizeof(reg2code));

	for (uint8_t i = 0; i < cmLen; i++)
		reg2code[codemap[i]] = (int8_t)i;

	/* Buffer the output pair-stream so we know lenPairs ahead of header. */
	size_t cap = nev * 4 + 16;
	uint8_t* out = malloc(cap);

	if (!out) {
		free(ev);
		free(raw);
		return 9;
	}

	size_t op = 0;

	for (size_t i = 0; i < out_n; i++) {
		uint16_t reg = ev[i].reg;
		int8_t code = reg2code[reg & 0x1FF];

		if (code < 0) {
			fprintf(stderr, "warn: register 0x%03x not in codemap, dropped\n",
					reg);

		} else {
			uint8_t c = (uint8_t)code | (reg & 0x100 ? 0x80 : 0);
			out[op++] = c;
			out[op++] = ev[i].val;
		}

		uint32_t d = ev[i].delay_after;

		while (d) {
			if (d >= 256) {
				uint32_t units = d / 256;       /* (val+1)*256 ms each   */

				if (units > 256)
					units = 256;

				out[op++] = longCode;
				out[op++] = (uint8_t)(units - 1);
				d -= units * 256;

			} else {
				uint32_t units = d;             /* (val+1) ms each       */

				if (units > 256)
					units = 256;

				out[op++] = shortCode;
				out[op++] = (uint8_t)(units - 1);
				d -= units;
			}
		}
	}

	uint32_t out_pairs = (uint32_t)(op / 2);

	/* ---- write the new file ---- */
	FILE* g = fopen(argv[2], "wb");

	if (!g) {
		perror(argv[2]);
		free(out);
		free(ev);
		free(raw);
		return 10;
	}

	fwrite("DBRAWOPL", 1, 8, g);
	write_u32(g, 2);
	write_u32(g, out_pairs);
	write_u32(g, out_ms);
	uint8_t hdr2[5] = { hwType, format, compress, shortCode, longCode };
	fwrite(hdr2, 1, 5, g);
	fwrite(&cmLen, 1, 1, g);
	fwrite(codemap, 1, cmLen, g);
	fwrite(out, 1, op, g);
	fclose(g);

	printf("Wrote %s: %u pairs, %u ms.\n", argv[2], out_pairs, out_ms);

	free(out);
	free(ev);
	free(raw);
	return 0;
}
