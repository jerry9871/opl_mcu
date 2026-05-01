/*
    hs_stream.c — generic heatshrink byte-stream pump.

    No malloc, no stdio.  See hs_stream.h for the contract.
*/
#include "hs_stream.h"

#define HS_FEED_CHUNK   64u  /* bytes per sink call (latency vs. overhead) */

void
hs_stream_init(hs_stream* st,
			   const uint8_t* data, uint32_t len,
			   heatshrink_decoder* dec)
{
	st->data = data;
	st->len  = len;
	st->cpos = 0;
	st->dec  = dec;

	heatshrink_decoder_reset(dec);
}

int
hs_stream_next(hs_stream* st)
{
	heatshrink_decoder* dec = st->dec;

	for (;;) {
		uint8_t b;
		size_t  got = 0;
		HSD_poll_res pr =
			heatshrink_decoder_poll(dec, &b, 1, &got);

		if (got == 1)
			return b;

		if (pr == HSDR_POLL_EMPTY) {
			/*  Decoder wants more compressed input. */
			if (st->cpos >= st->len) {
				HSD_finish_res fr =
					heatshrink_decoder_finish(dec);

				if (fr == HSDR_FINISH_DONE)
					return -1;

				/* HSDR_FINISH_MORE: loop and poll again */
				continue;
			}

			size_t want = st->len - st->cpos;

			if (want > HS_FEED_CHUNK)
				want = HS_FEED_CHUNK;

			size_t sunk = 0;
			heatshrink_decoder_sink(dec,
									(uint8_t*)st->data + st->cpos,
									want, &sunk);
			st->cpos += (uint32_t)sunk;
			continue;
		}

		/*  HSDR_POLL_MORE: more output buffered, loop and pull it. */
	}
}

void
hs_stream_rewind(hs_stream* st)
{
	heatshrink_decoder_reset(st->dec);
	st->cpos = 0;
}
