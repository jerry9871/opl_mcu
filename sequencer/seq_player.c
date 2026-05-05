/*
    seq_player.c — implementation of the tick/render API declared in
    seq_player.h.

    Single-instance: one global OPL3 chip and one current song.  This
    is exactly what you want on a microcontroller; multi-song mixing
    is out of scope here.

    Two render strategies are provided side by side; pick one at the
    call site.

      - synth_render_sample()       -- inline, no FIFO; calls
                                       OPL3_GenerateResampled() right
                                       in the audio ISR.  Simplest
                                       possible code path.

      - synth_update_fifo() +       -- producer/consumer split via a
        synth_get_fifo_sample()        local lock-free SPSC ring of
                                       stereo frames.  The producer
                                       runs in the sequencer task,
                                       the consumer pops one frame
                                       in the audio ISR and holds the
                                       previous frame on underrun.

    Both APIs are always compiled.  The FIFO is single-producer /
    single-consumer with head and tail indices treated as plain
    integers; no interrupt guards are needed as long as the producer
    and the consumer each run in their own context.  The ring lives
    in plain .bss -- no CCM/DTCM attribute, no CMSIS dependency, no
    external fifo.h.  Place it in fast RAM via your linker script if
    you like.
*/
#include "seq_player.h"
#include "opl3.h"

/* ---- ISR guards ---------------------------------------------------- */

/*  Two short critical sections inside this file must not be preempted
    by the audio ISR:

      - the OPL3_WriteRegBuffered() call in seq_tick(), because the
        inline render path's audio ISR calls OPL3_GenerateResampled()
        which reads the very chip state seq_tick is mutating;

      - the ring-slot store + head publish in synth_update_fifo(),
        because the FIFO render path's audio ISR calls
        synth_get_fifo_sample() which reads head/tail and dereferences
        the slot.

    The hooks below are no-ops by default (PC build / non-MCU host).
    On a Cortex-M target wire them to CMSIS, e.g. in a project header
    or on the compile command line:

        #define SEQ_ISR_DISABLE() __disable_irq()
        #define SEQ_ISR_ENABLE()  __enable_irq()

    Or, if you only need to mask a single high-priority audio IRQ,
    use NVIC_DisableIRQ()/NVIC_EnableIRQ() for that vector. */

#include "mcu.h"

#ifndef SEQ_ISR_DISABLE
	#define SEQ_ISR_DISABLE() ((void)0)
#endif
#ifndef SEQ_ISR_ENABLE
	#define SEQ_ISR_ENABLE()  ((void)0)
#endif

/*  Place the audio-ISR-side render helpers in a fast memory section
    (e.g. STM32 CCM RAM) to avoid flash-bus contention with DMA and
    other interrupt fetches.  Defaults to `.ccm` on GCC/Clang; override
    at build time via -DHOT_FUNC=... or disable with -DHOT_FUNC=. */
#ifndef HOT_FUNC
	#if defined(__GNUC__) || defined(__clang__)
		#define HOT_FUNC __attribute__((section(".ccm")))
	#else
		#define HOT_FUNC
	#endif
#endif

/* ---- resampling switch -------------------------------------------- */

/*  Both real-time render paths (inline `synth_render_sample` and FIFO
    `synth_update_fifo`) go through the same generator, so they share a
    single compile-time switch:

      SEQ_RESAMPLE = 1 (default) -> OPL3_GenerateResampled(): one frame
                                    at the host sample_rate_hz passed
                                    to synth_init().  Use when the
                                    audio sink runs at an arbitrary
                                    rate (44100, 48000, ...).

      SEQ_RESAMPLE = 0           -> OPL3_Generate(): one frame at the
                                    chip's native rate (49716 Hz for
                                    most cores).  Cheaper -- no
                                    resampler accumulator, no extra
                                    sample on phase wrap -- and bit-
                                    exact w.r.t. the underlying core.
                                    Use when your DAC clock is set to
                                    the chip's native rate (or when
                                    you don't care about the small
                                    pitch offset).

    Both paths must use the same value, otherwise the FIFO and the
    inline path would disagree on what one frame means.  */
#ifndef SEQ_RESAMPLE
	#define SEQ_RESAMPLE 1
#endif

#if SEQ_RESAMPLE
	#define SEQ_GENERATE(chip, f) OPL3_GenerateResampled((chip), (f))
#else
	#define SEQ_GENERATE(chip, f) OPL3_Generate((chip), (f))
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

/* ---- local SPSC frame ring ---------------------------------------- */

/*  Power-of-two so the modulo collapses to a mask.  1024 frames @
    49716 Hz is ~20 ms of headroom; tune via -DSEQ_FIFO_FRAMES if you
    need more or less. */
#ifndef SEQ_FIFO_FRAMES
	#define SEQ_FIFO_FRAMES 1024u
#endif
#if (SEQ_FIFO_FRAMES & (SEQ_FIFO_FRAMES - 1u)) != 0u
	#error "SEQ_FIFO_FRAMES must be a power of two"
#endif
#define SEQ_FIFO_MASK    (SEQ_FIFO_FRAMES - 1u)

static int16_t  g_fifo_l[SEQ_FIFO_FRAMES];
static int16_t  g_fifo_r[SEQ_FIFO_FRAMES];
static volatile uint32_t g_fifo_head;       /* writer index (producer) */
static volatile uint32_t g_fifo_tail;       /* reader index (consumer) */

static int16_t           g_last_l;          /* held on consumer underrun */
static int16_t           g_last_r;

static seq_key_cb        g_key_cb;          /* optional visualizer hook */

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

	g_fifo_head = 0;
	g_fifo_tail = 0;
	g_last_l    = 0;
	g_last_r    = 0;
	/* g_key_cb is preserved across re-init so callers can install once */
}

void
seq_set_key_cb(seq_key_cb cb)
{
	g_key_cb = cb;
}

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

			/*  Inline render path: the audio ISR may be reading chip
			    state via OPL3_GenerateResampled() right now.  Mask it
			    for the duration of the register write so the ISR
			    never sees a half-updated operator/channel.  No-op on
			    the FIFO path (the ISR doesn't touch chip state). */
			SEQ_ISR_DISABLE();
			OPL3_WriteRegBuffered(&g_chip, reg, val);
			SEQ_ISR_ENABLE();

			if (g_key_cb && (reg & 0xff) >= 0xB0 && (reg & 0xff) <= 0xB8)
				g_key_cb(val & 0x1f);
		}
	}
}

/* ---- inline render path ------------------------------------------- */

HOT_FUNC void
synth_render_sample(int16_t* out_l, int16_t* out_r)
{
	int16_t f[2];
	SEQ_GENERATE(&g_chip, f);
	*out_l = f[0];
	*out_r = f[1];
}

/* ---- FIFO render path --------------------------------------------- */

HOT_FUNC void
synth_update_fifo(void)
{
	/*  Top up the FIFO with as many frames as currently fit.  Runs
	    at the sequencer cadence (typ. 1 ms), so any per-tick jitter
	    in OPL3 cost is absorbed by the ring rather than starving
	    the audio ISR.

	    The `timeout` cap is a belt-and-braces safety net: if the
	    audio ISR is somehow draining faster than we can produce, we
	    refuse to spin forever in this call and yield to whatever is
	    above us in the priority chain. */

	int timeout = (int)SEQ_FIFO_FRAMES;

	for (;;) {
		uint32_t head = g_fifo_head;
		uint32_t tail = g_fifo_tail;
		uint32_t used = (head - tail) & SEQ_FIFO_MASK;

		if (used >= SEQ_FIFO_MASK)        /* one slot reserved as full marker */
			break;

		int16_t f[2];
		SEQ_GENERATE(&g_chip, f);         /* f[0] = L, f[1] = R */

		uint32_t slot = head & SEQ_FIFO_MASK;

		/*  Mask the audio ISR while we publish the new frame.  The
		    consumer reads tail-then-head-then-slot; if it sees the
		    bumped head it must also see the slot stores, so the two
		    have to land atomically from the ISR's point of view.
		    The window is tiny (two int16 stores + one uint32 store).
		    No-op on the inline render path (the FIFO is unused). */
		SEQ_ISR_DISABLE();
		g_fifo_l[slot] = f[0];
		g_fifo_r[slot] = f[1];
		g_fifo_head    = head + 1u;
		SEQ_ISR_ENABLE();

		if (--timeout <= 0)
			break;
	}
}

HOT_FUNC void
synth_get_fifo_sample(int16_t* out_l, int16_t* out_r)
{
	uint32_t head = g_fifo_head;
	uint32_t tail = g_fifo_tail;

	if (head == tail) {
		/* underrun -- hold the previous frame to avoid a click */
		*out_l = g_last_l;
		*out_r = g_last_r;
		return;
	}

	uint32_t slot = tail & SEQ_FIFO_MASK;
	int16_t  l    = g_fifo_l[slot];
	int16_t  r    = g_fifo_r[slot];

	g_fifo_tail = tail + 1u;

	g_last_l = l;
	g_last_r = r;
	*out_l   = l;
	*out_r   = r;
}
