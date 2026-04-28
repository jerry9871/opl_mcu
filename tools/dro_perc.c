/* List 0xBD (percussion control) writes with timestamps. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s file.dro\n", argv[0]);
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
	uint32_t ms    = b[16] | (b[17] << 8) | (b[18] << 16) | (b[19] << 24);
	uint8_t cmap_len = b[23];
	uint8_t short_d = b[24], long_d = b[25];
	uint8_t* cmap = b + 26;
	uint8_t* p = cmap + cmap_len;
	uint64_t t = 0;
	int n = 0;
	printf("file len=%u ms, pairs=%u, cmap_len=%u\n", ms, pairs, cmap_len);

	for (uint32_t i = 0; i < pairs; i++) {
		uint8_t reg = p[2 * i], val = p[2 * i + 1];

		if (reg == short_d) {
			t += val + 1;
			continue;
		}

		if (reg == long_d) {
			t += (val + 1) * 256;
			continue;
		}

		uint8_t bank = reg >> 7;
		uint8_t idx  = reg & 0x7F;
		uint8_t r = cmap[idx];

		if (r == 0xBD && bank == 0) {
			printf("t=%6llu ms  BD=0x%02X  perc_mode=%d  BD=%d SD=%d TT=%d CY=%d HH=%d\n",
				   (unsigned long long)t, val,
				   (val >> 5) & 1, (val >> 4) & 1, (val >> 3) & 1, (val >> 2) & 1, (val >> 1) & 1, val & 1);

			if (++n >= 40)
				break;
		}
	}

	free(b);
	return 0;
}
