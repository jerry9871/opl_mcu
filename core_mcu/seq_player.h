/*
    seq_player.h — minimal MCU-friendly OPL2/OPL3 sequence player.

    What this module does
    ---------------------
    Plays a statically-compiled stream of OPL register-write events
    (`opl_event[]`) through the bundled Nuked-OPL3 emulator and exposes
    the rendered audio one stereo PCM frame at a time.  No malloc, no
    stdio, no FILE*, no float math.  One global chip + one current song.

    Pipeline
    --------

      sequencer tick (low priority,            audio ISR (high priority,
       e.g. 1 ms SysTick)                       e.g. PWM/DAC half-empty)
      ------------------------                 -------------------------
       seq_tick(ms_elapsed)                     synth_render_sample(&l,&r)
        - decrements next-event timer            - pops one stereo frame
        - flushes due register writes              from the FIFO
          via OPL3_WriteRegBuffered              - on underrun, holds the
                                                   previous frame
       synth_update()
        - tops up the sample FIFO with as
          many frames as fit (one OPL3 sample
          per Generate call)

      The synth ? ISR FIFO absorbs jitter so a single expensive
      seq_tick() can never starve the audio output.

    Typical wiring
    --------------
        // boot
        synth_init(SAMPLE_RATE_HZ);
        seq_play(my_song, ARRAY_SIZE(my_song), 1);  // loop

        // 1 ms periodic context (SysTick / RTOS task)
        seq_tick(1);
        synth_update();

        // audio output ISR (PWM / I2S DMA half-/full-complete)
        int16_t l, r;
        synth_render_sample(&l, &r);
        // route l, r to your DACs/PWMs

    Files to compile/link on the microcontroller:
        opl3.c           the synthesizer
        seq_player.c     this player + FIFO glue
        fifo.c           generic byte FIFO from lib/_LIB
        <song_data>.h    your embedded opl_event[] (e.g. ww_theme_song.h)

    Threading
    ---------
    Designed for single-core MCUs where:
        seq_tick() + synth_update() run in a low-priority context (task
        or low-priority IRQ).
        synth_render_sample() runs in a high-priority audio IRQ.
    The producer (synth_update) guards its FIFO write with
    __disable_irq()/__enable_irq() because the underlying byte FIFO uses
    non-atomic counter updates.  The audio ISR pops without further
    guarding (it cannot be preempted by the producer).
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

/* ---- one-time initialization ---------------------------------------- */

/*  Initialize the OPL3 chip, reset the player state and clear the
    sample FIFO.  Call once at boot.  `sample_rate_hz` must match the
    rate at which synth_render_sample() will subsequently be called
    (e.g. 24858 for the FALCON build). */
void synth_init(uint32_t sample_rate_hz);

/* ---- sequence control ----------------------------------------------- */

/*  Begin playback of a static song (typically a const array embedded
    from a generated header).  `loop != 0` restarts at the end. */
void seq_play(const opl_event* events, uint32_t count, int loop);

/*  Stop advancing the sequencer.  Existing OPL register state is left
    as-is so any still-sounding notes will release naturally.  Call
    seq_silence() afterwards to forcibly key-off everything. */
void seq_stop(void);

/*  Force key-off on all 18 OPL3 channels (both ports) and disable the
    rhythm part.  Useful before seq_play() to avoid bleed-over from a
    previous song. */
void seq_silence(void);

/* Returns non-zero while a song is still playing. */
int  seq_is_playing(void);

/*  Optional percussion-strike hook.  Called from seq_tick() context for
    every write to OPL register 0xBD (rhythm mode); `perc` carries the
    low 5 strike bits (bd/sd/tom/tc/hh).  Useful to drive a visualizer
    LED.  Pass NULL to disable. */
typedef void (*seq_key_cb)(uint8_t perc);
void seq_set_key_cb(seq_key_cb cb);

/* ---- real-time entry points ----------------------------------------- */

/*  Advance the sequencer by `ms_elapsed` milliseconds and dispatch any
    OPL register writes whose deadline has been reached.  Call from a
    periodic low-priority source (1 ms timer is ideal).  Cheap: O(events
    fired); typically zero or one per call.  Does NOT render audio — see
    synth_update(). */
void seq_tick(uint32_t ms_elapsed);

/*  Render OPL3 audio into the sample FIFO until it is full (or an
    internal safety cap is hit).  Call from the same low-priority
    context as seq_tick(), right after it.  This is where the heavy
    OPL3 cost lives; it is intentionally separated from the audio ISR
    so the ISR never blocks on it. */
void synth_update(void);

/*  Pop one stereo PCM frame at the rate passed to synth_init().  Call
    from your DAC / PWM / I2S audio IRQ.  On FIFO underrun the previous
    frame is held to avoid clicks.  For mono output, mix the channels
    yourself: `mono = (l + r) >> 1;`. */
void synth_render_sample(int16_t* out_l, int16_t* out_r);

#ifdef __cplusplus
}
#endif
#endif /* SEQ_PLAYER_H */
