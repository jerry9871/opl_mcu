/*  dro_loopfind: detect where tail events repeat the start-of-loop region.

    Decodes a DRO v2 file into a flat list of (t_ms, reg, val) writes
    (delay opcodes are folded into t_ms). Then, given a "loop_start_ms"
    argument (after which the main pattern begins), it slides a window
    of the last ~N events across the post-intro region and finds the
    (reg,val) pair sequence with the best match. The match position
    gives the exact loop boundary at the tail end.

    Usage: dro_loopfind file.dro loop_start_ms [search_window_events]
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
	uint32_t t;
	uint8_t reg;
	uint8_t val;
} Ev;

int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s file.dro loop_start_ms [tail_window=64]\n", argv[0]);
		return 1;
	}

	FILE* f = fopen(argv[1], "rb");

	if (!f) {
		perror(argv[1]);
		return 1;
	}

	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* b = malloc(sz);
	fread(b, 1, sz, f);
	fclose(f);

	if (memcmp(b, "DBRAWOPL", 8)) {
		fprintf(stderr, "not DRO\n");
		return 1;
	}

	uint32_t ver = b[8] | (b[9] << 8) | (b[10] << 16) | (b[11] << 24);

	if (ver != 2) {
		fprintf(stderr, "need DRO v2 (got %u)\n", ver);
		return 1;
	}

	uint32_t pairs = b[12] | (b[13] << 8) | (b[14] << 16) | (b[15] << 24);
	uint8_t short_d = b[23], long_d = b[24];
	uint8_t cmap_len = b[25];
	uint8_t* cmap = b + 26;
	uint8_t* p    = cmap + cmap_len;

	Ev* ev = malloc(sizeof(Ev) * pairs);
	uint32_t n = 0;
	uint64_t t = 0;

	for (uint32_t i = 0; i < pairs; i++) {
		uint8_t reg = p[2 * i], val = p[2 * i + 1];

		if (reg == short_d) {
			t += val + 1;
			continue;
		}

		if (reg == long_d) {
			t += (val + 1u) * 256u;
			continue;
		}

		uint8_t bank = reg >> 7;
		uint8_t idx  = reg & 0x7F;
		uint16_t r = cmap[idx] | (bank ? 0x100 : 0);
		ev[n].t = (uint32_t)t;
		ev[n].reg = (uint8_t)(r & 0xFF) | (bank ? 0x80 : 0); /* fold bank into top bit for compare */
		ev[n].val = val;
		n++;
	}

	uint32_t loop_start_ms = (uint32_t)atoi(argv[2]);
	uint32_t W = (argc >= 4) ? (uint32_t)atoi(argv[3]) : 64;
	int      have_end_hint = (argc >= 5);
	uint32_t end_hint_ms = have_end_hint ? (uint32_t)atoi(argv[4]) : 0;
	uint32_t end_window_ms = (argc >= 6) ? (uint32_t)atoi(argv[5]) : 1500;

	/* find first event index >= loop_start_ms */
	uint32_t loop0 = 0;

	while (loop0 < n && ev[loop0].t < loop_start_ms)
		loop0++;

	if (loop0 + W >= n) {
		fprintf(stderr, "too few events after loop_start\n");
		return 1;
	}

	printf("decoded %u events; loop region starts at idx %u (t=%u ms)\n",
		   n, loop0, ev[loop0].t);

	if (have_end_hint) {
		/*  Search for the cut event index c such that the K events starting at c
		    best match the K events starting at loop0. We score by the longest
		    matching prefix length (capped at W). The candidate c is restricted
		    to events whose timestamp is within +/- end_window_ms of end_hint_ms. */
		uint32_t lo_t = (end_hint_ms > end_window_ms) ? (end_hint_ms - end_window_ms) : 0;
		uint32_t hi_t = end_hint_ms + end_window_ms;
		uint32_t best_score = 0;
		uint32_t best_c = 0;

		printf("end-hint mode: searching cut events in t = [%u .. %u] ms (+/- %u ms around %u)\n",
			   lo_t, hi_t, end_window_ms, end_hint_ms);

		for (uint32_t c = loop0 + 1; c < n; c++) {
			if (ev[c].t < lo_t)
				continue;

			if (ev[c].t > hi_t)
				break;

			uint32_t maxk = W;

			if (n - c < maxk)
				maxk = n - c;

			uint32_t k = 0;

			while (k < maxk &&
				   ev[loop0 + k].reg == ev[c + k].reg &&
				   ev[loop0 + k].val == ev[c + k].val)
				k++;

			if (k > best_score) {
				best_score = k;
				best_c = c;
			}
		}

		if (best_score < 4) {
			printf("No strong cut found (best prefix match = %u events).\n", best_score);
			return 2;
		}

		uint32_t cut_t  = ev[best_c].t;
		uint32_t period = cut_t - ev[loop0].t;
		printf("\nBest cut: event idx %u, t=%u ms\n", best_c, cut_t);
		printf("  matched %u consecutive events against start of loop\n", best_score);
		printf("  -> recommended slice: %u .. %u  (loop period %u ms)\n",
			   ev[loop0].t, cut_t, period);
		printf("\nFirst 4 matched events at the cut:\n");

		for (uint32_t j = 0; j < 4 && j < best_score; j++) {
			printf("  cut[%u] t=%u reg=0x%02X val=0x%02X   ==  start[%u] t=%u reg=0x%02X val=0x%02X\n",
				   best_c + j, ev[best_c + j].t, ev[best_c + j].reg, ev[best_c + j].val,
				   loop0 + j,  ev[loop0 + j].t,  ev[loop0 + j].reg,  ev[loop0 + j].val);
		}

		free(ev);
		free(b);
		return 0;
	}

	/*  Try every match length k from W down to 4, sliding the tail-of-length-k
	    window across event positions [loop0 .. n-2k]. First match wins (largest k). */
	uint32_t best_k = 0, best_i = 0;

	for (uint32_t k = W; k >= 4; k--) {
		if (loop0 + k > n - k)
			continue;

		for (uint32_t i = loop0; i + k <= n - k; i++) {
			int ok = 1;

			for (uint32_t j = 0; j < k; j++) {
				if (ev[i + j].reg != ev[n - k + j].reg ||
					ev[i + j].val != ev[n - k + j].val) {
					ok = 0;
					break;
				}
			}

			if (ok) {
				best_k = k;
				best_i = i;
				break;
			}
		}

		if (best_k)
			break;
	}

	if (best_k < 4) {
		printf("No good tail/loop match (best_k=%u). End may not duplicate any earlier region.\n", best_k);
		return 2;
	}

	uint32_t cut_idx = n - best_k;
	uint32_t cut_t = ev[cut_idx].t;
	uint32_t loop_t = ev[best_i].t;
	printf("\nBest match: %u trailing events == events starting at idx %u (t=%u ms).\n",
		   best_k, best_i, loop_t);
	printf("  first repeated event in tail: idx %u, t=%u ms\n", cut_idx, cut_t);
	printf("  -> recommended slice: %u .. %u  (loop period %u ms)\n",
		   loop_t, cut_t, cut_t - loop_t);
	printf("\nFirst 4 matched events:\n");

	for (uint32_t j = 0; j < 4 && j < best_k; j++) {
		printf("  tail[%u] t=%u reg=0x%02X val=0x%02X   ==  src[%u] t=%u reg=0x%02X val=0x%02X\n",
			   cut_idx + j, ev[cut_idx + j].t, ev[cut_idx + j].reg, ev[cut_idx + j].val,
			   best_i + j,  ev[best_i + j].t,  ev[best_i + j].reg,  ev[best_i + j].val);
	}

	free(ev);
	free(b);
	return 0;
}
