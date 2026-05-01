/*
    seq_player.c — implementation of the tick/render API declared in
    seq_player.h.

    Single-instance: one global OPL3 chip and one current song.  This
    is exactly what you want on a microcontroller; multi-song mixing
    is out of scope here.

    Two render strategies are available; pick one at compile time.

      - Default (PC build):  synth_render_sample() calls
        OPL3_GenerateResampled() inline.  No FIFO, no separate
        producer.  Simplest possible code path.

      - -DSEQ_PLAYER_FIFO (MCU build):  synth_update() bulk-renders
        OPL3 frames into a sample FIFO from a low-priority context;
        synth_render_sample() pops one frame in the audio ISR and
        holds the previous frame on underrun.  The producer guards
        the FIFO put with __disable_irq()/__enable_irq() because the
        underlying byte FIFO uses non-atomic counter updates; the
        audio ISR pops without further guarding (it cannot be
        preempted by the producer).  Requires fifo.h and cmsis_gcc.h
        on the include path.
*/
#include "seq_player.h"
#include "opl3.h"

#ifdef SEQ_PLAYER_FIFO
	#include "fifo.h"
	#include "cmsis_gcc.h"
	#ifndef SEQ_FAST_SECTION
		#define SEQ_FAST_SECTION __attribute__((section(".ccm")))
	#endif
#else
	#define SEQ_FAST_SECTION
#endif

/* ---- module state -------------------------------------------------- */

static opl3_chip          g_chip;

static const opl_song*        g_song;
static uint32_t               g_ppos;       /* byte offset into song->data, after codemap */

static const opl_song_stream* g_stream;
static uint8_t                g_codemap[128];
static int                    g_eof;

static int32_t           g_due_ms;     /* ms until next event fires    */
static uint8_t           g_loop;
static uint8_t           g_playing;

#ifdef SEQ_PLAYER_FIFO
	/*  Sample FIFO between the (low-priority) sequencer/synth and the
	(high-priority) audio ISR consumer.  See file header for details.
	One frame = stereo pair (int16 L + int16 R = 4 bytes).             */
	#ifndef SEQ_FIFO_FRAMES
		#define SEQ_FIFO_FRAMES 1024u
	#endif
	#define SEQ_FIFO_BYTES   (SEQ_FIFO_FRAMES * 4u)
	#define SEQ_FRAME_BYTES  4u

	static struct fifo       g_fifo;
	static int16_t           g_last_l;     /* last popped frame, used to   */
	static int16_t           g_last_r;     /* hold output on underrun      */
	static seq_key_cb        g_key_cb;     /* optional visualizer hook     */
#endif

/* ---- streaming helpers --------------------------------------------- */

static int
stream_next(void)
{
	int b = g_stream->next_byte(g_stream->user);

	if (b < 0)
		g_eof = 1;

	return b;
}

static void
stream_load_codemap(void)
{
	for (uint16_t i = 0; i < g_stream->codemap_len; i++) {
		int b = stream_next();

		if (b < 0) {
			g_playing = 0;
			return;
		}

		g_codemap[i] = (uint8_t)b;
	}
}

/* ---- public API ---------------------------------------------------- */

void
synth_init(uint32_t sample_rate_hz)
{
	OPL3_Reset(&g_chip, sample_rate_hz);
	g_song    = 0;
	g_stream  = 0;
	g_ppos    = 0;
	g_eof     = 0;
	g_due_ms  = 0;
	g_loop    = 0;
	g_playing = 0;

	#ifdef SEQ_PLAYER_FIFO
	fifo_init(&g_fifo, SEQ_FIFO_BYTES);
	g_last_l = 0;
	g_last_r = 0;
	/* g_key_cb is preserved across re-init so callers can install once */
	#endif
}

#ifdef SEQ_PLAYER_FIFO
void
seq_set_key_cb(seq_key_cb cb)
{
	g_key_cb = cb;
}
#endif

void
seq_play_song(const opl_song* song, int loop)
{
	g_song    = song;
	g_stream  = 0;
	g_ppos    = song ? song->codemap_len : 0;
	g_due_ms  = 0;
	g_loop    = loop ? 1 : 0;
	g_playing = (song && song->data && song->data_len > song->codemap_len) ? 1 : 0;
}

void
seq_play_stream(const opl_song_stream* stream, int loop)
{
	g_song    = 0;
	g_stream  = stream;
	g_eof     = 0;
	g_due_ms  = 0;
	g_loop    = loop ? 1 : 0;
	g_playing = (stream && stream->next_byte) ? 1 : 0;

	if (g_playing && stream->codemap_len)
		stream_load_codemap();
}

void
seq_stop(void)
{
	g_playing = 0;
}
int
seq_is_playing(void)
{
	return g_playing;
}

void
seq_silence(void)
{
	/*  Key-off all 18 OPL3 channels (registers 0xB0..0xB8 on both ports)
	    and disable the rhythm part. */
	for (uint16_t r = 0xB0; r <= 0xB8; r++)
		OPL3_WriteReg(&g_chip, r,         0);

	for (uint16_t r = 0xB0; r <= 0xB8; r++)
		OPL3_WriteReg(&g_chip, r | 0x100, 0);

	OPL3_WriteReg(&g_chip, 0xBD, 0);
}

void
seq_tick(uint32_t ms_elapsed)
{
	if (!g_playing)
		return;

	g_due_ms -= (int32_t)ms_elapsed;

	/*  Walk the (code,val) opcode stream, firing every event whose
	    deadline has been reached.  The while loop matters because a
	    coarse tick (or an init burst with delay 0) can release several
	    events at once. */
	const opl_song* s = g_song;

	while (g_due_ms <= 0) {
		uint8_t code, val;

		if (s) {
			if (g_ppos >= s->data_len) {
				if (g_loop) {
					g_ppos   = s->codemap_len;
					g_due_ms = 0;
					continue;
				}

				g_playing = 0;
				return;
			}

			code = s->data[g_ppos++];
			val  = s->data[g_ppos++];

		} else {
			/* Streaming mode: pull from callback. */
			int c = stream_next();

			if (c < 0) {
				if (g_loop && g_stream->rewind) {
					g_eof = 0;
					g_stream->rewind(g_stream->user);
					stream_load_codemap();
					g_due_ms = 0;
					continue;
				}

				g_playing = 0;
				return;
			}

			code = (uint8_t)c;
			int v = stream_next();

			if (v < 0) {
				g_playing = 0;
				return;
			}

			val = (uint8_t)v;
		}

		uint8_t  short_code = s ? s->short_code : g_stream->short_code;
		uint8_t  long_code  = s ? s->long_code  : g_stream->long_code;

		if (code == short_code)
			g_due_ms += (int32_t)val + 1;

		else if (code == long_code)
			g_due_ms += ((int32_t)val + 1) * 256;

		else {
			const uint8_t* cm = s ? s->data : g_codemap;
			uint16_t reg = (uint16_t)cm[code & 0x7F]
						   | ((code & 0x80) ? 0x100 : 0);
			OPL3_WriteRegBuffered(&g_chip, reg, val);

			#ifdef SEQ_PLAYER_FIFO

			if (g_key_cb && (reg & 0xff) >= 0xB0 && (reg & 0xff) <= 0xB8)
				g_key_cb(val & 0x1f);

			#endif
		}
	}
}

#ifdef SEQ_PLAYER_FIFO

SEQ_FAST_SECTION void
synth_update(void)
{
	/*  Top up the sample FIFO with as many frames as currently fit.
	    Runs at the sequencer cadence (typ. 1 ms), so any per-tick
	    jitter in OPL3 cost is absorbed by the ring rather than
	    starving the audio ISR.

	    The `timeout` cap is a belt-and-braces safety net: if the
	    audio ISR is somehow draining faster than we can produce, we
	    refuse to spin forever in this call and yield to whatever is
	    above us in the priority chain. */

	int timeout = 1000;

	while (FIFO_FREECOUNT(&g_fifo) >= SEQ_FRAME_BYTES) {
		int16_t f[2];
		OPL3_Generate(&g_chip, f);  /* f[0] = L, f[1] = R */

		/*  Push the whole stereo frame in one buffered call.  The
		    underlying fifo cnt+=len is non-atomic vs. the ISR's
		    cnt-=len, so we gate IRQs around the put.  The window is
		    tiny (a memcpy of 4 bytes + a counter add). */
		__disable_irq();
		(void)fifo_put_buf(&g_fifo, (uint8_t*)f, SEQ_FRAME_BYTES);
		__enable_irq();

		if (timeout == 0)
			break;

		timeout--;
	}
}

SEQ_FAST_SECTION void
synth_render_sample(int16_t* out_l, int16_t* out_r)
{
	/*  Pop one stereo frame.  On underrun (producer fell behind) hold
	    the previous frame to avoid the click that a hard zero would
	    produce. */
	int16_t f[2];

	if (fifo_get_buf(&g_fifo, (uint8_t*)f, SEQ_FRAME_BYTES) < 0) {
		*out_l = g_last_l;
		*out_r = g_last_r;

	} else {
		g_last_l = f[0];
		g_last_r = f[1];
		*out_l   = f[0];
		*out_r   = f[1];
	}
}

#else  /* !SEQ_PLAYER_FIFO -- simple inline render (PC build) */

void
synth_render_sample(int16_t* out_l, int16_t* out_r)
{
	int16_t f[2];
	OPL3_GenerateResampled(&g_chip, f);
	*out_l = f[0];
	*out_r = f[1];
}

#endif
