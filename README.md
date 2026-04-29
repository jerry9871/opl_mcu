# OPL3 microcontroller synthesizer

A small, hackable, MCU-friendly **OPL3 (YMF262) synthesizer** with a
sequence-player API designed to fit comfortably on a Cortex-M / RP2040 /
ESP32 / similar microcontroller. Comes with a Windows demo wrapper that
exercises the **exact** API that the MCU port uses, so what you hear on
your PC is what your MCU will play.

> **Why?** For fun, and because the OPL3 (the FM synth chip in the
> Sound Blaster 16 / AdLib Gold) is a beautiful piece of late-80s
> hardware whose music — fast attack envelopes, gritty FM timbres, that
> distinctive ~50 kHz sample rate — is impossible to reproduce
> convincingly with any other technique. With ~100 KB of flash and one
> spare CPU you can have authentic OPL3 audio on a $3 board.

The included demo plays the title-screen music from the 1993 DOS game
**Prehistorik 2**, captured live from the original `PRE2.EXE` running
in DOSBox and converted to a `static const opl_event[]` array.

---

## Project layout

```
opl/
+-- nuked/                      <-- portable reference build (PC + MCU)
|   +-- opl3.c     opl3.h         Nuked-OPL3 v1.8 (LGPL 2.1+, unmodified)
|   +-- seq_player.c              Sequence player: tick + render API
|   +-- seq_player.h              Public interface -- read this file first
|
+-- nuked_optimized/            <-- MCU-optimized variant of the Nuked port
|   +-- opl3.c     opl3.h         Nuked-OPL3 v1.8 + heavy Cortex-M tweaks
|   +-- seq_player.c              Same API as nuked/, plus a sample FIFO
|   +-- seq_player.h              Adds synth_update() between tick and ISR
|
+-- songs/                      <-- + at least one of these
|   +-- pre2_loop_song.h          Prehistorik 2 title music (~102 KB flash)
|   +-- test_melody.c             Beethoven "Ode to Joy" (small example)
|
+-- demo_win/                   <-- Windows-only wrapper, NOT for the MCU
|   +-- main.c                    Fake-ISR loop calling seq_tick + render
|   +-- audio.h                   Tiny audio-sink interface
|   +-- audio_win.c               WinMM (waveOut) implementation
|
+-- tools/                      <-- offline PC utilities
|   +-- dro2hdr.c / .exe          DRO capture -> embeddable C header
|   +-- dro_loop3.c / .exe        Loop-point detector for DRO captures
|
+-- capture/                      DRO captures from DOSBox
+-- preh2/                        Prehistorik 2 game files (your copy)
+-- prehistorik2.conf             DOSBox config to recapture music
+-- Makefile / build.bat          Windows build
+-- README.md                     This file
```

## Two cores, one API

The repository keeps each OPL emulation core in its own folder so that
alternative engines (e.g. DOSBox `dbopl`) can be added side-by-side
without disturbing the others. Today there are two **Nuked-OPL3**
flavours:

- [`nuked/`](nuked/) is the **portable reference build** � byte-identical
  Nuked-OPL3 v1.8 plus a tiny sequencer. Use it on PC for
  bit-accurate playback, regression testing, and as the readable
  baseline against which `nuked_optimized/` can be diffed.
- [`nuked_optimized/`](nuked_optimized/) is the **MCU-optimized variant
  of the same Nuked port** � same `seq_player.h` / `opl3.h` API, but
  the synth has been heavily reworked for low-flash, low-cycle
  Cortex-M targets (active-slot index list, cached envelope/phase
  increments, fused L+R mix, CCM/RAM placement,
  `__SSAT`/`__USAT`/`__builtin_ctz` intrinsics, plus opt-in
  `OPL_MONO` / `OPL_FORCE_OPL2` / `OPL_MAX_CHANNELS` switches). All
  non-bit-exact compromises are listed in the banner at the top of
  [`nuked_optimized/opl3.c`](nuked_optimized/opl3.c). The sequencer
  adds a sample FIFO and a separate `synth_update()` call so the heavy
  OPL3 work runs in a low-priority context while the audio ISR just
  pops one frame.

Select which core the demo links against with `make CORE=nuked` (the
default) or `make CORE=nuked_optimized`.

Three things matter for the MCU build: **`nuked_optimized/`** (or
`nuked/` if you want the unmodified reference), **one song header from
`songs/`**, and your own audio sink. Everything else is PC tooling.

### Going further than `nuked_optimized/`

`nuked_optimized/` keeps the Nuked-OPL3 envelope/phase/operator
pipeline intact and just makes it cheaper. If you need _more_ headroom
on a slower MCU, the next step is to swap the synth engine entirely
for one that uses simpler maths. Worth knowing about, even if we don't
use it here yet:

- **DOSBox `dbopl`** &mdash; the OPL emulator that ships with DOSBox,
  by Peter "Wohlstand" / DOSBox team. Uses table-driven envelope
  generation and a much simpler operator loop than Nuked. Drastically
  cheaper per sample (often 3&ndash;5&times; faster on M-class cores)
  but **not bit-exact**: envelopes, key-on transients and some
  waveforms are audibly different. Fine for general FM playback,
  noticeable on percussion and short attack transients. Could live in
  a sibling folder such as `dbopl/` behind the same `seq_player.h` API.
- **Other simpler ones**: `ymfm` (Aaron Giles, MAME) trades some
  accuracy for speed too; `adlmidi`'s built-in `OPL3-emu` is also
  worth a look. None match Nuked's "decap-accurate" reputation, but
  all are lighter.

Rule of thumb: stick with `nuked_optimized/` unless your audio ISR is
overrunning _with_ `OPL_MAX_CHANNELS` capped low and `OPL_FORCE_OPL2`
enabled. If even that is too much, port `dbopl` behind the same
`seq_player.h` API and accept the audio compromises.

---

## Quick start (Windows)

Requires MinGW gcc (anything ≥ 8 works).

```
make                 (or:  build.bat)
opl_demo.exe         loop the Prehistorik 2 title music
opl_demo.exe --once  play it once and exit
opl_demo.exe --melody  play the Ode to Joy demo
```

---

## How it works

### Synth core

[`nuked/opl3.c`](nuked/opl3.c) and [`nuked/opl3.h`](nuked/opl3.h) are
**Nuked-OPL3 v1.8** by Nuke.YKT, taken byte-for-byte from
<https://github.com/nukeykt/Nuked-OPL3> (verified by SHA-256 against
the upstream `master` branch). It is a cycle-accurate, decap-derived
emulation of the YMF262, well-known and widely used.

Two functions matter:

- `OPL3_Reset(chip, sample_rate_hz)` — initialise.
- `OPL3_GenerateResampled(chip, &stereo_pair)` — produce one stereo PCM
  frame at _your_ sample rate, internally driving the chip at its
  native 49 716 Hz and linearly interpolating to your rate.

Native rate is a hardware fact: the real YMF262 derives its sample
clock by dividing 14.318 MHz (the NTSC color burst, on every PC) by
288 (= 36 operators × 8 cycles). The resampler lets you run any
sensible DAC frequency.

### Sequence player

[`nuked/seq_player.h`](nuked/seq_player.h) � read this. The whole API is:

```c
typedef struct {
    uint16_t reg;        // OPL register (bit 8 = OPL3 second port)
    uint8_t  val;        // value to write
    uint16_t delay_ms;   // delay AFTER this write
} opl_event;

void synth_init(uint32_t sample_rate_hz);              // once at boot
void seq_play(const opl_event *events, uint32_t n,
              int loop);                               // start a song
void seq_stop(void);
void seq_silence(void);                                // emergency mute
int  seq_is_playing(void);

void seq_tick(uint32_t ms_elapsed);                    // call every ~1 ms
void synth_render_sample(int16_t *l, int16_t *r);      // call at sample_rate_hz
```

That's it. Two real-time entry points:

- **`seq_tick(1)`** in a 1 ms timer ISR — walks the song, fires due
  events with `OPL3_WriteRegBuffered`. O(1) amortised; cheap.
- **`synth_render_sample(&l, &r)`** in your DAC half-buffer-empty IRQ —
  produces one PCM frame.

### Songs

A song is a `static const opl_event[]`. You make new songs by capturing
DRO files in DOSBox (`Ctrl+Alt+F7` to start/stop) and converting them
with `tools/dro2hdr.exe`:

```
tools\dro2hdr.exe capture\my.dro songs\my_song.h my_song
```

This produces a header with `my_song[]`, `my_song_count` and
`my_song_total_ms`.

[`songs/pre2_loop_song.h`](songs/pre2_loop_song.h) was generated this
way from a recording of Prehistorik 2's title screen. Loop-point
detection (snipping out exactly one repetition of the song) is done by
`tools/dro_loop3.exe`, which finds the section markers in the OPL
register stream — see the comments in [`tools/dro_loop3.c`](tools/dro_loop3.c)
for the heuristic.

---

## Porting to a microcontroller

### Files to copy / link

| File                                       | Why       |
| ------------------------------------------ | --------- |
| `nuked/opl3.c`, `nuked/opl3.h`             | Synth     |
| `nuked/seq_player.c`, `nuked/seq_player.h` | Sequencer |
| `songs/<your_song>.h`                      | The music |

That's everything. No malloc, no stdio, no `FILE *`, no float math, no
threads. Pure C99.

### Footprint

- **Flash, code:** ~50 KB (Nuked-OPL3 + seq_player, `-Os` on Cortex-M4).
- **Flash, song:** Prehistorik 2 loop = ~102 KB. Other songs scale with
  length and density (the title music is ~17 k events / ~58 s, i.e.
  ~300 events/s on average — fairly dense).
- **RAM:** `sizeof(opl3_chip)` ≘ 14 KB on a 32-bit MCU (the bulk is the
  1024-entry write buffer; you can shrink it by editing
  `OPL_WRITEBUF_SIZE` in `opl3.h` if you don't use buffered writes).
  Plus a few hundred bytes of player state.
- **CPU:** `OPL3_GenerateResampled` is roughly 350–500 cycles per stereo
  frame on Cortex-M4. At 20 kHz that's about 7–10 MHz of effective CPU.
  Comfortable on STM32F4/F7/H7, RP2040, ESP32, etc.

### Skeleton

```c
#include "seq_player.h"
#include "pre2_loop_song.h"

#define SAMPLE_RATE_HZ 20000

void boot(void)
{
    synth_init(SAMPLE_RATE_HZ);
    seq_play(pre2_loop_song, pre2_loop_song_count, /*loop=*/1);

    timer_setup_periodic(1000 /* µs */, on_systick_1ms);
    dac_setup(SAMPLE_RATE_HZ, on_dac_sample);
    enable_irq();
}

void on_systick_1ms(void)             // 1 kHz timer ISR
{
    seq_tick(1);
}

void on_dac_sample(void)              // 20 kHz DAC ISR
{
    int16_t l, r;
    synth_render_sample(&l, &r);
    dac_write(l, r);                  // or  dac_mono((l + r) >> 1);
}
```

The two ISRs share only the `opl3_chip` struct. As long as both run on
the same core, no locking is needed (the chip is a passive data
structure and the writes/reads don't race in any meaningful way). On a
multi-core MCU put both on the same core.

### Sample-rate choices

- **20 000 Hz** — typical clean divider from common MCU peripheral
  clocks; perfectly fine for OPL3 audio. Use this unless you have a
  reason not to.
- **22 050 Hz / 44 100 Hz** — radio-friendly, easy I²S codec rates.
- **49 716 Hz** — native chip rate. If you can clock your DAC there
  exactly, you can use `OPL3_Generate` directly and skip the resampler
  for ~30 % less CPU.

### What the demo wrapper teaches you

[`demo_win/main.c`](demo_win/main.c) is the canonical real-time loop
shape, just done cooperatively (no actual ISRs). Each iteration:

1. Renders one tick's worth of samples (`SAMPLE_RATE / 1000` frames).
2. Calls `seq_tick(1)`.
3. Pushes the buffer to `audio_write`, which blocks until the WaveOut
   ring has room — that's what keeps the loop in real time.

On the MCU you don't need that explicit buffering; the DAC IRQ
self-paces the synth and the timer IRQ self-paces the sequencer.

---

## Re-capturing the Prehistorik 2 music (optional)

A copy of the game lives in `preh2/`. To capture / re-capture:

1. Install DOSBox (already done by `winget install DOSBox.DOSBox`).
2. `& "C:\Program Files (x86)\DOSBox-0.74-3\DOSBox.exe" -conf prehistorik2.conf`
3. When the title music starts, press **Ctrl+Alt+F7** to begin Adlib
   capture; press it again to stop. The `.dro` lands in `capture\`.
4. Detect the loop point and trim:
   `tools\dro_loop3.exe capture\pre2_000.dro capture\pre2_loop.dro`
5. Convert to a header:
   `tools\dro2hdr.exe capture\pre2_loop.dro songs\pre2_loop_song.h pre2_loop_song`
6. `make`

---

## Acknowledgements & licenses

- **Nuked-OPL3** (`nuked/opl3.[ch]`) — © Nuke.YKT, **LGPL 2.1+**.
  Unmodified upstream. See the header in `nuked/opl3.h` for credits to
  the MAME team, OPLx decap project, and others whose work made the
  emulation possible.
- **Prehistorik 2** music — © 1993 Titus Interactive. The `preh2/`
  game folder and any captured `.dro` / generated `.h` files derived
  from it are present here for personal/historical/educational use; do
  not redistribute.
- **Beethoven, "Ode to Joy"** (1824) — public domain.
- Everything else (sequencer, demo, tools) — do whatever you like.
