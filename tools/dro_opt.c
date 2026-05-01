/*
    dro_opt.c — lossless / near-lossless size optimizer for DRO v2 files.

    Two passes, both optional:

      1. Redundant-write filter (always on)
         Track the OPL register file (init = all zero, matching chip
         reset).  Drop any write whose value already matches the
         current shadow.  Delay of dropped event is folded into the
         previous kept event so total length is preserved.

      2. Time quantizer (--quant=N, default 0 = off)
         Snap every event's absolute timestamp to an N-ms grid
         (round-to-nearest).  After snapping, several events for the
         same register may share the same tick -- only the *last* one
         in original order is kept (the chip would only see that one
         anyway in a single OPL register cycle).  Pass 1 is then re-run
         over the snapped stream because new writes may have become
         redundant.

    Usage:
        dro_opt  in.dro  out.dro  [--quant=N]
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

	*o = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
		 ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	return 0;
}

static void
write_u32(FILE* f, uint32_t v)
{
	uint8_t b[4] = { v & 0xFF, (v >> 8) & 0xFF,
					 (v >> 16) & 0xFF, (v >> 24) & 0xFF
				   };
	fwrite(b, 1, 4, f);
}

typedef struct {
	uint32_t t_ms;
	uint16_t reg;
	uint8_t val;
} Ev;

/* --------------------------------------------------------------- */
/*  Pass 1: drop writes that don't change OPL state.               */
/* --------------------------------------------------------------- */
static size_t
filter_redundant(Ev* ev, size_t n)
{
	uint8_t shadow[0x200] = {0};
	size_t  out = 0;

	for (size_t i = 0; i < n; i++) {
		if (ev[i].reg < 0x200 && shadow[ev[i].reg] == ev[i].val)
			continue;

		shadow[ev[i].reg] = ev[i].val;
		ev[out++] = ev[i];
	}

	return out;
}

/* --------------------------------------------------------------- */
/*  Pass 2: quantize timestamps to an N-ms grid.                   */
/* --------------------------------------------------------------- */
static size_t
quantize(Ev* ev, size_t n, uint32_t grid)
{
	if (grid <= 1 || n == 0)
		return n;

	for (size_t i = 0; i < n; i++)
		ev[i].t_ms = ((ev[i].t_ms + grid / 2) / grid) * grid;

	return n;
}

/* --------------------------------------------------------------- */
/*  Pass 3: per-tick dedup of same-register writes.                */
/*                                                                 */
/*  After quantize() many events land on the same tick.  Within a  */
/*  single tick the OPL chip would only see the LAST write to any  */
/*  given register, so earlier writes to that register are usually */
/*  redundant.                                                     */
/*                                                                 */
/*  EXCEPTION -- retrigger registers must keep every write in      */
/*  order:                                                         */
/*     0xB0..0xB8, 0x1B0..0x1B8 -- bit 5 = key-on; (off,on) is     */
/*                                  the standard envelope retrigger */
/*     0xBD                     -- rhythm-mode key-on bits         */
/*                                                                 */
/*  --keep-retriggers=0 disables the exception (smaller, but kills */
/*  percussion attack).                                            */
/* --------------------------------------------------------------- */
static int
is_retrigger_reg(uint16_t r)
{
	if (r == 0xBD)
		return 1;

	uint16_t lo = r & 0xFF;

	return lo >= 0xB0 && lo <= 0xB8;
}

static size_t
dedup_per_tick(Ev* ev, size_t n, int keep_retriggers)
{
	if (n == 0)
		return 0;

	uint8_t* keep = malloc(n);

	if (!keep) {
		fprintf(stderr, "dro_opt: out of memory\n");
		exit(1);
	}

	memset(keep, 1, n);

	/*  Walk ticks.  Within each tick, scan from end to start and
	    mark a register as "already kept"; drop any earlier write to
	    the same register UNLESS it's a retrigger reg and the user
	    asked us to preserve those.                              */
	size_t i = 0;

	while (i < n) {
		size_t j = i + 1;

		while (j < n && ev[j].t_ms == ev[i].t_ms)
			j++;

		/*  [i, j) is one tick.  Mark from the back. */
		uint8_t seen[0x200] = {0};

		for (size_t k = j; k-- > i;) {
			uint16_t r = ev[k].reg;

			if (r >= 0x200)
				continue;

			if (keep_retriggers && is_retrigger_reg(r))
				continue;

			if (seen[r])
				keep[k] = 0;

			else
				seen[r] = 1;
		}

		i = j;
	}

	size_t out = 0;

	for (size_t k = 0; k < n; k++)
		if (keep[k])
			ev[out++] = ev[k];

	free(keep);
	return out;
}

/* --------------------------------------------------------------- */
int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr,
				"usage: %s in.dro out.dro "
				"[--quant=N] [--dedup] [--keep-retriggers=0|1]\n",
				argv[0]);
		return 1;
	}

	const char* in_path         = argv[1];
	const char* out_path        = argv[2];
	uint32_t    grid            = 0;
	int         keep_retriggers = 1;
	int         do_dedup        = 0;

	for (int i = 3; i < argc; i++) {
		if (strncmp(argv[i], "--quant=", 8) == 0)
			grid = (uint32_t)strtoul(argv[i] + 8, NULL, 10);

		else if (strcmp(argv[i], "--dedup") == 0)
			do_dedup = 1;

		else if (strcmp(argv[i], "--keep-retriggers=0") == 0)
			keep_retriggers = 0;

		else if (strcmp(argv[i], "--keep-retriggers=1") == 0)
			keep_retriggers = 1;

		else {
			fprintf(stderr, "dro_opt: unknown arg '%s'\n", argv[i]);
			return 1;
		}
	}

	FILE* f = fopen(in_path, "rb");

	if (!f) {
		perror(in_path);
		return 1;
	}

	char     m[8];
	uint32_t v, lp, lm;
	uint8_t  hdr[5], cmLen, cm[256] = {0};

	if (fread(m, 1, 8, f) != 8 || memcmp(m, "DBRAWOPL", 8) ||
		read_u32(f, &v) || read_u32(f, &lp) || read_u32(f, &lm) || v != 2) {
		fprintf(stderr, "%s: not DRO v2\n", in_path);
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

	/*  Decode (cmd,val) pairs into a flat absolute-time event list. */
	Ev*      ev  = malloc(sizeof(Ev) * lp);
	size_t   nev = 0;
	uint32_t now = 0;

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
			ev[nev].reg  = (uint16_t)(cm[idx] | ((c & 0x80) ? 0x100 : 0));
			ev[nev].val  = val;
			nev++;
		}
	}

	free(raw);

	size_t n0 = nev;

	nev = filter_redundant(ev, nev);
	size_t after_pass1 = nev;

	if (grid > 1) {
		nev = quantize(ev, nev, grid);

		if (do_dedup)
			nev = dedup_per_tick(ev, nev, keep_retriggers);

		nev = filter_redundant(ev, nev);

	} else if (do_dedup) {
		nev = dedup_per_tick(ev, nev, keep_retriggers);
		nev = filter_redundant(ev, nev);
	}

	size_t after_all = nev;

	/* Build a fresh codemap from low-bytes actually used. */
	uint8_t used[256] = {0};

	for (size_t i = 0; i < nev; i++)
		used[ev[i].reg & 0xFF] = 1;

	uint8_t newcm[128];
	uint8_t newcmLen = 0;
	int16_t lo_to_slot[256];

	for (int i = 0; i < 256; i++)
		lo_to_slot[i] = -1;

	for (int r = 0; r < 256; r++) {
		if (used[r]) {
			if (newcmLen >= 126) {
				fprintf(stderr,
						"dro_opt: codemap overflow (>%d)\n", 126);
				return 9;
			}

			newcm[newcmLen]  = (uint8_t)r;
			lo_to_slot[r]    = newcmLen;
			newcmLen++;
		}
	}

	/* Encode (cmd,val) pair stream with delay opcodes. */
	uint8_t* pairs   = malloc((size_t)nev * 4 + 4096);
	size_t   np      = 0;
	uint32_t last_t  = 0;
	uint8_t  out_sc  = 0xFE;
	uint8_t  out_lc  = 0xFF;

	for (size_t i = 0; i < nev; i++) {
		uint32_t dt = (i == 0) ? ev[i].t_ms : (ev[i].t_ms - last_t);
		last_t = ev[i].t_ms;

		while (dt) {
			if (dt >= 256) {
				uint32_t units = dt / 256;

				if (units > 256)
					units = 256;

				pairs[np++] = out_lc;
				pairs[np++] = (uint8_t)(units - 1);
				dt -= units * 256;

			} else {
				pairs[np++] = out_sc;
				pairs[np++] = (uint8_t)(dt - 1);
				dt = 0;
			}
		}

		int slot = lo_to_slot[ev[i].reg & 0xFF];
		uint8_t hi = (ev[i].reg & 0x100) ? 0x80 : 0x00;
		pairs[np++] = (uint8_t)(slot | hi);
		pairs[np++] = ev[i].val;
	}

	/*  Trailing delay so out_lm matches the original total length
	    (preserves loop tail / silence padding).                  */
	uint32_t out_lm = lm;

	if (out_lm > last_t) {
		uint32_t dt = out_lm - last_t;

		while (dt) {
			if (dt >= 256) {
				uint32_t units = dt / 256;

				if (units > 256)
					units = 256;

				pairs[np++] = out_lc;
				pairs[np++] = (uint8_t)(units - 1);
				dt -= units * 256;

			} else {
				pairs[np++] = out_sc;
				pairs[np++] = (uint8_t)(dt - 1);
				dt = 0;
			}
		}
	}

	uint32_t out_lp = (uint32_t)(np / 2);

	FILE* g = fopen(out_path, "wb");

	if (!g) {
		perror(out_path);
		return 11;
	}

	fwrite("DBRAWOPL", 1, 8, g);
	write_u32(g, 2);
	write_u32(g, out_lp);
	write_u32(g, out_lm);

	uint8_t out_hdr[5] = { hdr[0], hdr[1], hdr[2], out_sc, out_lc };
	fwrite(out_hdr, 1, 5, g);
	fwrite(&newcmLen, 1, 1, g);
	fwrite(newcm, 1, newcmLen, g);
	fwrite(pairs, 1, np, g);
	fclose(g);

	(void)sc;
	(void)lc;
	(void)v;

	printf("%-58s %6zu->%-6zu writes "
		   "(pass1=%zu) cm=%u q=%u dedup=%d ktrig=%d\n",
		   in_path, n0, after_all, after_pass1,
		   newcmLen, grid, do_dedup, keep_retriggers);

	free(ev);
	free(pairs);
	return 0;
}
