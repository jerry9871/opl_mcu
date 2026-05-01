/*
    hs_stream.h — generic heatshrink-blob byte stream.

    Wraps a heatshrink-compressed blob (typically resident in flash)
    behind a tiny pull API:

        hs_stream_init(st, ptr, len, dec);
        int b;
        while ((b = hs_stream_next(st)) >= 0) consume(b);
        hs_stream_rewind(st);

    No malloc, no stdio, no FILE*, no application-specific knowledge.
    All state lives in caller-provided structs (`hs_stream` and a
    `heatshrink_decoder`), so it links cleanly into MCU firmware.

    The decoder pointer is opaque to this module; on the PC it is
    typically obtained from `heatshrink_decoder_alloc()`, on the MCU
    it is the address of a static `heatshrink_decoder` built in the
    HEATSHRINK_STATIC_ALLOC=1 mode.  Either way, the (window,
    lookahead) baked into the decoder MUST match the values the blob
    was packed with.
*/
#ifndef HS_STREAM_H
#define HS_STREAM_H

#include <stdint.h>
#include <stddef.h>
#include "heatshrink_decoder.h"

typedef struct hs_stream {
	const uint8_t*       data;    /* compressed blob (flash)             */
	uint32_t             len;     /* size of `data` in bytes             */
	uint32_t             cpos;    /* bytes already sunk into the decoder */
	heatshrink_decoder*  dec;     /* caller-provided, must outlive st    */
} hs_stream;

/*  Bind a compressed blob to a decoder.  Resets the decoder so the
    next hs_stream_next() returns the first byte of the decompressed
    payload.  No allocation. */
void
hs_stream_init(hs_stream* st,
			   const uint8_t* data, uint32_t len,
			   heatshrink_decoder* dec);

/*  Pull one decompressed byte.  Returns 0..255, or -1 on end-of-
    stream.  Pumps the decoder (sink/poll/finish) as needed. */
int
hs_stream_next(hs_stream* st);

/*  Rewind to the beginning of the compressed blob.  After this the
    next hs_stream_next() once again returns the first decompressed
    byte.  Cheap: just a decoder_reset and a cursor zero. */
void
hs_stream_rewind(hs_stream* st);

#endif /* HS_STREAM_H */
