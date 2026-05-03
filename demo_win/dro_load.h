/*
    dro_load.h — load a DOSBox DRO v2 file (raw or heatshrink-packed)
    at runtime into a malloc'd opl_song.  PC-only helper (uses stdio +
    malloc).  NOT for the MCU build.

    Also defines opl_song_hs, the descriptor that `dro2hdr --hs`
    emits.  That descriptor is just data: a flash pointer to a
    heatshrink-compressed DRO v2 payload plus the metadata needed to
    drive playback without first decompressing the file header.  The
    MCU consumes opl_song_hs together with hs_stream (../hs_stream.h)
    and seq_play_stream() (no malloc, no stdio).
*/
#ifndef DRO_LOAD_H
#define DRO_LOAD_H

#include <stdint.h>
#include <stddef.h>
#include "seq_player.h"
#include "opl_song_hs.h"

/*  Load a plain DRO v2 file (no compression).  On success returns 0
    and fills *out_song (its `data` buffer is malloc'd).  Free with
    dro_song_free().  Returns non-zero (and prints) for ".dro_hs*"
    files — use dro_load_hs() for those. */
int
dro_load(const char* path, opl_song* out_song);

void
dro_song_free(opl_song* song);

/*  Load a heatshrink-packed ".dro_hs<W>[l<L>]" file as an
    opl_song.  PC-only convenience: fully decompresses the file,
    strips the 26-byte DRO v2 header, and returns the codemap +
    opcode stream as a flat malloc'd buffer.  Free with
    dro_song_free() (same as plain DRO loads).

    The MCU build doesn't use this — embedded songs are emitted as
    opl_song_hs (header-stripped at compile time by `dro2hdr --hs`)
    and consumed via hs_stream + seq_play_stream() with no malloc. */
int
dro_load_hs(const char* path, opl_song* out_song);

#endif
