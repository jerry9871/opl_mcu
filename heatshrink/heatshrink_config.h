#ifndef HEATSHRINK_CONFIG_H
#define HEATSHRINK_CONFIG_H

/* Should functionality assuming dynamic allocation be used? */
#ifndef HEATSHRINK_DYNAMIC_ALLOC
	#define HEATSHRINK_DYNAMIC_ALLOC 1
#endif

#if HEATSHRINK_DYNAMIC_ALLOC
	/* Optional replacement of malloc/free */
	#define HEATSHRINK_MALLOC(SZ) malloc(SZ)
	#define HEATSHRINK_FREE(P, SZ) free(P)
#else
	/*  Required parameters for static configuration.
	All three are #ifndef-guarded so a project Makefile can
	override them via -D, and that override REACHES EVERY
	TRANSLATION UNIT THAT INCLUDES THIS FILE.

	Do not remove the guards.  Without them an unconditional
	`#define` here silently overrides any -D from the build
	system, leaving the decoder code with one window size and
	callers (anyone who declares `static heatshrink_decoder x;`)
	with another.  The struct embeds the window buffer at
	compile time, so the result is a per-TU struct-size
	mismatch: callers allocate a too-small instance and the
	decoder happily writes past its end into adjacent BSS.
	Symptom is a stream that decodes correctly for the first
	few hundred bytes (small backrefs stay inside the truncated
	window) and then degrades into garbage as soon as a longer
	backref is emitted.  Put the -D flags into your global
	CFLAGS, not a per-file rule. */
	#ifndef HEATSHRINK_STATIC_INPUT_BUFFER_SIZE
		#define HEATSHRINK_STATIC_INPUT_BUFFER_SIZE 32
	#endif
	#ifndef HEATSHRINK_STATIC_WINDOW_BITS
		#define HEATSHRINK_STATIC_WINDOW_BITS 8
	#endif
	#ifndef HEATSHRINK_STATIC_LOOKAHEAD_BITS
		#define HEATSHRINK_STATIC_LOOKAHEAD_BITS 4
	#endif
#endif

/* Turn on logging for debugging. */
#define HEATSHRINK_DEBUGGING_LOGS 0

/* Use indexing for faster compression. (This requires additional space.) */
#define HEATSHRINK_USE_INDEX 1

#endif
