/*
    dro_pack.c -- compress a DRO (or any) file with heatshrink.

    Heatshrink has no on-disk format -- the (window, lookahead)
    parameters must be agreed out-of-band between encoder and
    decoder.  We encode the window-bits in the output extension:

        in.dro  ->  in.dro_hs11      (window=11, lookahead=4)

    Lookahead is fixed at 4 by default since increasing it past 4
    rarely helps on event-stream data and shrinks the maximum match
    length usefully encodable.

    Usage:
        dro_pack in.dro out.dro_hsN [--window=N] [--lookahead=L]

    If --window is omitted, it is parsed from the output extension
    (after "_hs"), defaulting to 11 if no number is present.
*/
#define HEATSHRINK_DYNAMIC_ALLOC 1
#include "../heatshrink/heatshrink_encoder.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t
encode(const uint8_t* in, size_t n, int wbits, int lbits,
	   uint8_t* out, size_t outcap)
{
	heatshrink_encoder* enc =
		heatshrink_encoder_alloc((uint8_t)wbits, (uint8_t)lbits);

	if (!enc)
		return 0;

	size_t consumed = 0, produced = 0;

	for (;;) {
		if (consumed < n) {
			size_t got = 0;
			heatshrink_encoder_sink(enc, (uint8_t*)in + consumed,
									n - consumed, &got);
			consumed += got;

		} else {
			HSE_finish_res fr = heatshrink_encoder_finish(enc);

			if (fr == HSER_FINISH_DONE)
				break;
		}

		HSE_poll_res pr;

		do {
			size_t got = 0;
			pr = heatshrink_encoder_poll(enc, out + produced,
										 outcap - produced, &got);
			produced += got;
		} while (pr == HSER_POLL_MORE);
	}

	heatshrink_encoder_free(enc);
	return produced;
}

/* --------------------------------------------------------------- */
/*  Parse "...dro_hs<W>[l<L>]" tail.  Returns 0 on match.          */
/* --------------------------------------------------------------- */
static int
parse_w_l_from_ext(const char* path, int* wbits, int* lbits)
{
	const char* p = strstr(path, "_hs");

	if (!p)
		return -1;

	p += 3;

	int w = 0, any = 0;

	while (*p >= '0' && *p <= '9') {
		w = w * 10 + (*p - '0');
		p++;
		any = 1;
	}

	if (!any)
		return -1;

	int l = 4;

	if (*p == 'l' || *p == 'L') {
		p++;
		int la = 0, lany = 0;

		while (*p >= '0' && *p <= '9') {
			la = la * 10 + (*p - '0');
			p++;
			lany = 1;
		}

		if (!lany)
			return -1;

		l = la;
	}

	*wbits = w;
	*lbits = l;
	return 0;
}

int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr,
				"usage: %s in out.dro_hsN [--window=N] [--lookahead=L]\n",
				argv[0]);
		return 1;
	}

	const char* in_path  = argv[1];
	const char* out_path = argv[2];
	int wbits = -1;
	int lbits = -1;

	for (int i = 3; i < argc; i++) {
		if (strncmp(argv[i], "--window=", 9) == 0)
			wbits = atoi(argv[i] + 9);

		else if (strncmp(argv[i], "--lookahead=", 12) == 0)
			lbits = atoi(argv[i] + 12);

		else {
			fprintf(stderr, "dro_pack: unknown arg '%s'\n", argv[i]);
			return 1;
		}
	}

	{
		int ew, el;

		if (parse_w_l_from_ext(out_path, &ew, &el) == 0) {
			if (wbits < 0)
				wbits = ew;

			if (lbits < 0)
				lbits = el;
		}
	}

	if (wbits < 0)
		wbits = 11;

	if (lbits < 0)
		lbits = 4;

	if (wbits < 4 || wbits > 15) {
		fprintf(stderr, "dro_pack: window must be 4..15 (got %d)\n", wbits);
		return 1;
	}

	if (lbits < 3 || lbits >= wbits) {
		fprintf(stderr, "dro_pack: lookahead must be 3..%d (got %d)\n",
				wbits - 1, lbits);
		return 1;
	}

	FILE* f = fopen(in_path, "rb");

	if (!f) {
		perror(in_path);
		return 2;
	}

	fseek(f, 0, SEEK_END);
	size_t n = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);

	uint8_t* in = malloc(n);

	if (fread(in, 1, n, f) != n) {
		perror("read");
		return 2;
	}

	fclose(f);

	size_t   cap = n + 1024;
	uint8_t* out = malloc(cap);

	size_t produced = encode(in, n, wbits, lbits, out, cap);

	if (produced == 0 || produced >= cap) {
		fprintf(stderr, "dro_pack: encode failed\n");
		return 3;
	}

	FILE* g = fopen(out_path, "wb");

	if (!g) {
		perror(out_path);
		return 4;
	}

	fwrite(out, 1, produced, g);
	fclose(g);

	printf("%-58s  %7zu -> %7zu  (%.1f%%)  w=%d l=%d\n",
		   in_path, n, produced, 100.0 * produced / n, wbits, lbits);

	free(in);
	free(out);
	return 0;
}
