/*
    dro2hdr.c — convert a DOSBox DRO v2 capture into a C header file
    containing  static const opl_song songname  ready to embed in
    microcontroller flash.

    The packed `opl_song` format is byte-compatible with the DRO v2
    payload (codemap + (code,val) opcode stream), so this tool is
    essentially a verbatim dump of the relevant bytes wrapped in C
    syntax.  ~2 bytes per register write versus ~6 bytes for the older
    opl_event[] format.

    Usage:
       dro2hdr input.dro output.h song_symbol_name [--hs[=W[lL]]]

    With --hs the payload is written as a heatshrink-compressed blob
    plus an opl_song_hs descriptor.  The runtime code calls
    dro_song_from_hs() to materialise an opl_song.  Default W=13 L=4
    (~8 KB window, very high compression ratio).

    Offline tool — not part of the MCU build.
*/
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HEATSHRINK_DYNAMIC_ALLOC 1
#include "../heatshrink/heatshrink_encoder.h"

static size_t
hs_encode(const uint8_t* in, size_t n, int wbits, int lbits,
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
main(int argc, char** argv)
{
	if (argc < 4) {
		fprintf(stderr,
				"usage: %s in.dro out.h symbol [--hs[=W[lL]]]\n",
				argv[0]);
		return 1;
	}

	const char* in_path  = argv[1];
	const char* out_path = argv[2];
	const char* sym      = argv[3];

	int use_hs   = 0;
	int hs_w     = 13;
	int hs_l     = 4;

	for (int i = 4; i < argc; i++) {
		if (strncmp(argv[i], "--hs", 4) == 0) {
			use_hs = 1;
			const char* p = argv[i] + 4;

			if (*p == '=') {
				p++;
				int w = 0, any = 0;

				while (*p >= '0' && *p <= '9') {
					w = w * 10 + (*p - '0');
					p++;
					any = 1;
				}

				if (any)
					hs_w = w;

				if (*p == 'l' || *p == 'L') {
					p++;
					int l = 0, lany = 0;

					while (*p >= '0' && *p <= '9') {
						l = l * 10 + (*p - '0');
						p++;
						lany = 1;
					}

					if (lany)
						hs_l = l;
				}
			}

		} else {
			fprintf(stderr, "dro2hdr: unknown arg '%s'\n", argv[i]);
			return 1;
		}
	}

	if (use_hs && (hs_w < 4 || hs_w > 15 || hs_l < 3 || hs_l >= hs_w)) {
		fprintf(stderr, "dro2hdr: bad --hs window/lookahead (w=%d l=%d)\n",
				hs_w, hs_l);
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
		fprintf(stderr, "%s: not a DRO v2 file\n", in_path);
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

	if (!raw || fread(raw, 1, (size_t)lp * 2, f) != (size_t)lp * 2) {
		fprintf(stderr, "%s: short read\n", in_path);
		free(raw);
		fclose(f);
		return 7;
	}

	fclose(f);

	/*  Build the packed payload: codemap bytes followed by the raw
	    (code,val) opcode stream.  This is identical to what
	    seq_player's MODE_PACKED decoder consumes. */
	size_t total = (size_t)cmLen + (size_t)lp * 2;
	uint8_t* out = malloc(total);
	memcpy(out, cm, cmLen);
	memcpy(out + cmLen, raw, (size_t)lp * 2);
	free(raw);

	FILE* g = fopen(out_path, "w");

	if (!g) {
		perror(out_path);
		free(out);
		return 8;
	}

	if (use_hs) {
		/*  Re-emit the original DRO file (header + codemap + stream),
		    then heatshrink-compress the lot.  At runtime the helper
		    dro_song_from_hs() decompresses and re-parses it. */
		size_t   dro_len = 26u + total;
		uint8_t* dro     = malloc(dro_len);

		memcpy(dro, "DBRAWOPL", 8);

		uint32_t v2  = 2u;
		dro[8]  = v2 & 0xFF;
		dro[9]  = (v2 >> 8) & 0xFF;
		dro[10] = (v2 >> 16) & 0xFF;
		dro[11] = (v2 >> 24) & 0xFF;

		dro[12] = lp & 0xFF;
		dro[13] = (lp >> 8) & 0xFF;
		dro[14] = (lp >> 16) & 0xFF;
		dro[15] = (lp >> 24) & 0xFF;

		dro[16] = lm & 0xFF;
		dro[17] = (lm >> 8) & 0xFF;
		dro[18] = (lm >> 16) & 0xFF;
		dro[19] = (lm >> 24) & 0xFF;

		dro[20] = hdr[0];
		dro[21] = hdr[1];
		dro[22] = hdr[2];
		dro[23] = sc;
		dro[24] = lc;
		dro[25] = cmLen;

		memcpy(dro + 26, out, total);

		size_t   cap = dro_len + 1024;
		uint8_t* hs  = malloc(cap);
		size_t   hs_len = hs_encode(dro, dro_len, hs_w, hs_l, hs, cap);

		if (hs_len == 0 || hs_len >= cap) {
			fprintf(stderr, "dro2hdr: heatshrink encode failed\n");
			free(out);
			free(dro);
			free(hs);
			fclose(g);
			return 9;
		}

		fprintf(g,
				"/* Auto-generated by tools/dro2hdr --hs=%dl%d from %s.\n"
				" * heatshrink-packed DRO v2 payload.\n"
				" * Register writes : %u\n"
				" * Total length    : %u ms (%.1f s)\n"
				" * Plain DRO size  : %zu bytes\n"
				" * Packed size     : %zu bytes (%.1f%%)\n"
				" * Decoder RAM     : %u bytes (window) + ~40 bytes state\n"
				" * Do not edit.\n"
				" */\n"
				"#ifndef %s_H\n#define %s_H\n\n"
				"#include \"opl_song_hs.h\"\n\n"
				"static const uint8_t %s_hs_data[];   /* defined below */\n\n"
				"static const opl_song_hs %s = {\n"
				"    .hs_data      = %s_hs_data,\n"
				"    .hs_len       = %zuu,\n"
				"    .total_ms     = %uu,\n"
				"    .codemap_len  = %u,\n"
				"    .short_code   = 0x%02X,\n"
				"    .long_code    = 0x%02X,\n"
				"    .hs_window    = %d,\n"
				"    .hs_lookahead = %d,\n"
				"};\n\n"
				"static const uint8_t %s_hs_data[] = {\n   ",
				hs_w, hs_l, in_path, lp, lm, lm / 1000.0,
				dro_len, hs_len, 100.0 * hs_len / dro_len,
				1u << hs_w,
				sym, sym, sym,
				sym, sym, hs_len, lm, cmLen, sc, lc, hs_w, hs_l,
				sym);

		for (size_t i = 0; i < hs_len; i++) {
			fprintf(g, " 0x%02X,", hs[i]);

			if ((i & 15) == 15)
				fputs("\n   ", g);
		}

		if ((hs_len & 15) != 0)
			fputc('\n', g);

		fprintf(g, "};\n\n#endif\n");

		fclose(g);

		printf("Wrote %s: %u writes, %u ms, %zu bytes plain -> %zu bytes hs (%.1f%%, w=%d l=%d).\n",
			   out_path, lp, lm, dro_len, hs_len,
			   100.0 * hs_len / dro_len, hs_w, hs_l);

		free(dro);
		free(hs);
		free(out);
		return 0;
	}

	fprintf(g,
			"/* Auto-generated by tools/dro2hdr from %s.\n"
			" * Packed (opl_song) format: ~2 bytes per OPL register write.\n"
			" * Register writes : %u\n"
			" * Total length    : %u ms (%.1f s)\n"
			" * Payload size    : %zu bytes (codemap=%u + stream=%u)\n"
			" * Do not edit.\n"
			" */\n"
			"#ifndef %s_H\n#define %s_H\n\n"
			"#include \"seq_player.h\"\n\n"
			"static const uint8_t %s_data[];   /* defined below */\n\n"
			"static const opl_song %s = {\n"
			"    .data        = %s_data,\n"
			"    .data_len    = %zuu,\n"
			"    .codemap_len = %u,\n"
			"    .short_code  = 0x%02X,\n"
			"    .long_code   = 0x%02X,\n"
			"    .total_ms    = %uu,\n"
			"};\n\n"
			"static const uint8_t %s_data[] = {\n   ",
			in_path, lp, lm, lm / 1000.0,
			total, cmLen, lp * 2,
			sym, sym, sym,
			sym, sym, total, cmLen, sc, lc, lm,
			sym);

	for (size_t i = 0; i < total; i++) {
		fprintf(g, " 0x%02X,", out[i]);

		if ((i & 15) == 15)
			fputs("\n   ", g);
	}

	if ((total & 15) != 0)
		fputc('\n', g);

	fprintf(g, "};\n\n#endif\n");

	fclose(g);

	printf("Wrote %s: %u register writes, %u ms, %zu bytes payload (~%.1f KB).\n",
		   out_path, lp, lm, total, total / 1024.0);
	free(out);
	return 0;
}
