/*
    seq_player.h — minimal MCU-friendly OPL sequence player.

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
    For mono output, mix:  out = (l + r) >> 1; */
void synth_render_sample(int16_t* out_l, int16_t* out_r);

#ifdef __cplusplus
}
#endif
#endif /* SEQ_PLAYER_H */
