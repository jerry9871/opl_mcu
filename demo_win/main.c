/*
    main.c — Windows demo that exercises the *exact MCU API* of
    seq_player.h.  The only Windows-specific thing here is the audio sink
    (audio_win.c).  Replace audio_open/write/close with your DAC/I2S setup
    and this same main loop runs unchanged on a microcontroller.

    Built-in songs (compiled in from songs):
      melody     -- "Ode to Joy" (Beethoven, public domain)  [DEFAULT]
      pre2       -- Prehistorik 2 title music  (~102 KB flash, looping)
      ww_intro   -- Wacky Wheels intro         (~6  KB flash)
      ww_theme   -- Wacky Wheels theme         (~26 KB flash)
      doom       -- DOOM setup utility music   (~86 KB flash, looping)

    Build (MinGW gcc): see Makefile / build.bat.

    Usage:
       opl_demo.exe                    -- plays the Ode to Joy demo (default)
       opl_demo.exe <name>             -- plays a named built-in song
       opl_demo.exe <file.dro>         -- plays any DOSBox DRO v2 capture
       opl_demo.exe ... --loop         -- loop forever  (Ctrl+C to stop)
       opl_demo.exe ... --once         -- play once and exit
       opl_demo.exe --list             -- list built-in song names
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seq_player.h"
#include "audio.h"
#include "dro_load.h"
#include "pre2_loop_song.h"
#include "ww_intro_song.h"
#include "ww_theme_song.h"
#include "doom_setup_song.h"

/* alternate built-in song (test_melody.c) */
extern const opl_event opl_demo_melody[];
extern const uint32_t  opl_demo_melody_count;

#define SAMPLE_RATE   22050   /* a microcontroller would use 20000 here  */
#define TICK_MS       1       /* sequencer tick granularity              */

typedef struct {
	const char*       name;
	const char*       desc;
	const opl_event*  events;
	uint32_t          count;
	int               default_loop;
} song_t;

static const song_t SONGS[] = {
	{
		"melody",   "\"Ode to Joy\" (Beethoven, PD)",
		/* events */ NULL, /* count */ 0, /* loop */ 0
	},   /* patched at runtime */
	{
		"pre2",     "Prehistorik 2 title",
		pre2_loop_song,  0, 1
	},                            /* count patched below */
	{
		"ww_intro", "Wacky Wheels intro",
		ww_intro_song,   0, 0
	},
	{
		"ww_theme", "Wacky Wheels theme",
		ww_theme_song,   0, 1
	},
	{
		"doom",     "DOOM setup music",
		opl_doom_setup_song, 0, 1
	},
};
#define N_SONGS ((int)(sizeof(SONGS)/sizeof(SONGS[0])))

static void
list_songs(void)
{
	printf("Built-in songs:\n");

	for (int i = 0; i < N_SONGS; i++)
		printf("  %-10s %s\n", SONGS[i].name, SONGS[i].desc);
}

int
main(int argc, char** argv)
{
	/* Patch in the counts that aren't compile-time constants here. */
	song_t songs[N_SONGS];
	memcpy(songs, SONGS, sizeof(songs));
	songs[0].events = opl_demo_melody;
	songs[0].count = opl_demo_melody_count;
	songs[1].count  = pre2_loop_song_count;
	songs[2].count  = ww_intro_song_count;
	songs[3].count  = ww_theme_song_count;
	songs[4].count  = opl_doom_setup_song_count;

	int loop_explicit = -1;          /* -1 = use song's default */
	const char* arg   = NULL;        /* song name OR .dro path  */

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--once"))
			loop_explicit = 0;

		else if (!strcmp(argv[i], "--loop"))
			loop_explicit = 1;

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
		picked = &songs[0];          /* default: melody */

	} else {
		for (int i = 0; i < N_SONGS; i++)
			if (!strcmp(arg, songs[i].name)) {
				picked = &songs[i];
				break;
			}

		if (!picked)
			dro = arg;               /* treat as DRO file path */
	}

	int loop = (loop_explicit >= 0)
			   ? loop_explicit
			   : (picked ? picked->default_loop : 0);

	printf("OPL3 demo (Nuked-OPL3 + tick/render API)\n");
	printf("  sample rate : %d Hz\n", SAMPLE_RATE);
	printf("  tick rate   : %d ms\n", TICK_MS);

	if (audio_open(SAMPLE_RATE) != 0) {
		fprintf(stderr, "Could not open audio device.\n");
		return 1;
	}

	synth_init(SAMPLE_RATE);

	opl_event* dro_events = NULL;    /* owned only when a .dro is used */

	if (dro) {
		uint32_t n, ms;

		if (dro_load(dro, &dro_events, &n, &ms) != 0) {
			audio_close();
			return 1;
		}

		printf("  song        : %s (%u events, %u ms)\n", dro, n, ms);
		printf("  loop        : %s\n", loop ? "yes (Ctrl+C to stop)" : "no");
		seq_play(dro_events, n, loop);

	} else {
		printf("  song        : %s (%s, %u events)\n",
			   picked->name, picked->desc, picked->count);
		printf("  loop        : %s\n", loop ? "yes (Ctrl+C to stop)" : "no");
		seq_play(picked->events, picked->count, loop);
	}

	/*  ---- the canonical real-time loop ---------------------------------

	    On the MCU this whole block is replaced by:

	     void timer_isr_1ms(void) { seq_tick(1); }
	     void dac_isr_at_SR(void) { int16_t l, r;
	                                synth_render_sample(&l, &r);
	                                dac_write(l, r); }

	    Here we fake both ISRs cooperatively: every iteration of the loop
	    renders exactly TICK_MS milliseconds of audio (= SR/1000 frames),
	    then advances the sequencer by one tick.  audio_write() blocks
	    naturally to keep wall-clock pace.
	    --------------------------------------------------------------- */
	enum { FRAMES_PER_TICK = (SAMPLE_RATE * TICK_MS) / 1000 };
	int16_t pcm[FRAMES_PER_TICK * 2];

	while (seq_is_playing()) {
		for (int i = 0; i < FRAMES_PER_TICK; i++)
			synth_render_sample(&pcm[2 * i + 0], &pcm[2 * i + 1]);

		audio_write(pcm, FRAMES_PER_TICK);
		seq_tick(TICK_MS);
	}

	audio_close();
	free(dro_events);
	printf("Done.\n");
	return 0;
}
