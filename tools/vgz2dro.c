/*
    vgz2dro.c -- convert a VGM (or gzipped VGM = .vgz) file into a
    DOSBox DRO v2 capture, keeping only the OPL writes.

    Why a separate stage instead of going straight to opl_song?
    Because once the file is in DRO form it can flow through the rest
    of our pipeline (dro_loop3, dro_slice, dro2hdr, the upcoming
    redundant-write filter, the upcoming time-quantizer, ...) so every
    optimization we add applies symmetrically to DOSBox captures and
    to wafflenet VGM rips.

    Supported VGM commands (everything else is silently skipped, with
    a one-line summary printed at the end):
       0x4F dd          GG stereo (ignored)
       0x50 dd          PSG (ignored)
       0x5A rr vv       YM3812 / OPL2          -> port 0 register rr
       0x5B rr vv       YM3526 / OPL-L         -> port 0 register rr
       0x5E rr vv       YMF262 port 0          -> register rr
       0x5F rr vv       YMF262 port 1          -> register 0x100|rr
       0x61 nn nn       wait n samples (44100 Hz)
       0x62             wait 735 samples (~1/60 s)
       0x63             wait 882 samples (~1/50 s)
       0x66             end of sound data
       0x70..0x7F       wait n+1 samples (1..16)

    Loop handling: if the VGM header carries a non-zero loop offset
    AND --loop-only is given on the command line, only the loop body
    is emitted (the intro is dropped).  Otherwise the whole song goes
    through unchanged.

    Usage:
        vgz2dro [--loop-only] in.vgz|in.vgm out.dro

    Offline tool -- not part of the MCU build.  Requires zlib.
*/
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ------------------------------------------------------------------- */
/*  Slurp a possibly-gzipped file into a single malloc'd buffer.       */
/* ------------------------------------------------------------------- */

static int
slurp_gz(const char* path, uint8_t** out_buf, size_t* out_len)
{
	gzFile gz = gzopen(path, "rb");

	if (!gz) {
		perror(path);
		return -1;
	}

	size_t   cap = 1 << 16;
	size_t   len = 0;
	uint8_t* buf = malloc(cap);

	for (;;) {
		if (len + 4096 > cap) {
			cap *= 2;
			buf = realloc(buf, cap);
		}

		int n = gzread(gz, buf + len, (unsigned)(cap - len));

		if (n < 0) {
			fprintf(stderr, "%s: gzread failed\n", path);
			free(buf);
			gzclose(gz);
			return -1;
		}

		if (n == 0)
			break;

		len += (size_t)n;
	}

	gzclose(gz);
	*out_buf = buf;
	*out_len = len;
	return 0;
}

static uint32_t
rd32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------- */
/*  Decoded register-write event with the delay that follows it.       */
/* ------------------------------------------------------------------- */

typedef struct {
	uint16_t reg;        /* 0x000..0x1FF (bit 8 = OPL3 second port) */
	uint8_t  val;
	uint32_t delay_ms;   /* delay AFTER this write */
} ev_t;

/* ------------------------------------------------------------------- */
/*  Walk the VGM command stream into an event list.                    */
/* ------------------------------------------------------------------- */

static int
decode_vgm(const uint8_t* p, size_t len, size_t start, size_t end,
		   ev_t** out_ev, size_t* out_n,
		   uint64_t* out_total_samples,
		   uint32_t* out_unknown_cmds)
{
	(void)len;
	size_t cap = 1024;
	size_t n   = 0;
	ev_t*  ev  = malloc(sizeof(ev_t) * cap);

	uint64_t pending = 0;     /* samples of delay accumulated since last write */
	uint64_t total   = 0;
	uint32_t unknown = 0;

#define FLUSH_DELAY()                                                  \
	do {                                                                \
		if (n > 0 && pending) {                                         \
			/*  Convert sample count (44100 Hz) to ms with rounding.    \
									    Accumulator stays in samples to avoid drift over long   \
									    songs. */                                               \
			uint32_t ms = (uint32_t)((pending * 1000 + 22050) / 44100); \
			ev[n - 1].delay_ms += ms;                                   \
			total              += pending;                              \
			pending = 0;                                                \
		}                                                                \
	} while (0)

#define EMIT(R, V)                                                     \
	do {                                                                \
		FLUSH_DELAY();                                                  \
		if (n == cap) {                                                 \
			cap *= 2;                                                   \
			ev   = realloc(ev, sizeof(ev_t) * cap);                     \
		}                                                                \
		ev[n].reg      = (R);                                           \
		ev[n].val      = (V);                                           \
		ev[n].delay_ms = 0;                                             \
		n++;                                                             \
	} while (0)

	size_t i = start;

	while (i < end) {
		uint8_t c = p[i++];

		switch (c) {
			case 0x4F:              /* GG stereo */
			case 0x50:              /* PSG       */
				if (i >= end)
					goto done;

				i++;
				break;

			case 0x5A:              /* YM3812 OPL2 */
			case 0x5B:              /* YM3526 OPL  */
				if (i + 1 >= end)
					goto done;

				EMIT(p[i], p[i + 1]);
				i += 2;
				break;

			case 0x5E:              /* YMF262 port 0 */
				if (i + 1 >= end)
					goto done;

				EMIT(p[i], p[i + 1]);
				i += 2;
				break;

			case 0x5F:              /* YMF262 port 1 */
				if (i + 1 >= end)
					goto done;

				EMIT(0x100 | p[i], p[i + 1]);
				i += 2;
				break;

			case 0x61:              /* wait n samples */
				if (i + 1 >= end)
					goto done;

				pending += (uint16_t)(p[i] | (p[i + 1] << 8));
				i += 2;
				break;

			case 0x62:              /* wait 735 samples */
				pending += 735;
				break;

			case 0x63:              /* wait 882 samples */
				pending += 882;
				break;

			case 0x66:              /* end of sound data */
				goto done;

			default:
				if (c >= 0x70 && c <= 0x7F)
					pending += (c - 0x70) + 1;

				else if (c >= 0x80 && c <= 0x8F) {
					/*  YM2612 PCM write + wait n; we don't use YM2612
					    data, but the wait part still needs to count. */
					pending += (c - 0x80);

				} else {
					/*  Skip the operand bytes for unrecognised
					    commands using the standard VGM length table.
					    We don't need to be exhaustive -- any chip we
					    don't know about just contributes silence. */
					unknown++;

					if (c >= 0x30 && c <= 0x4E)
						i += 1;

					else if ((c >= 0x51 && c <= 0x5F) ||
							 (c >= 0xA0 && c <= 0xBF))
						i += 2;

					else if (c >= 0xC0 && c <= 0xDF)
						i += 3;

					else if (c >= 0xE0)
						i += 4;

					/* else: single-byte unknown, nothing to skip */
				}

				break;
		}
	}

done:
	FLUSH_DELAY();
	*out_ev            = ev;
	*out_n             = n;
	*out_total_samples = total;
	*out_unknown_cmds  = unknown;
	return 0;

#undef FLUSH_DELAY
#undef EMIT
}

/* ------------------------------------------------------------------- */
/*  Lossless filter: drop writes that don't change the chip state.     */
/*                                                                     */
/*  At reset every OPL register is 0, and many VGM rips begin with an  */
/*  exhaustive zero-wipe of every operator/channel register.  Those    */
/*  writes don't change anything audible AND they bloat the codemap    */
/*  with low-bytes that never get touched again.  Dropping them is     */
/*  pure win and is what dro_compress / dro_analyze would do anyway.   */
/* ------------------------------------------------------------------- */

static void
filter_redundant(ev_t* ev, size_t* pn)
{
	size_t  n   = *pn;
	uint8_t shadow[0x200] = {0};   /* mirrors the chip register file */
	size_t  out = 0;

	for (size_t i = 0; i < n; i++) {
		if (ev[i].reg < 0x200 && shadow[ev[i].reg] == ev[i].val) {
			/*  Same value already in the register; this write is a
			    no-op.  Fold its delay into the previous kept event
			    (or drop entirely if there's no previous event yet). */
			if (out > 0)
				ev[out - 1].delay_ms += ev[i].delay_ms;

			continue;
		}

		if (ev[i].reg < 0x200)
			shadow[ev[i].reg] = ev[i].val;

		ev[out++] = ev[i];
	}

	*pn = out;
}

/* ------------------------------------------------------------------- */
/*  Re-encode an event list as a DRO v2 file.                          */
/* ------------------------------------------------------------------- */

static int
write_dro(const char* path, const ev_t* ev, size_t n)
{
	/*  Build the codemap: each distinct register low-byte gets an
	    index 0..127.  The high port bit becomes the top bit of the
	    opcode in the stream. */
	int    cm_idx[256];
	uint8_t cm[128];
	uint8_t cm_len = 0;

	for (int i = 0; i < 256; i++)
		cm_idx[i] = -1;

	for (size_t i = 0; i < n; i++) {
		uint8_t lo = (uint8_t)(ev[i].reg & 0xFF);

		if (cm_idx[lo] < 0) {
			if (cm_len >= 126) {
				fprintf(stderr,
						"vgz2dro: codemap overflow at write #%zu reg=0x%03X "
						"(>%d distinct OPL reg low-bytes)\n",
						i, ev[i].reg, 126);
				fprintf(stderr, "  low-bytes seen so far:");

				for (int k = 0; k < 256; k++)
					if (cm_idx[k] >= 0)
						fprintf(stderr, " %02X", k);

				fputc('\n', stderr);
				return -1;
			}

			cm_idx[lo] = cm_len;
			cm[cm_len++] = lo;
		}
	}

	/*  Reserve 0xFE / 0xFF as the short / long delay opcodes.  With
	    cm_len <= 126, codemap indices 0..125 plus port-1 codes
	    0x80..0xFD are all valid -- 0xFE and 0xFF are guaranteed not
	    to collide. */
	uint8_t sc = 0xFE;
	uint8_t lc = 0xFF;

	uint64_t tot_ms = 0;

	for (size_t i = 0; i < n; i++)
		tot_ms += ev[i].delay_ms;

	/*  Build the (code, val) opcode pair stream. */
	size_t   pcap = n * 2 + 64;
	size_t   plen = 0;
	uint8_t* pairs = malloc(pcap);

#define PUT2(A, B)                                                  \
	do {                                                             \
		if (plen + 2 > pcap) { pcap *= 2; pairs = realloc(pairs, pcap); } \
		pairs[plen++] = (A);                                         \
		pairs[plen++] = (B);                                         \
	} while (0)

	for (size_t i = 0; i < n; i++) {
		uint16_t reg = ev[i].reg;
		uint8_t  lo  = (uint8_t)(reg & 0xFF);
		uint8_t  code = (uint8_t)cm_idx[lo];

		if (reg & 0x100)
			code |= 0x80;

		PUT2(code, ev[i].val);

		uint32_t d = ev[i].delay_ms;

		while (d >= 257) {
			/*  long opcode encodes (val + 1) * 256 ms, val in 0..255,
			    so 256..65536 ms in 256 ms steps. */
			uint32_t units = d / 256;

			if (units > 256)
				units = 256;

			PUT2(lc, (uint8_t)(units - 1));
			d -= units * 256;
		}

		while (d > 0) {
			/* short opcode: val + 1 ms, val in 0..255, so 1..256 ms */
			uint32_t step = d > 256 ? 256 : d;
			PUT2(sc, (uint8_t)(step - 1));
			d -= step;
		}
	}

#undef PUT2

	/*  Header.  */
	FILE* g = fopen(path, "wb");

	if (!g) {
		perror(path);
		free(pairs);
		return -1;
	}

	uint32_t pairs_count = (uint32_t)(plen / 2);
	uint32_t length_ms   = (uint32_t)tot_ms;

	uint8_t hdr[26] = {
		'D', 'B', 'R', 'A', 'W', 'O', 'P', 'L',
		2, 0, 0, 0,
		(uint8_t)pairs_count, (uint8_t)(pairs_count >> 8),
		(uint8_t)(pairs_count >> 16), (uint8_t)(pairs_count >> 24),
		(uint8_t)length_ms, (uint8_t)(length_ms >> 8),
		(uint8_t)(length_ms >> 16), (uint8_t)(length_ms >> 24),
		1,        /* hwType: OPL3 */
		0,        /* format: cmd/data interleaved */
		0,        /* compression: none */
		sc,
		lc,
		cm_len,
	};

	fwrite(hdr, 1, sizeof(hdr), g);
	fwrite(cm,  1, cm_len, g);
	fwrite(pairs, 1, plen, g);
	fclose(g);
	free(pairs);
	return 0;
}

/* ------------------------------------------------------------------- */
int
main(int argc, char** argv)
{
	int loop_only = 0;
	const char* in_path  = NULL;
	const char* out_path = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--loop-only"))
			loop_only = 1;

		else if (!in_path)
			in_path = argv[i];

		else if (!out_path)
			out_path = argv[i];

		else {
			fprintf(stderr, "extra arg: %s\n", argv[i]);
			return 1;
		}
	}

	if (!in_path || !out_path) {
		fprintf(stderr,
				"usage: %s [--loop-only] in.vgz|in.vgm out.dro\n",
				argv[0]);
		return 1;
	}

	uint8_t* buf;
	size_t   len;

	if (slurp_gz(in_path, &buf, &len) || len < 0x40) {
		fprintf(stderr, "%s: not a readable VGM/VGZ\n", in_path);
		return 2;
	}

	if (memcmp(buf, "Vgm ", 4) != 0) {
		fprintf(stderr, "%s: bad VGM magic\n", in_path);
		free(buf);
		return 2;
	}

	uint32_t version    = rd32(buf + 0x08);
	uint32_t total_smp  = rd32(buf + 0x18);
	uint32_t loop_off   = rd32(buf + 0x1C);
	uint32_t loop_smp   = rd32(buf + 0x20);
	uint32_t data_off   = (version >= 0x150) ? rd32(buf + 0x34) : 0;

	size_t data_start = data_off ? (0x34 + data_off) : 0x40;
	size_t loop_start = loop_off ? (0x1C + loop_off) : 0;

	if (data_start >= len) {
		fprintf(stderr, "%s: bad data offset\n", in_path);
		free(buf);
		return 2;
	}

	size_t walk_start = data_start;

	if (loop_only) {
		if (!loop_off || loop_start <= data_start || loop_start >= len) {
			fprintf(stderr,
					"%s: --loop-only requested but no loop point in file\n",
					in_path);

		} else
			walk_start = loop_start;
	}

	ev_t*    ev;
	size_t   nev;
	uint64_t got_smp;
	uint32_t unknown;

	decode_vgm(buf, len, walk_start, len, &ev, &nev, &got_smp, &unknown);
	free(buf);

	size_t nev_raw = nev;
	filter_redundant(ev, &nev);

	if (write_dro(out_path, ev, nev) != 0) {
		free(ev);
		return 3;
	}

	double secs = (double)got_smp / 44100.0;

	printf("%-48s %5zu writes (-%zu redundant)  %6.1f s",
		   in_path, nev, nev_raw - nev, secs);

	if (loop_off)
		printf("  (loop@%us, %s)",
			   loop_smp / 44100,
			   loop_only ? "loop body only" : "full song");

	if (unknown)
		printf("  [%u unknown cmds skipped]", unknown);

	printf("  -> %s\n", out_path);

	(void)total_smp;
	(void)version;
	free(ev);
	return 0;
}
