/*
    dro_loop2.c — find the musical loop point in a DRO capture by matching
    key-on (note-trigger) events instead of every register write.

    On OPL2/OPL3, registers 0xB0..0xB8 (and the mirror 0x1B0..0x1B8 on
    the second OPL3 port) carry the key-on bit (0x20). Note triggers form
    the melody backbone and repeat exactly across musical loops, even when
    the engine's volume/timbre tweaks don't.

    Output: a truncated DRO containing init + exactly one musical loop.
*/
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	uint16_t reg;
	uint8_t  val;
	uint32_t delay_after;
} ev_t;

/*  "Note-trigger fingerprint": (channel-in-0..17, key-state, block, fnum-hi).
    Only emitted when bit 0x20 changes. */
typedef struct {
	size_t   ev_idx;       /* index into ev[] of this trigger             */
	uint32_t t_ms;         /* time of trigger                             */
	uint8_t  ch;           /* 0..17                                       */
	uint8_t  key;          /* 0 or 1                                      */
	uint8_t  blk_fn_hi;    /* low 5 bits of the 0xB0+ register value      */
} trig_t;

static int
read_u32(FILE* f, uint32_t* o)
{
	uint8_t b[4];

	if (fread(b, 1, 4, f) != 4)
		return -1;

	*o = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
	return 0;
}
static void
write_u32(FILE* f, uint32_t v)
{
	uint8_t b[4] = {v, v >> 8, v >> 16, v >> 24};
	fwrite(b, 1, 4, f);
}

int
main(int argc, char** argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s in.dro out.dro\n", argv[0]);
		return 1;
	}

	FILE* f = fopen(argv[1], "rb");

	if (!f) {
		perror(argv[1]);
		return 1;
	}

	char m[8];
	uint32_t v, lp, lm;
	uint8_t hdr[5], cmLen, cm[256] = {0};

	if (fread(m, 1, 8, f) != 8 || memcmp(m, "DBRAWOPL", 8) ||
		read_u32(f, &v) || read_u32(f, &lp) || read_u32(f, &lm) || v != 2) {
		fprintf(stderr, "bad DRO\n");
		return 2;
	}

	fread(hdr, 1, 5, f);
	uint8_t hwType = hdr[0], fmt = hdr[1], comp = hdr[2], sc = hdr[3], lc = hdr[4];
	fread(&cmLen, 1, 1, f);
	fread(cm, 1, cmLen, f);
	uint8_t* raw = malloc(lp * 2);
	fread(raw, 1, lp * 2, f);
	fclose(f);

	/* decode events */
	ev_t* ev = malloc(sizeof(ev_t) * lp);
	size_t nev = 0;

	for (uint32_t i = 0; i < lp; i++) {
		uint8_t c = raw[i * 2], val = raw[i * 2 + 1];

		if (c == sc) {
			if (nev)
				ev[nev - 1].delay_after += val + 1;

		} else if (c == lc) {
			if (nev)
				ev[nev - 1].delay_after += (val + 1u) * 256u;

		} else {
			uint8_t idx = c & 0x7F;

			if (idx >= cmLen)
				continue;

			ev[nev].reg = cm[idx] | (c & 0x80 ? 0x100 : 0);
			ev[nev].val = val;
			ev[nev].delay_after = 0;
			nev++;
		}
	}

	/*  Walk events: track previous key-state per channel; emit a trig
	    whenever key-state CHANGES. (key-on alone, or release-then-retrigger.) */
	uint8_t  prev_key[18] = {0};
	trig_t* tr = malloc(sizeof(trig_t) * nev);
	size_t  nt = 0;
	uint32_t t = 0;

	for (size_t i = 0; i < nev; i++) {
		uint16_t r = ev[i].reg;
		uint8_t  v8 = ev[i].val;

		if ((r & 0xF0) == 0xB0 && (r & 0x0F) <= 8) {
			uint8_t ch = (r & 0x0F) + (r & 0x100 ? 9 : 0);
			uint8_t key = (v8 & 0x20) ? 1 : 0;

			if (key != prev_key[ch]) {
				tr[nt].ev_idx = i;
				tr[nt].t_ms = t;
				tr[nt].ch = ch;
				tr[nt].key = key;
				tr[nt].blk_fn_hi = v8 & 0x1F;
				nt++;
				prev_key[ch] = key;
			}
		}

		t += ev[i].delay_after;
	}

	printf("Decoded %zu writes (%u ms), %zu key-state changes.\n",
		   nev, lm, nt);

	/*  Find smallest period P (in trigger count) such that triggers from
	    some offset S onward are periodic-by-P. We compare (ch,key,blk_fn_hi);
	    timings can drift so we don't compare them. */
	static const size_t   MIN_P_TR  = 8;     /* musical loop has many notes */
	static const uint32_t MIN_P_MS  = 3000;
	static const int      MIN_REPS  = 2;

	size_t best_S = 0, best_P = 0;
	uint32_t best_P_ms = 0;

	for (size_t P = MIN_P_TR; P * MIN_REPS <= nt; P++) {
		size_t S = 0;

		for (ssize_t i = (ssize_t)nt - (ssize_t)P - 1; i >= 0; i--) {
			const trig_t* a = &tr[i], *b = &tr[i + P];

			if (a->ch != b->ch || a->key != b->key ||
				a->blk_fn_hi != b->blk_fn_hi) {
				S = (size_t)i + 1;
				break;
			}
		}

		if (nt - S < (size_t)MIN_REPS * P)
			continue;

		uint32_t pms = tr[S + P].t_ms - tr[S].t_ms;

		if (pms < MIN_P_MS)
			continue;

		best_S = S;
		best_P = P;
		best_P_ms = pms;
		break;
	}

	if (!best_P) {
		fprintf(stderr, "No musical loop detected. Aborting.\n");
		return 3;
	}

	/* Translate trigger indices to event indices. */
	size_t ev_loop_start = tr[best_S].ev_idx;
	size_t ev_loop_end   = tr[best_S + best_P].ev_idx;
	uint32_t loops_captured_x10 = (uint32_t)((nt - best_S) * 10 / best_P);
	printf("Loop: starts at event %zu (~%u ms), period = %zu triggers (~%u ms),\n"
		   "      ~%u.%u repetitions captured.\n",
		   ev_loop_start, tr[best_S].t_ms, best_P, best_P_ms,
		   loops_captured_x10 / 10, loops_captured_x10 % 10);

	size_t out_n = ev_loop_end;        /* keep init + exactly one loop */

	/* Drop trailing delay so loop joins seamlessly when player repeats. */
	uint32_t out_ms = 0;

	for (size_t i = 0; i < out_n; i++)
		out_ms += ev[i].delay_after;

	if (out_n > 0) {
		out_ms -= ev[out_n - 1].delay_after;
		ev[out_n - 1].delay_after = 0;
	}

	/* re-emit */
	int8_t reg2code[512];
	memset(reg2code, -1, sizeof(reg2code));

	for (uint8_t i = 0; i < cmLen; i++)
		reg2code[cm[i]] = (int8_t)i;

	uint8_t* out = malloc(nev * 8 + 16);
	size_t op = 0;

	for (size_t i = 0; i < out_n; i++) {
		uint16_t r = ev[i].reg;
		int8_t code = reg2code[r & 0xFF];   /* low 9 bits indexed; high bit via 0x80 */

		if ((r & 0x100) && code < 0)
			code = reg2code[r & 0xFF]; /* same map */

		if (code < 0)
			fprintf(stderr, "warn: reg 0x%03x missing in codemap\n", r);

		else {
			out[op++] = (uint8_t)code | (r & 0x100 ? 0x80 : 0);
			out[op++] = ev[i].val;
		}

		uint32_t d = ev[i].delay_after;

		while (d) {
			if (d >= 256) {
				uint32_t u = d / 256;

				if (u > 256)
					u = 256;

				out[op++] = lc;
				out[op++] = (uint8_t)(u - 1);
				d -= u * 256;

			} else {
				out[op++] = sc;
				out[op++] = (uint8_t)(d - 1);
				d = 0;
			}
		}
	}

	uint32_t out_pairs = op / 2;

	FILE* g = fopen(argv[2], "wb");

	if (!g) {
		perror(argv[2]);
		return 4;
	}

	fwrite("DBRAWOPL", 1, 8, g);
	write_u32(g, 2);
	write_u32(g, out_pairs);
	write_u32(g, out_ms);
	uint8_t h2[5] = {hwType, fmt, comp, sc, lc};
	fwrite(h2, 1, 5, g);
	fwrite(&cmLen, 1, 1, g);
	fwrite(cm, 1, cmLen, g);
	fwrite(out, 1, op, g);
	fclose(g);

	printf("Wrote %s: %u pairs, %u ms.\n", argv[2], out_pairs, out_ms);
	free(out);
	free(tr);
	free(ev);
	free(raw);
	return 0;
}
