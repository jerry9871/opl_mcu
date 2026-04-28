/* Print every event in [from_ms,to_ms]. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char** argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s file.dro from_ms to_ms\n", argv[0]);
		return 1;
	}

	FILE* f = fopen(argv[1], "rb");
	uint8_t hdr[26];
	fread(hdr, 1, 26, f);
	uint32_t lp = hdr[12] | (hdr[13] << 8) | (hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
	uint8_t sc = hdr[23], lc = hdr[24], cmLen = hdr[25];
	uint8_t cm[256] = {0};
	fread(cm, 1, cmLen, f);
	uint8_t* raw = malloc(lp * 2);
	fread(raw, 1, lp * 2, f);
	fclose(f);

	uint32_t from = (uint32_t)atoi(argv[2]);
	uint32_t to   = (uint32_t)atoi(argv[3]);
	uint32_t t = 0;
	uint32_t idx = 0;

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

		if (t > to)
			break;

		if (t >= from) {
			uint16_t r = cm[c & 0x7F] | ((c & 0x80) ? 0x100 : 0);
			const char* tag = "";

			if ((r & 0xF0) == 0xB0)
				tag = (val & 0x20) ? " <-KEY-ON " : " <-key-off";

			printf("[%5u]  t=%6u ms  reg=0x%03X  val=0x%02X%s\n", idx, t, r, val, tag);
		}

		idx++;
	}

	free(raw);
	return 0;
}
