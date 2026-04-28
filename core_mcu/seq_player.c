/*
    seq_player.c — implementation of the API declared in seq_player.h.

    Single global instance: one opl3_chip, one current song, one sample
    FIFO.  See seq_player.h for the public contract, threading model and
    typical wiring example.

    Internally this file is split into three responsibilities:

      1. Sequencer       — seq_play / seq_stop / seq_silence / seq_tick.
                           Walks the opl_event[] array on a millisecond
                           timebase and pushes register writes into the
                           OPL3 core.

      2. Audio producer  — synth_update.
                           Renders OPL3 samples in bulk into a stereo
                           sample FIFO.  Runs in low-priority context.

      3. Audio consumer  — synth_render_sample.
                           Pops one stereo frame for the audio ISR.
                           Holds the previous frame on FIFO underrun.
*/
#include "seq_player.h"
#include "opl3.h"
#include "fifo.h"
#include "cmsis_gcc.h"

/* ---- module state --------------------------------------------------- */

static opl3_chip          g_chip;       /* the OPL3 emulator instance   */
static const opl_event*   g_events;     /* current song (caller-owned)  */
static uint32_t           g_count;      /* number of events in g_events */
static uint32_t           g_pos;        /* index of next event to fire  */
static int32_t            g_due_ms;     /* ms until next event fires    */
static uint8_t            g_loop;       /* 1 = restart at end           */
static uint8_t            g_playing;    /* 1 while sequencer is active  */

/*  Sample FIFO between the (low-priority) sequencer/synth and the
    (high-priority) PWM ISR consumer.  The synth pre-renders frames in
    bulk in synth_update() and pushes them in; the ISR pops one frame
    per call in synth_render_sample().  This decouples worst-case OPL3
    cost from the per-sample budget and absorbs jitter when many
    channels are active.

    One frame = stereo pair (int16 L + int16 R = 4 bytes).
    Sized for ~20 ms cushion at 48 kHz / ~40 ms at 24 kHz. */
#define SYNTH_FIFO_FRAMES    1024u
#define SYNTH_FIFO_BYTES     (SYNTH_FIFO_FRAMES * 4u)
#define SYNTH_FRAME_BYTES    4u

static struct fifo        g_fifo;
static int16_t            g_last_l;     /* last popped frame, used to   */
static int16_t            g_last_r;     /* hold output on underrun      */

static seq_key_cb         g_key_cb;     /* optional LED visualizer hook */

/*  ====================================================================
    One-time initialization
    ==================================================================== */

void
synth_init(uint32_t sample_rate_hz)
{
	OPL3_Reset(&g_chip, sample_rate_hz);
	g_events  = 0;
	g_count   = 0;
	g_pos     = 0;
	g_due_ms  = 0;
	g_loop    = 0;
	g_playing = 0;

	fifo_init(&g_fifo, SYNTH_FIFO_BYTES);
	g_last_l = 0;
	g_last_r = 0;
	/* g_key_cb is preserved across re-init so callers can install once */
}

void
seq_set_key_cb(seq_key_cb cb)
{
	g_key_cb = cb;
}

/*  ====================================================================
    Sequencer control
    ==================================================================== */

void
seq_play(const opl_event* events, uint32_t count, int loop)
{
	g_events  = events;
	g_count   = count;
	g_pos     = 0;
	g_due_ms  = 0;
	g_loop    = loop ? 1 : 0;
	g_playing = (events && count) ? 1 : 0;
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
	/*  Key-off all 18 OPL3 channels (registers 0xB0..0xB8 on both
	    ports, KEY-ON bit cleared) and disable the rhythm part. */
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

	/*  Fire every event whose deadline has been reached.  The while
	    loop matters for two reasons:
	     1) Several events may share delay_ms == 0 (an init burst).
	     2) ms_elapsed could exceed a single event's delay if the
	        caller ticks at a coarser cadence than the song's finest
	        grain. */
	while (g_due_ms <= 0) {
		if (g_pos >= g_count) {
			if (g_loop) {
				g_pos    = 0;
				g_due_ms = 0;
				continue;

			} else {
				g_playing = 0;
				return;
			}
		}

		const opl_event* e = &g_events[g_pos++];
		OPL3_WriteRegBuffered(&g_chip, e->reg, e->val);

		/* Visualizer hook: report rhythm-mode percussion strike bits. */
		if (g_key_cb && (e->reg & 0xff) >= 0xB0 && (e->reg & 0xff) <= 0xB8)
			g_key_cb(e->val & 0x1f);

		//another variant?
		//g_key_cb((e->val & 0x20) ? 1 : 0);

		g_due_ms += (int32_t)e->delay_ms;
	}
}

/*  ====================================================================
    Audio producer (low-priority context)
    ==================================================================== */

__attribute__((section(".ccm"))) void
synth_update()
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

	while (FIFO_FREECOUNT(&g_fifo) >= SYNTH_FRAME_BYTES) {
		int16_t f[2];
		OPL3_Generate(&g_chip, f);  /* f[0] = L, f[1] = R */

		/*  Push the whole stereo frame in one buffered call.  The
		    underlying fifo cnt+=len is non-atomic vs. the ISR's
		    cnt-=len, so we gate IRQs around the put.  The window is
		    tiny (a memcpy of 4 bytes + a counter add). */
		__disable_irq();
		(void)fifo_put_buf(&g_fifo, (uint8_t*)f, SYNTH_FRAME_BYTES);
		__enable_irq();

		if (timeout == 0)
			break;

		timeout--;
	}
}

/*  ====================================================================
    Audio consumer (high-priority audio ISR)
    ==================================================================== */

__attribute__((section(".ccm"))) void
synth_render_sample(int16_t* out_l, int16_t* out_r)
{
	/*  Pop one stereo frame.  On underrun (producer fell behind) hold
	    the previous frame to avoid the click that a hard zero would
	    produce.  This is preferable to silence because typical
	    underruns are single-sample; a held value is sub-LSB different
	    from the missed sample. */
	int16_t f[2];

	if (fifo_get_buf(&g_fifo, (uint8_t*)f, SYNTH_FRAME_BYTES) < 0) {
		*out_l = g_last_l;
		*out_r = g_last_r;

	} else {
		g_last_l = f[0];
		g_last_r = f[1];
		*out_l   = f[0];
		*out_r   = f[1];
	}
}
