/*
    adlibemu_compat.h - tiny shim that replaces three libvgm headers
    (stdtype.h, snddef.h, common_def.h) with self-contained C99
    typedefs.  Including this lets the upstream adlibemu_opl_inc.[ch]
    pair compile standalone.
*/
#ifndef ADLIBEMU_COMPAT_H
#define ADLIBEMU_COMPAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint8_t  UINT8;
typedef int8_t   INT8;
typedef uint16_t UINT16;
typedef int16_t  INT16;
typedef uint32_t UINT32;
typedef int32_t  INT32;
typedef uint64_t UINT64;
typedef int64_t  INT64;

/* DEV_SMPL = single audio sample, signed 32-bit. */
typedef INT32 DEV_SMPL;

/* libvgm has every chip struct embed a DEV_DATA as its first field
   so that libvgm's generic device dispatch can downcast.  We don't
   use that machinery; the field is harmless padding for us.        */
typedef struct {
    void* chipInf;
} DEV_DATA;

#ifndef INLINE
#define INLINE static __inline
#endif

#endif /* ADLIBEMU_COMPAT_H */
