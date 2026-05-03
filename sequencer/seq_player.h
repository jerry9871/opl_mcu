/*
    seq_player.h — minimal MCU-friendly OPL sequence player.

    Core-agnostic: depends only on the small `OPL3_*` surface declared
    in <opl3.h> (Reset / WriteReg / WriteRegBuffered / GenerateResampled).
    Pair this single .c/.h with whichever OPL emulator folder you
    prefer (nuked/, opal/, ...) by adding -I<core> to the compile
    command.  See the README for the supported cores.

    Architecture (microcontroller layout):

     +-------------------------+      every ~1 ms (timer IRQ / SysTick)
     |  seq_tick(ms_elapsed)   |  <----------------------------------+
     |  - decrements timer     |                                     |
     |  - writes pending OPL   |                                     |
     |    registers via the    |                                     |
     |    Nuked-OPL3 core      |                                     |
     +-------------------------+                                     |
                                                                     |
     +-------------------------+      every 1/sample_rate s          |
     | synth_render_sample(*l, |  <----------------------------------+
     |                    *r)  |  (DAC / I2S DMA-half-empty IRQ)
     |  - one stereo frame     |
     +-------------------------+

    The OPL3 emulator is the *only* expensive part. seq_tick() is O(1)
    amortized (just a counter compare + an OPL_WriteReg per event).

    Files to compile/link on the microcontroller:
        opl3.c           (the synthesizer)
        seq_player.c     (this player)
        <song_data>.h    (your embedded sequence, e.g. pre2_loop_song.h)
*/
#ifndef SEQ_PLAYER_H
#define SEQ_PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*  Packed/compact song format (~2 bytes per register write, mirrors the
    DRO v2 wire format).

    Layout of `data[]`:
      - first `codemap_len` bytes: a code -> OPL register-low-byte map.
        A code C in the opcode stream resolves to register address
          codemap[C & 0x7F] | ((C & 0x80) ? 0x100 : 0)
        (high bit of C selects the OPL3 second port).
      - the rest:  (code, val) pairs.
          code == short_code  ->  delay = val + 1   ms
          code == long_code   ->  delay = (val+1)*256 ms
          else                ->  write `val` to the register decoded above
*/
typedef struct {
	const uint8_t* data;        /* codemap (codemap_len bytes) + opcode stream */
	uint32_t       data_len;    /* total bytes in `data[]`                     */
	uint16_t       codemap_len; /* number of codemap entries (<= 128)          */
	uint8_t        short_code;  /* opcode value used for short delays          */
	uint8_t        long_code;   /* opcode value used for long delays           */
	uint32_t       total_ms;    /* nominal song length, for diagnostics        */
} opl_song;

/*  Streaming variant of opl_song: bytes are pulled on demand from a
    caller-provided callback rather than read from a flat buffer in
    addressable memory.  Useful when the song lives behind a
    decompressor (e.g. heatshrink) or in non-memory-mapped flash.

    The byte stream MUST be exactly what `opl_song.data` would be:
    `codemap_len` codemap bytes followed by the (code,val) opcode
    stream.  The player reads the codemap once at play start and
    caches it (128 bytes) so codemap[] lookups during playback don't
    require seeking.

    next_byte()  : returns the next byte 0..255, or -1 on EOF/error.
    rewind()     : restart the stream at byte 0.  Required iff loop=1.
    user         : opaque context pointer passed to both callbacks.   */
typedef struct opl_song_stream {
	int (*next_byte)(void* user);
	void (*rewind)(void* user);
	void*  user;

	uint16_t codemap_len;
	uint8_t  short_code;
	uint8_t  long_code;
	uint32_t total_ms;
} opl_song_stream;

/*  Begin playback of a streamed song.  Same semantics as
    seq_play_song(), but pulls bytes from the callback.  The stream
    descriptor must outlive playback (the player keeps a pointer). */
void seq_play_stream(const opl_song_stream* stream, int loop);

/* ---- one-time initialization ---- */

/*  Initialize the OPL3 synth and the player state.
    Call once at boot. sample_rate_hz must equal the rate at which you
    will subsequently call synth_render_sample() (e.g. 20000 for 20 kHz). */
void synth_init(uint32_t sample_rate_hz);

/* ---- sequence control ---- */

/*  Begin playback of a packed song (typically from a generated header).
    loop != 0 means restart at end. */
void seq_play_song(const opl_song* song, int loop);

/*  Stop playback.  Existing OPL register state is left as-is so any
    still-sounding notes will release naturally; call seq_silence() to
    forcibly key-off everything. */
void seq_stop(void);

/* Force key-off on all 18 OPL3 channels (and the rhythm part). */
void seq_silence(void);

/* Returns non-zero while a song is still playing. */
int  seq_is_playing(void);

/* ---- the two real-time entry points ---- */

/*  Advance the sequencer by `ms_elapsed` milliseconds.
    Call this from a periodic source (1 ms timer is ideal; larger values
    also work but reduce timing precision).  ISR-safe with respect to
    synth_render_sample() as long as both run on the same core: they share
    only the OPL3 chip state, and the writes happen here while the reads
    happen there. */
void seq_tick(uint32_t ms_elapsed);

/*  Produce one stereo PCM frame at the rate passed to synth_init().
    Call from your DAC / I2S half-buffer IRQ.
    For mono output, mix:  out = (l + r) >> 1;

    Inline path: renders one frame on the spot via
    OPL3_GenerateResampled().  No FIFO; the producer and consumer
    are the same thread.  This is what the PC demo uses and is also
    fine on an MCU if your audio IRQ has the cycles.

    For an alternative producer/consumer split (heavy synth runs in
    a low-priority context, light ISR pops one ready frame), see
    synth_update_fifo() / synth_get_fifo_sample() below.  Both APIs
    are always compiled; pick whichever you call from your audio
    IRQ and ignore the other. */
void synth_render_sample(int16_t* out_l, int16_t* out_r);

/*  -- Optional FIFO path -------------------------------------------
    A small lock-free SPSC ring of stereo frames lives inside
    seq_player.c.  The producer (synth_update_fifo) and the consumer
    (synth_get_fifo_sample) must each be called from a single
    context, but the producer and consumer contexts can differ
    (typical MCU split: producer in the sequencer task, consumer in
    the DAC ISR).  No interrupt guards or CMSIS dependencies. */

/*  Render OPL3 audio into the FIFO until it is full (or an internal
    safety cap is hit).  Call from the same low-priority context as
    seq_tick(), right after it. */
void synth_update_fifo(void);

/*  Pop one stereo frame from the FIFO.  On underrun (producer fell
    behind) the previous frame is held to avoid clicks. */
void synth_get_fifo_sample(int16_t* out_l, int16_t* out_r);

/*  Optional percussion-strike hook.  Called from seq_tick() context
    every time a key-on/off byte is dispatched (registers 0xB0..0xB8).
    Useful to drive a visualizer LED.  Pass NULL to disable. */
typedef void (*seq_key_cb)(uint8_t key_byte);
void seq_set_key_cb(seq_key_cb cb);

#ifdef __cplusplus
}
#endif
#endif /* SEQ_PLAYER_H */
