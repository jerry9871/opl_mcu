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

    No malloc, no stdio, no FILE*, no float math.
*/
#ifndef SEQ_PLAYER_H
#define SEQ_PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*  One OPL register-write event with the delay that follows it.

     reg      : OPL register address (low 9 bits).
                Bit 8 (i.e. value >= 0x100) addresses the OPL3 second port.
     val      : 8-bit value to write to that register.
     delay_ms : milliseconds to wait AFTER this write, before the next one.

    sizeof(opl_event) == 6 bytes (4 with #pragma pack on most ABIs).
*/
typedef struct {
	uint16_t reg;
	uint8_t  val;
	uint16_t delay_ms;
} opl_event;

/* ---- one-time initialization ---- */

/*  Initialize the OPL3 synth and the player state.
    Call once at boot. sample_rate_hz must equal the rate at which you
    will subsequently call synth_render_sample() (e.g. 20000 for 20 kHz). */
void synth_init(uint32_t sample_rate_hz);

/* ---- sequence control ---- */

/* Begin playback of a static song.  loop != 0 means restart at end. */
void seq_play(const opl_event* events, uint32_t count, int loop);

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
