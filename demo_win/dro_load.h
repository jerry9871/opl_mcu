/*
    dro_load.h — load a DOSBox DRO v2 file at runtime into a malloc'd
    array of opl_event.  PC-only helper (uses stdio + malloc).  NOT
    for the MCU build.
*/
#ifndef DRO_LOAD_H
#define DRO_LOAD_H

#include <stdint.h>
#include "seq_player.h"

/*  Load a DRO v2 file.  On success returns 0 and fills *out_events
    (malloc'd, caller frees), *out_count and *out_total_ms.  On failure
    returns non-zero and prints to stderr. */
int
dro_load(const char* path,
		 opl_event** out_events,
		 uint32_t*   out_count,
		 uint32_t*   out_total_ms);

#endif
