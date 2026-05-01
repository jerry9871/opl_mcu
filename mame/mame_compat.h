/*
    mame_compat.h — minimal compatibility shim for vendoring FBNeo's
    ymf262.cpp (which is byte-identical to MAME's original
    ymf262.c by Jarek Burczynski / Tatsuyuki Satoh) as a standalone
    pure-C source file.

    Provides:
      - Width-tagged integer typedefs that MAME's source expects
        (UINT8/16/32, INT8/16/32).  We piggy-back on <stdint.h>.
      - No-op stubs for FBNeo's save-state machinery (SCAN_VAR /
        ACB_DRIVER_DATA / ACB_WRITE).  This repo doesn't use save
        states; the surrounding `if (nAction & ACB_*)` blocks
        compile and dead-code-eliminate down to nothing.
      - logerror() as a no-op so the upstream diagnostics compile.

    Nothing here changes the audio path; the chip math is unmodified.
*/
#ifndef MAME_COMPAT_H
#define MAME_COMPAT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*  M_PI is non-standard; provide it for sources that expect it.    */
#ifndef M_PI
	#define M_PI 3.14159265358979323846
#endif

typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef int8_t    INT8;
typedef int16_t   INT16;
typedef int32_t   INT32;

/*  No-op save-state surface.  ACB_DRIVER_DATA / ACB_WRITE are bit
    flags in nAction; setting them to 0 means the corresponding
    `if (nAction & ACB_*)` blocks are never entered.                */
#define SCAN_VAR(x)         do { (void)(x); } while (0)
#define ACB_DRIVER_DATA     0
#define ACB_WRITE           0

/*  MAME-style diagnostics: silently dropped.                       */
#define logerror(...)       do { } while (0)

/*  MAME's source uses `INLINE` rather than the C99 `inline`
    keyword.  We make it a `static inline` so each TU emits its own
    definition (matches the upstream gcc build behaviour).          */
#ifndef INLINE
	#define INLINE              static inline
#endif

/*  The vendored ymf262.c declares its structs with C++-style
    "struct Foo { ... };" then uses `Foo` as a type name elsewhere.
    Forward-typedef them so plain C99 accepts that.                 */
typedef struct OPL3_SLOT    OPL3_SLOT;
typedef struct OPL3_CH      OPL3_CH;
typedef struct OPL3         OPL3;

#endif
