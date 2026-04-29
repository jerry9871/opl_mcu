/*
    dro_load.h — load a DOSBox DRO v2 file at runtime into a malloc'd
    opl_song (packed format).  PC-only helper (uses stdio + malloc).
    NOT for the MCU build.
*/
#ifndef DRO_LOAD_H
#define DRO_LOAD_H

#include <stdint.h>
#include "seq_player.h"

/*  Load a DRO v2 file.  On success returns 0 and fills *out_song
    (its `data` buffer is malloc'd; free both with dro_song_free()).
    On failure returns non-zero and prints to stderr. */
int
dro_load(const char* path, opl_song* out_song);
void
dro_song_free(opl_song* song);

#endif
