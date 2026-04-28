/*
    seq_player.c — implementation of the tick/render API declared in
    seq_player.h.

    Single-instance: one global OPL3 chip and one current song.  This is
    exactly what you want on a microcontroller; multi-song mixing is out
    of scope here.
*/
#include "seq_player.h"
#include "opl3.h"

/* ---- module state -------------------------------------------------- */

static opl3_chip          g_chip;
static const opl_event*   g_events;
static uint32_t           g_count;
static uint32_t           g_pos;
static int32_t            g_due_ms;     /* ms until next event fires    */
static uint8_t            g_loop;
static uint8_t            g_playing;

/* ---- public API ---------------------------------------------------- */

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
}

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

	/*  Fire every event whose deadline has been reached.  The while loop
	    matters for two reasons:
	     1) Several events may share delay_ms == 0 (an init burst).
	     2) ms_elapsed could exceed a single event's delay if the caller
	        ticks at a coarser cadence than the song's finest grain. */
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
		g_due_ms += (int32_t)e->delay_ms;
	}
}

void
synth_render_sample(int16_t* out_l, int16_t* out_r)
{
	int16_t f[2];
	OPL3_GenerateResampled(&g_chip, f);
	*out_l = f[0];
	*out_r = f[1];
}
