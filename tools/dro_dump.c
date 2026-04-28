/*
    dro_dump.c — quick diagnostic: dump DRO event timeline.
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

	*o = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
	return 0;
}

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s file.dro [start_ev] [count]\n", argv[0]);
		return 1;
	}

	FILE* f = fopen(argv[1], "rb");
	char m[8];
	uint32_t v, lp, lm;
	uint8_t hdr[5], cmLen, cm[256] = {0};
	fread(m, 1, 8, f);
	read_u32(f, &v);
	read_u32(f, &lp);
	read_u32(f, &lm);
	fread(hdr, 1, 5, f);
	uint8_t sc = hdr[3], lc = hdr[4];
	fread(&cmLen, 1, 1, f);
	fread(cm, 1, cmLen, f);
	uint8_t* raw = malloc(lp * 2);
	fread(raw, 1, lp * 2, f);
	fclose(f);

	/* decode */
	typedef struct {
		uint16_t reg;
		uint8_t val;
		uint32_t t_ms;
	} E;
	E* ev = malloc(sizeof(E) * lp);
	size_t n = 0;
	uint32_t t = 0;

	for (uint32_t i = 0; i < lp; i++) {
		uint8_t c = raw[i * 2], val = raw[i * 2 + 1];

		if (c == sc)
			t += val + 1;

		else if (c == lc)
			t += (val + 1u) * 256u;

		else {
			ev[n].reg = cm[c & 0x7F] | (c & 0x80 ? 0x100 : 0);
			ev[n].val = val;
			ev[n].t_ms = t;
			n++;
		}
	}

	printf("Total %zu writes, %u ms\n", n, lm);

	/* histogram of writes per second */
	printf("\nWrites per second:\n");
	uint32_t bucket[200] = {0};

	for (size_t i = 0; i < n; i++) {
		uint32_t s = ev[i].t_ms / 1000;

		if (s < 200)
			bucket[s]++;
	}

	for (uint32_t s = 0; s < lm / 1000 + 1 && s < 200; s++)
		printf("  %3u s: %5u\n", s, bucket[s]);

	/* find longest identical (reg,val) substring between halves */
	size_t mid = n / 2;
	/* count of common (reg,val) pairs starting at every position vs. every other? too slow */
	/* instead: try each candidate period in events (pure (reg,val), no delay) */
	printf("\nSearching pure (reg,val) periodicity (ignoring delays)...\n");

	for (size_t P = 100; P * 2 <= n; P++) {
		size_t S = 0;

		for (ssize_t i = (ssize_t)n - P - 1; i >= 0; i--) {
			if (ev[i].reg != ev[i + P].reg || ev[i].val != ev[i + P].val) {
				S = i + 1;
				break;
			}
		}

		if (n - S >= 2 * P && n - S >= 1000) {
			uint32_t period_ms = ev[S + P - 1].t_ms - ev[S].t_ms;

			if (period_ms > 2000) {
				printf("  P=%zu events, S=%zu, periodic_len=%zu, ~%u ms/loop, ~%.1f loops\n",
					   P, S, n - S, period_ms, (double)(n - S) / P);

				if (P > 500)
					break; /* show a few small ones then stop */
			}
		}
	}

	free(ev);
	free(raw);
	return 0;
}
