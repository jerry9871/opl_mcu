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

static const opl_song*        g_song;
static uint32_t               g_ppos;       /* byte offset into song->data, after codemap */

static const opl_song_stream* g_stream;
static uint8_t                g_codemap[128];
static int                    g_eof;

static int32_t           g_due_ms;     /* ms until next event fires    */
static uint8_t           g_loop;
static uint8_t           g_playing;

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
			OPL3_WriteRegBuffered(&g_chip, reg, val);
		}
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
