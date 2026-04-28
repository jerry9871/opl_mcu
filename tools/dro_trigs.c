/*
    dro_trigs.c — print key-on events with timing, also bucket histogram.
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

	uint8_t prev_key[18] = {0};
	uint32_t t = 0;
	/* histogram of key-ons per second */
	uint32_t kons_per_s[200] = {0};
	/* per-second "fingerprint": which channels have key-ons */
	uint32_t mask_per_s[200] = {0};
	/* count per channel total */
	uint32_t total_per_ch[18] = {0};
	size_t total_kons = 0;

	for (uint32_t i = 0; i < lp; i++) {
		uint8_t c = raw[i * 2], val = raw[i * 2 + 1];

		if (c == sc) {
			t += val + 1;
			continue;
		}

		if (c == lc) {
			t += (val + 1u) * 256u;
			continue;
		}

		uint8_t idx = c & 0x7F;

		if (idx >= cmLen)
			continue;

		uint16_t r = cm[idx] | (c & 0x80 ? 0x100 : 0);

		if ((r & 0xF0) == 0xB0 && (r & 0x0F) <= 8) {
			uint8_t ch = (r & 0x0F) + (r & 0x100 ? 9 : 0);
			uint8_t key = (val & 0x20) ? 1 : 0;

			if (key && !prev_key[ch]) {
				/* key-on edge */
				uint32_t s = t / 1000;

				if (s < 200) {
					kons_per_s[s]++;
					mask_per_s[s] |= (1u << ch);
				}

				total_per_ch[ch]++;
				total_kons++;
			}

			prev_key[ch] = key;
		}
	}

	printf("Total key-ON edges: %zu\n\nPer-channel:\n", total_kons);

	for (int c = 0; c < 18; c++)
		if (total_per_ch[c])
			printf("  ch%2d: %u\n", c, total_per_ch[c]);

	printf("\nPer-second  (kons / channel-mask):\n");

	for (uint32_t s = 0; s * 1000 < lm && s < 200; s++) {
		printf("  %3u s: %4u kons  ch=", s, kons_per_s[s]);

		for (int c = 0; c < 18; c++)
			if (mask_per_s[s] & (1u << c))
				printf("%d,", c);

		printf("\n");
	}

	return 0;
}
