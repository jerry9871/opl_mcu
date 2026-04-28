/*
    dro_segments.c — find probable scene boundaries in a DRO v2 capture.

    Heuristic:
        Decode the DRO into (time_ms, reg, val) events.
        For each event report the gap to the previous event.
        Print the longest gaps (likely scene boundaries / silences).
        Also report any big "kill switch" — many KON-off (regs 0xB0-0xB8
        and 0x1B0-0x1B8 with bit 5 cleared) within a short window.

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

typedef struct {
	uint32_t t_ms;
	uint16_t reg;
	uint8_t val;
} Ev;

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s file.dro [topN]\n", argv[0]);
		return 1;
	}

	int topN = (argc >= 3) ? atoi(argv[2]) : 30;

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
		fprintf(stderr, "not DRO v2\n");
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

	/*  delays in DRO apply *after* the previous event, but for our timeline
	    we want time at which each event fires. Track running clock. */
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

	printf("DRO total length     : %u ms (%.2f s)\n", lm, lm / 1000.0);
	printf("Decoded events       : %zu\n", nev);
	printf("Last event timestamp : %u ms\n\n", nev ? ev[nev - 1].t_ms : 0);

	/* Find largest gaps between consecutive events */
	typedef struct {
		uint32_t gap;
		uint32_t t0;
		uint32_t t1;
		size_t idx;
	} G;
	G* gaps = malloc(sizeof(G) * nev);

	for (size_t i = 1; i < nev; i++) {
		gaps[i].gap = ev[i].t_ms - ev[i - 1].t_ms;
		gaps[i].t0  = ev[i - 1].t_ms;
		gaps[i].t1  = ev[i].t_ms;
		gaps[i].idx = i;
	}

	gaps[0].gap = 0;

	/* simple selection of top-N */
	int n = (int)nev;

	if (topN > n)
		topN = n;

	printf("Top %d gaps between events (likely silences / scene cuts):\n", topN);
	printf("  %-8s %-10s %-10s %-8s\n", "gap_ms", "from_ms", "to_ms", "ev_idx");

	for (int k = 0; k < topN; k++) {
		int best = 0;

		for (int i = 1; i < n; i++)
			if (gaps[i].gap > gaps[best].gap)
				best = i;

		if (gaps[best].gap == 0)
			break;

		printf("  %-8u %-10u %-10u %-8zu\n",
			   gaps[best].gap, gaps[best].t0, gaps[best].t1, gaps[best].idx);
		gaps[best].gap = 0;
	}

	/*  Also: look for "kill bursts" — many key-off events (B0..B8 reg with
	    bit 5 cleared) in a 50ms window. */
	printf("\nKey-off bursts (>= 4 KOFFs within 50 ms):\n");
	printf("  %-10s %-6s\n", "t_ms", "kills");
	size_t i = 0;

	while (i < nev) {
		if ((ev[i].reg & 0xF0) == 0xB0 && (ev[i].reg & 0x0F) <= 8 && !(ev[i].val & 0x20)) {
			size_t j = i;
			int kills = 0;
			uint32_t t0 = ev[i].t_ms;

			while (j < nev && ev[j].t_ms <= t0 + 50) {
				if ((ev[j].reg & 0xF0) == 0xB0 && (ev[j].reg & 0x0F) <= 8 && !(ev[j].val & 0x20))
					kills++;

				j++;
			}

			if (kills >= 4) {
				printf("  %-10u %-6d\n", t0, kills);
				i = j;
				continue;
			}
		}

		i++;
	}

	free(gaps);
	free(ev);
	return 0;
}
