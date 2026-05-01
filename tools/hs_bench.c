/*
    hs_bench.c -- measure heatshrink encoded size of an arbitrary file
    at a few (window, lookahead) combos.  Used to decide whether to
    ship a heatshrink decoder on the MCU.
*/
#define HEATSHRINK_DYNAMIC_ALLOC 1
#include "../heatshrink/heatshrink_encoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t
encode(const uint8_t* in, size_t n, int wbits, int lbits, uint8_t* out, size_t outcap)
{
	heatshrink_encoder* enc = heatshrink_encoder_alloc((uint8_t)wbits, (uint8_t)lbits);

	if (!enc)
		return 0;

	size_t consumed = 0, produced = 0;

	for (;;) {
		if (consumed < n) {
			size_t got = 0;
			heatshrink_encoder_sink(enc, (uint8_t*)in + consumed, n - consumed, &got);
			consumed += got;

		} else {
			HSE_finish_res fr = heatshrink_encoder_finish(enc);

			if (fr == HSER_FINISH_DONE)
				break;
		}

		HSE_poll_res pr;

		do {
			size_t got = 0;
			pr = heatshrink_encoder_poll(enc, out + produced, outcap - produced, &got);
			produced += got;
		} while (pr == HSER_POLL_MORE);
	}

	heatshrink_encoder_free(enc);
	return produced;
}

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s file [file...]\n", argv[0]);
		return 1;
	}

	printf("%-58s %8s %8s %8s %8s %8s %8s\n",
		   "file", "raw", "w8/l4", "w10/l4", "w11/l4", "w12/l4", "w13/l5");

	size_t cap = 16 * 1024 * 1024;
	uint8_t* out = malloc(cap);

	uint64_t tot_raw = 0, tot[5] = {0};

	for (int i = 1; i < argc; i++) {
		FILE* f = fopen(argv[i], "rb");

		if (!f) {
			perror(argv[i]);
			continue;
		}

		fseek(f, 0, SEEK_END);
		size_t n = (size_t)ftell(f);
		fseek(f, 0, SEEK_SET);

		uint8_t* in = malloc(n);
		fread(in, 1, n, f);
		fclose(f);

		struct {
			int w, l;
		} cfg[] = {{8, 4}, {10, 4}, {11, 4}, {12, 4}, {13, 5}};
		size_t sz[5];

		for (int k = 0; k < 5; k++) {
			sz[k] = encode(in, n, cfg[k].w, cfg[k].l, out, cap);
			tot[k] += sz[k];
		}

		tot_raw += n;

		printf("%-58s %8zu %8zu %8zu %8zu %8zu %8zu\n",
			   argv[i], n, sz[0], sz[1], sz[2], sz[3], sz[4]);

		free(in);
	}

	printf("%-58s %8llu %8llu %8llu %8llu %8llu %8llu\n",
		   "TOTAL", (unsigned long long)tot_raw,
		   (unsigned long long)tot[0], (unsigned long long)tot[1],
		   (unsigned long long)tot[2], (unsigned long long)tot[3],
		   (unsigned long long)tot[4]);

	free(out);
	return 0;
}
