/*
    Tiny abstract audio sink.

    The OPL synth core (Nuked-OPL3) is platform-independent. Anything that
    needs the OS (Windows WaveOut here, I2S DAC on a microcontroller later)
    sits behind this two-function interface.
*/
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

/*  Open a 16-bit signed stereo PCM sink.
    Returns 0 on success, non-zero on failure. */
int
audio_open(uint32_t sample_rate);

/*  Block until the given interleaved L/R sample frames have been queued
    for playback. `frames` is the number of stereo frames (= samples/2). */
void
audio_write(const int16_t* interleaved_lr, uint32_t frames);

/* Drain any queued audio and release the device. */
void
audio_close(void);

#endif
