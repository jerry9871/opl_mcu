/*
    main.c — Windows demo that exercises the *exact MCU API* of
    seq_player.h.  The only Windows-specific thing here is the audio sink
    (audio_win.c).  Replace audio_open/write/close with your DAC/I2S setup
    and this same main loop runs unchanged on a microcontroller.

    Built-in songs (compiled in from songs):
      pre2       -- Prehistorik 2 title music  (~41 KB flash, looping)  [DEFAULT]
      ww_intro   -- Wacky Wheels intro         (~2.5 KB flash)
      ww_theme   -- Wacky Wheels theme         (~10 KB flash)
      doom       -- DOOM setup utility music   (~34 KB flash, looping)

    Build (MinGW gcc): see Makefile / build.bat.

    Usage:
       opl_demo_<core>.exe                    -- plays the default built-in song
       opl_demo_<core>.exe <name>             -- plays a named built-in song
       opl_demo_<core>.exe <file.dro>         -- plays any DOSBox DRO v2 capture
       opl_demo_<core>.exe ... --loop         -- loop forever  (Ctrl+C to stop)
       opl_demo_<core>.exe ... --once         -- play once and exit
       opl_demo_<core>.exe ... --bench [N]    -- render N s of song with no audio,
                                                 print render time / CPU%% (no real-time pacing)
       opl_demo_<core>.exe --list             -- list built-in song names
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "seq_player.h"
#include "audio.h"
#include "dro_load.h"
#include "../heatshrink/heatshrink_decoder.h"
#include "../heatshrink/hs_stream.h"

/*  ---- songs shipped with the demo ----------------------------------
    Each *_song.h is self-describing: it includes opl_song_hs.h and
    declares a `static const opl_song_hs <symbol>` whose fields carry
    everything the player needs (compressed payload, total length,
    codemap length, short/long opcodes, heatshrink window/lookahead).
    The list below is just a curated short-name -> descriptor map. */
#include "eric_prydz_call_on_me_song.h"
#include "pre2_loop_song.h"
#include "ww_intro_song.h"
#include "ww_theme_song.h"
#include "doom_loop_song.h"
#include "metallica_master_of_puppets_song.h"
#include "deadmau5_ghosts_n_stuff_song.h"
#include "tony_igy_astronomia_song.h"
#include "atb_9_pm_till_i_come_song.h"

#define SAMPLE_RATE   49716   /* OPL3 native rate -- no resampling */
#define TICK_MS       1       /* sequencer tick granularity              */

typedef struct {
	const char*        name;
	const char*        desc;
	const opl_song_hs* song_hs;
	int                default_loop;
} song_t;

static const song_t SONGS[] = {
	{ "eric",      "Eric Prydz - Call on Me",          &eric_prydz_call_on_me,        0 },
	{ "pre2",      "Prehistorik 2 title",              &pre2_loop,                    1 },
	{ "ww_intro",  "Wacky Wheels intro",               &ww_intro,                     0 },
	{ "ww_theme",  "Wacky Wheels theme",               &ww_theme,                     1 },
	{ "doom",      "DOOM E1M1",                        &doom_loop,                    1 },
	{ "metallica", "Metallica - Master of Puppets",    &metallica_master_of_puppets,  0 },
	{ "deadmau5",  "deadmau5 - Ghosts'n'Stuff",        &deadmau5_ghosts_n_stuff,      0 },
	{ "astronomia", "Tony Igy - Astronomia",            &tony_igy_astronomia,          0 },
	{ "atb",       "ATB - 9 PM (Till I Come)",         &atb_9_pm_till_i_come,         0 },
};
#define N_SONGS ((int)(sizeof(SONGS)/sizeof(SONGS[0])))

static void
list_songs(void)
{
	printf("Built-in songs:\n");

	for (int i = 0; i < N_SONGS; i++)
		printf("  %-12s %s\n", SONGS[i].name, SONGS[i].desc);
}

/*  ---- adapter: hs_stream  ->  opl_song_stream ---------------------
    Trivial wrapping of the generic byte source (hs_stream) into the
    callback contract that seq_play_stream() expects.  On the MCU,
    write the same six lines next to your song-launch code -- it's
    intentionally not packaged as a library, so each project owns its
    decoder lifetime and can substitute a different byte source. */
static hs_stream        g_hs;
static opl_song_stream  g_opl_stream;

static int
hs_byte_cb(void* user)
{
	return hs_stream_next((hs_stream*)user);
}

static void
hs_rewind_cb(void* user)
{
	hs_stream* st = (hs_stream*)user;

	hs_stream_rewind(st);

	/* re-skip the 26-byte DRO file header on each rewind */
	for (int i = 0; i < 26; i++)
		(void)hs_stream_next(st);
}

/*  Wire an opl_song_hs through hs_stream into seq_play_stream().
    `hs_dec` is consumed and remembered; the caller is responsible for
    keeping `desc` alive for as long as playback runs.  Returns 0 on
    success, non-zero if the decoder allocation failed. */
static int
start_hs_playback(const opl_song_hs* desc,
				  heatshrink_decoder** hs_dec_out, int loop)
{
	heatshrink_decoder* dec =
		heatshrink_decoder_alloc(64, desc->hs_window, desc->hs_lookahead);

	if (!dec)
		return -1;

	*hs_dec_out = dec;

	hs_stream_init(&g_hs, desc->hs_data, desc->hs_len, dec);

	/* skip the 26-byte DRO file header */
	for (int i = 0; i < 26; i++)
		(void)hs_stream_next(&g_hs);

	g_opl_stream.next_byte   = hs_byte_cb;
	g_opl_stream.rewind      = hs_rewind_cb;
	g_opl_stream.user        = &g_hs;
	g_opl_stream.codemap_len = desc->codemap_len;
	g_opl_stream.short_code  = desc->short_code;
	g_opl_stream.long_code   = desc->long_code;
	g_opl_stream.total_ms    = desc->total_ms;

	seq_play_stream(&g_opl_stream, loop);
	return 0;
}

int
main(int argc, char** argv)
{
	int loop_explicit = -1;          /* -1 = use song's default */
	const char* arg   = NULL;        /* song name OR .dro path  */
	int  bench_secs   = 0;           /* 0 = real-time playback; >0 = benchmark */

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--once"))
			loop_explicit = 0;

		else if (!strcmp(argv[i], "--loop"))
			loop_explicit = 1;

		else if (!strcmp(argv[i], "--bench")) {
			bench_secs = 30;

			/* optional integer argument: --bench N */
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
				bench_secs = atoi(argv[++i]);

				if (bench_secs <= 0)
					bench_secs = 30;
			}

			loop_explicit = 1;   /* benchmark always loops; we time-cap it */
		}

		else if (!strcmp(argv[i], "--list")) {
			list_songs();
			return 0;

		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			printf("usage: %s [<name>|<file.dro>] [--once|--loop]\n",
				   argv[0]);
			list_songs();
			return 0;

		} else if (argv[i][0] != '-') {
			if (arg) {
				fprintf(stderr, "only one song allowed\n");
				return 1;
			}

			arg = argv[i];

		} else {
			fprintf(stderr, "unknown arg: %s\n", argv[i]);
			return 1;
		}
	}

	/* Resolve `arg` to either a built-in song or a DRO file path. */
	const song_t* picked = NULL;
	const char*   dro    = NULL;

	if (arg == NULL) {
		picked = &SONGS[0];          /* default: pre2 */

	} else {
		for (int i = 0; i < N_SONGS; i++)
			if (!strcmp(arg, SONGS[i].name)) {
				picked = &SONGS[i];
				break;
			}

		if (!picked)
			dro = arg;               /* treat as DRO file path */
	}

	int loop = (loop_explicit >= 0)
			   ? loop_explicit
			   : (picked ? picked->default_loop : 0);

	#ifndef CORE_NAME
#define CORE_NAME "unknown"
	#endif
	printf("OPL3 demo (core: %s, tick/render API)\n", CORE_NAME);
	printf("  sample rate : %d Hz\n", SAMPLE_RATE);
	printf("  tick rate   : %d ms\n", TICK_MS);

	if (!bench_secs && audio_open(SAMPLE_RATE) != 0) {
		fprintf(stderr, "Could not open audio device.\n");
		return 1;
	}

	if (bench_secs)
		printf("  mode        : BENCHMARK (%d s of song, no audio out)\n",
			   bench_secs);

	synth_init(SAMPLE_RATE);

	opl_song dro_song = {0};
	int      dro_song_loaded = 0;

	opl_song_hs file_hs   = {0};
	uint8_t*    file_hs_buf = NULL;

	heatshrink_decoder* hs_dec = NULL;

	if (dro) {
		/*  Direct file path on the command line.  Two cases:
		       .dro_hs* ? stream via hs_stream + seq_play_stream
		                  (same path the MCU uses).
		       .dro     ? flat buffer + seq_play_song. */
		const char* ext = strrchr(dro, '.');
		int is_hs = ext && strncmp(ext, ".dro_hs", 7) == 0;

		if (is_hs) {
			if (dro_load_hs(dro, &file_hs, &file_hs_buf) != 0) {
				audio_close();
				return 1;
			}

			printf("  song        : %s (%u B compressed, %u ms, hs=w%d/l%d)\n",
				   dro, file_hs.hs_len, file_hs.total_ms,
				   file_hs.hs_window, file_hs.hs_lookahead);
			printf("  loop        : %s\n",
				   loop ? "yes (Ctrl+C to stop)" : "no");

			if (start_hs_playback(&file_hs, &hs_dec, loop) != 0) {
				fprintf(stderr, "failed to alloc hs decoder\n");
				dro_load_hs_free(&file_hs, file_hs_buf);
				audio_close();
				return 1;
			}

		} else {
			if (dro_load(dro, &dro_song) != 0) {
				audio_close();
				return 1;
			}

			dro_song_loaded = 1;
			printf("  song        : %s (%u bytes packed, %u ms)\n",
				   dro, dro_song.data_len, dro_song.total_ms);
			printf("  loop        : %s\n",
				   loop ? "yes (Ctrl+C to stop)" : "no");
			seq_play_song(&dro_song, loop);
		}

	} else if (picked->song_hs) {
		printf("  song        : %s (%s, %u B compressed, %u ms, hs=w%d/l%d)\n",
			   picked->name, picked->desc,
			   picked->song_hs->hs_len, picked->song_hs->total_ms,
			   picked->song_hs->hs_window, picked->song_hs->hs_lookahead);
		printf("  loop        : %s\n",
			   loop ? "yes (Ctrl+C to stop)" : "no");

		if (start_hs_playback(picked->song_hs, &hs_dec, loop) != 0) {
			fprintf(stderr, "failed to alloc hs decoder\n");
			audio_close();
			return 1;
		}

	} else {
		fprintf(stderr, "song '%s' has no payload\n", picked->name);
		audio_close();
		return 1;
	}

	enum { FRAMES_PER_TICK = (SAMPLE_RATE * TICK_MS) / 1000 };
	int16_t pcm[FRAMES_PER_TICK * 2];

	if (bench_secs) {
		/*  Benchmark: run the same render+tick loop as real-time
		    playback, but skip audio_write() and time the wall clock.
		    Measures *only* the cost of synth_render_sample +
		    seq_tick (i.e. the chip emulator + register dispatch),
		    which is exactly what would run inside the MCU's DAC ISR
		    and SysTick.                                              */
		const uint64_t total_ticks  = (uint64_t)bench_secs * 1000u / TICK_MS;
		const uint64_t total_frames = total_ticks * FRAMES_PER_TICK;

		LARGE_INTEGER qpf, t0, t1;
		QueryPerformanceFrequency(&qpf);
		QueryPerformanceCounter(&t0);

		for (uint64_t k = 0; k < total_ticks && seq_is_playing(); k++) {
			for (int i = 0; i < FRAMES_PER_TICK; i++)
				synth_render_sample(&pcm[2 * i + 0], &pcm[2 * i + 1]);

			seq_tick(TICK_MS);
		}

		QueryPerformanceCounter(&t1);

		double wall_s     = (double)(t1.QuadPart - t0.QuadPart) / (double)qpf.QuadPart;
		double song_s     = (double)bench_secs;
		double rt_factor  = song_s / wall_s;          /* >1 = faster than real-time */
		double ns_per_frm = (wall_s * 1.0e9) / (double)total_frames;
		double cpu_pct    = 100.0 / rt_factor;        /* % of one PC core needed at SR */

		printf("\n");
		printf("benchmark results (%s):\n", CORE_NAME);
		printf("  rendered      : %llu frames (%llu ticks)\n",
			   (unsigned long long)total_frames,
			   (unsigned long long)total_ticks);
		printf("  song time     : %.3f s @ %d Hz\n", song_s, SAMPLE_RATE);
		printf("  wall time     : %.3f s\n", wall_s);
		printf("  realtime ratio: %.1fx  (1.0x = exactly keeps up)\n", rt_factor);
		printf("  per frame     : %.1f ns  (host CPU)\n", ns_per_frm);
		printf("  CPU load      : %.2f%% of one core at %d Hz\n", cpu_pct, SAMPLE_RATE);
		printf("\n");
		printf("  rough Cortex-M scaling (very approximate, ignore caches):\n");
		double host_ghz = 3.5;   /* assume modern x64 ~3.5 GHz; just a yardstick */
		double m4_ghz   = 0.168; /* 168 MHz STM32F4 */
		double m7_ghz   = 0.480; /* 480 MHz STM32H7 */
		double scale_m4 = host_ghz / m4_ghz;
		double scale_m7 = host_ghz / m7_ghz;
		printf("    M4  @168 MHz: %.0f%% of one core (rough x%.0f)\n",
			   cpu_pct * scale_m4, scale_m4);
		printf("    M7  @480 MHz: %.0f%% of one core (rough x%.0f)\n",
			   cpu_pct * scale_m7, scale_m7);

	} else {
		/*  ---- the canonical real-time loop -------------------------

		    On the MCU this whole block is replaced by:

		     void timer_isr_1ms(void) { seq_tick(1); }
		     void dac_isr_at_SR(void) { int16_t l, r;
		                                synth_render_sample(&l, &r);
		                                dac_write(l, r); }

		    Here we fake both ISRs cooperatively.                     */
		while (seq_is_playing()) {
			for (int i = 0; i < FRAMES_PER_TICK; i++)
				synth_render_sample(&pcm[2 * i + 0], &pcm[2 * i + 1]);

			audio_write(pcm, FRAMES_PER_TICK);
			seq_tick(TICK_MS);
		}
	}

	if (!bench_secs)
		audio_close();

	if (dro_song_loaded)
		dro_song_free(&dro_song);

	if (file_hs_buf)
		dro_load_hs_free(&file_hs, file_hs_buf);

	if (hs_dec)
		heatshrink_decoder_free(hs_dec);

	printf("Done.\n");
	return 0;
}
