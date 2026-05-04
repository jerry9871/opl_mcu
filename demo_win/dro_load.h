/*
    dro_load.h — load a DOSBox DRO v2 file at runtime into a
    malloc'd opl_song.  PC-only helper (uses stdio + malloc).  NOT
    for the MCU build.

    Heatshrink-packed songs are baked into the binary via
    `tools/dro2hdr --hs` (see songs/opl_song_hs.h for the descriptor
    type).  There is no runtime ".dro_hs*" file format -- one source
    of truth keeps the demo, and the docs, simple.
*/
#ifndef DRO_LOAD_H
#define DRO_LOAD_H

#include <stdint.h>
#include <stddef.h>
#include "seq_player.h"

/*  Load a plain DRO v2 file.  On success returns 0 and fills
     out_song (its `data` buffer is malloc'd).  Free with
    dro_song_free(). */
int
dro_load(const char* path, opl_song* out_song);

void
dro_song_free(opl_song* song);

#endif
