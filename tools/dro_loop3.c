/*
    dro_loop3.c — find the loop point in a DRO using "section markers":
    channels with very few key-ons (often used to mark song-section
    boundaries). The loop length is the spacing between markers; the
    loop start is the time of the LAST same-position marker.
*/
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	uint16_t reg;
	uint8_t val;
	uint32_t delay_after;
} ev_t;

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
	fread(m, 1, 8, f);
	read_u32(f, &v);
	read_u32(f, &lp);
	read_u32(f, &lm);
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

	/* For each channel collect key-on edge timestamps and event indices. */
	typedef struct {
		uint32_t t_ms;
		size_t ev_idx;
	} konr_t;
	konr_t* kons[18];
	size_t  nkons[18] = {0};

	for (int i = 0; i < 18; i++)
		kons[i] = malloc(sizeof(konr_t) * nev);

	uint8_t prev_key[18] = {0};
	uint32_t t = 0;

	for (size_t i = 0; i < nev; i++) {
		uint16_t r = ev[i].reg;
		uint8_t val = ev[i].val;

		if ((r & 0xF0) == 0xB0 && (r & 0x0F) <= 8) {
			uint8_t ch = (r & 0x0F) + (r & 0x100 ? 9 : 0);
			uint8_t key = (val & 0x20) ? 1 : 0;

			if (key && !prev_key[ch]) {
				kons[ch][nkons[ch]].t_ms = t;
				kons[ch][nkons[ch]].ev_idx = i;
				nkons[ch]++;
			}

			prev_key[ch] = key;
		}

		t += ev[i].delay_after;
	}

	/*  Find the channel with the SMALLEST positive count (most likely a
	    section-boundary marker). Also require >=3 hits so we can measure
	    spacing and confirm it. */
	int marker_ch = -1;
	size_t marker_n = (size_t) -1;

	for (int i = 0; i < 18; i++) {
		if (nkons[i] >= 3 && nkons[i] < marker_n) {
			marker_n = nkons[i];
			marker_ch = i;
		}
	}

	if (marker_ch < 0) {
		fprintf(stderr, "No marker channel found.\n");
		return 2;
	}

	printf("Marker channel: ch%d  (%zu key-ons)\n", marker_ch, marker_n);
	printf("  marker times (ms): ");

	for (size_t i = 0; i < marker_n; i++)
		printf("%u ", kons[marker_ch][i].t_ms);

	printf("\n");

	/* Spacings between consecutive markers: */
	uint32_t spacings[64];
	size_t nsp = 0;

	for (size_t i = 1; i < marker_n && i <= 64; i++)
		spacings[nsp++] = kons[marker_ch][i].t_ms - kons[marker_ch][i - 1].t_ms;

	/* Median spacing = section length */
	uint32_t sp_sorted[64];
	memcpy(sp_sorted, spacings, sizeof(uint32_t)*nsp);

	/* simple insertion sort */
	for (size_t i = 1; i < nsp; i++) {
		uint32_t k = sp_sorted[i];
		size_t j = i;

		while (j && sp_sorted[j - 1] > k) {
			sp_sorted[j] = sp_sorted[j - 1];
			j--;
		}

		sp_sorted[j] = k;
	}

	uint32_t median_section = sp_sorted[nsp / 2];
	printf("  spacings: ");

	for (size_t i = 0; i < nsp; i++)
		printf("%u ", spacings[i]);

	printf("\n  median section ~%u ms\n", median_section);

	/*  The full SONG length is one repetition of the section-marker
	    pattern. Find smallest k such that spacings repeat with period k:
	     spacings[i] ~= spacings[i+k]   (within 250 ms tolerance).
	    Then song length = k * median_section ... but simpler: assume all
	    markers are equally spaced => song is just the span of markers. */
	/*  Heuristic: take loop length as kons[marker_ch][N-1].t_ms (i.e.,
	    the time of the last marker, assuming it is the start of the next
	    iteration). Rationale: capture stops part-way through 2nd iter, so
	    the LAST marker is the one that re-starts the song. */
	size_t   loop_marker = marker_n - 1;
	uint32_t loop_t = kons[marker_ch][loop_marker].t_ms;
	size_t   loop_ev = kons[marker_ch][loop_marker].ev_idx;
	printf("  -> loop start = marker #%zu at t=%u ms, event %zu\n",
		   loop_marker, loop_t, loop_ev);

	/*  Truncate to events [0 .. loop_ev). Drop trailing delay so the loop
	    joins seamlessly when the player wraps. */
	size_t out_n = loop_ev;
	uint32_t out_ms = 0;

	for (size_t i = 0; i < out_n; i++)
		out_ms += ev[i].delay_after;

	if (out_n > 0) {
		out_ms -= ev[out_n - 1].delay_after;
		ev[out_n - 1].delay_after = 0;
	}

	int8_t reg2code[256];
	memset(reg2code, -1, sizeof(reg2code));

	for (uint8_t i = 0; i < cmLen; i++)
		reg2code[cm[i]] = (int8_t)i;

	uint8_t* out = malloc(nev * 8 + 16);
	size_t op = 0;

	for (size_t i = 0; i < out_n; i++) {
		uint16_t r = ev[i].reg;
		int8_t code = reg2code[r & 0xFF];

		if (code < 0)
			fprintf(stderr, "warn: reg 0x%03x not in codemap\n", r);

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
		return 3;
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

	printf("Wrote %s: %u pairs, %u ms (%u s)\n",
		   argv[2], out_pairs, out_ms, out_ms / 1000);
	return 0;
}
