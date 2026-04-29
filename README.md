# OPL3 microcontroller synthesizer

A small, hackable, MCU-friendly **OPL3 (YMF262) synthesizer** with a
sequence-player API designed to fit comfortably on a Cortex-M / RP2040 /
ESP32 / similar microcontroller. Comes with a Windows demo wrapper that
exercises the **exact** API that the MCU port uses, so what you hear on
your PC is what your MCU will play.

> **Why?** For fun, and because the OPL3 (the FM synth chip in the
> Sound Blaster 16 / AdLib Gold) is a beautiful piece of late-80s
> hardware whose music â€” fast attack envelopes, gritty FM timbres, that
> distinctive ~50 kHz sample rate â€” is impossible to reproduce
> convincingly with any other technique. With ~80 KB of flash for the
> code plus whatever your songs need, and one spare CPU, you can have
> authentic OPL3 audio on a $3 board.

The included demo plays the title-screen music from the 1993 DOS game
**Prehistorik 2**, captured live from the original `PRE2.EXE` running
in DOSBox and converted to a packed `opl_song` C header.

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
|   +-- pre2_loop_song.h          Prehistorik 2 title music  (~41 KB flash)
|   +-- doom_setup_song.h         DOOM setup utility music   (~34 KB flash)
|   +-- ww_intro_song.h           Wacky Wheels intro         (~2.5 KB flash)
|   +-- ww_theme_song.h           Wacky Wheels theme         (~10 KB flash)
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
+-- games/                        DOSBox configs + your copies of the games
+-- Makefile / build.bat          Windows build
+-- README.md                     This file
```

## Two cores, one API

The repository keeps each OPL emulation core in its own folder so that
alternative engines (e.g. DOSBox `dbopl`) can be added side-by-side
without disturbing the others. Today there are two **Nuked-OPL3**
flavours:

- [`nuked/`](nuked/) is the **portable reference build** — byte-identical
  Nuked-OPL3 v1.8 plus a tiny sequencer. Use it on PC for
  bit-accurate playback, regression testing, and as the readable
  baseline against which `nuked_optimized/` can be diffed.
- [`nuked_optimized/`](nuked_optimized/) is the **MCU-optimized variant
  of the same Nuked port** — same `seq_player.h` / `opl3.h` API, but
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

> **Note:** `nuked_optimized/` targets the MCU and pulls in `cmsis_gcc.h`
> and a `fifo.[ch]` implementation that live outside this repo, so it
> does **not** build standalone on PC. Use the default `nuked/` core
> for the Windows demo; bring `nuked_optimized/` into your firmware
> project alongside its CMSIS / FIFO dependencies.

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

Requires MinGW gcc (anything â‰¥ 8 works).

```
make                 (or:  build.bat)
opl_demo.exe         loop the Prehistorik 2 title music
opl_demo.exe --once  play it once and exit
opl_demo.exe doom    play the DOOM setup music
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

- `OPL3_Reset(chip, sample_rate_hz)` â€” initialise.
- `OPL3_GenerateResampled(chip, &stereo_pair)` â€” produce one stereo PCM
  frame at _your_ sample rate, internally driving the chip at its
  native 49 716 Hz and linearly interpolating to your rate.

Native rate is a hardware fact: the real YMF262 derives its sample
clock by dividing 14.318 MHz (the NTSC color burst, on every PC) by
288 (= 36 operators Ã— 8 cycles). The resampler lets you run any
sensible DAC frequency.

### Sequence player

[`nuked/seq_player.h`](nuked/seq_player.h) — read this. The whole API is:

```c
typedef struct {
    const uint8_t *data;        // codemap + (code,val) opcode stream
    uint32_t       data_len;
    uint16_t       codemap_len;
    uint8_t        short_code;  // (val+1)     ms delay opcode
    uint8_t        long_code;   // (val+1)*256 ms delay opcode
    uint32_t       total_ms;
} opl_song;

void synth_init(uint32_t sample_rate_hz);              // once at boot
void seq_play_song(const opl_song *song, int loop);    // start a song
void seq_stop(void);
void seq_silence(void);                                // emergency mute
int  seq_is_playing(void);

void seq_tick(uint32_t ms_elapsed);                    // call every ~1 ms
void synth_render_sample(int16_t *l, int16_t *r);      // call at sample_rate_hz
```

The `opl_song` payload is byte-for-byte the same as a DOSBox DRO v2
file's data section, so songs are ~2 bytes per OPL register write.

That's it. Two real-time entry points:

- **`seq_tick(1)`** in a 1 ms timer ISR â€” walks the song, fires due
  events with `OPL3_WriteRegBuffered`. O(1) amortised; cheap.
- **`synth_render_sample(&l, &r)`** in your DAC half-buffer-empty IRQ â€”
  produces one PCM frame.

### Songs

A song is a `static const opl_song` (with its packed byte payload). You
make new songs by capturing DRO files in DOSBox (`Ctrl+Alt+F7` to
start/stop) and converting them with `tools/dro2hdr.exe`:

```
tools\dro2hdr.exe capture\my.dro songs\my_song.h my_song
```

This produces a header defining `my_song` (and its backing
`my_song_data[]` byte array).

[`songs/pre2_loop_song.h`](songs/pre2_loop_song.h) was generated this
way from a recording of Prehistorik 2's title screen. Loop-point
detection (snipping out exactly one repetition of the song) is done by
`tools/dro_loop3.exe`, which finds the section markers in the OPL
register stream â€” see the comments in [`tools/dro_loop3.c`](tools/dro_loop3.c)
for the heuristic.

---

## Porting to a microcontroller

### Files to copy / link

| File                                                           | Why       |
| -------------------------------------------------------------- | --------- |
| `nuked_optimized/opl3.c`, `nuked_optimized/opl3.h`             | Synth     |
| `nuked_optimized/seq_player.c`, `nuked_optimized/seq_player.h` | Sequencer |
| `songs/<your_song>.h`                                          | The music |

Plus `cmsis_gcc.h` (from your CMSIS pack) and a small byte FIFO that
exposes the `fifo_init` / `fifo_put_buf` / `fifo_get_buf` /
`FIFO_FREECOUNT` interface used by `nuked_optimized/seq_player.c`.
Drop in your project's existing FIFO or vendor any single-producer /
single-consumer ring of your choice.

(If you'd rather start from the unmodified reference port, swap
`nuked_optimized/` for `nuked/` everywhere above; you then don't need
CMSIS or a FIFO, but you give up the MCU optimizations described
below.)

That's everything. No malloc, no stdio, no `FILE *`, no float math, no
threads. Pure C99.

### Footprint (`nuked_optimized/` on Cortex-M4, `-Os`)

- **Flash, code:** ~50 KB (Nuked-OPL3 + seq_player).
- **Flash, song:** packed `opl_song` is ~2 bytes per OPL register
  write — e.g. Prehistorik 2 loop = ~41 KB, DOOM setup = ~34 KB,
  Wacky Wheels intro = ~2.5 KB. Songs scale with length and density.
- **RAM:** `sizeof(opl3_chip)` ? 14 KB (the bulk is the 1024-entry
  write buffer; you can shrink it by editing `OPL_WRITEBUF_SIZE` in
  `opl3.h` if you don't use buffered writes). Plus a few KB for the
  sample FIFO (`SYNTH_FIFO_FRAMES` in `seq_player.c`, default ~4 KB)
  and a few hundred bytes of player state.
- **CPU:** `OPL3_Generate` is roughly 350–500 cycles per stereo frame
  on Cortex-M4. At the native 49 716 Hz that's about 17–25 MHz of
  effective CPU — comfortable on STM32F4/F7/H7, RP2040, ESP32, etc.
  (The reference `nuked/` port is somewhat heavier and goes through
  `OPL3_GenerateResampled` instead; see _Sample-rate choices_ below.)

### Skeleton (`nuked_optimized/` on MCU)

```c
#include "seq_player.h"
#include "pre2_loop_song.h"

#define SAMPLE_RATE_HZ 49716   /* OPL3 native rate -- run as close to this as your DAC allows */

void boot(void)
{
    synth_init(SAMPLE_RATE_HZ);
    seq_play_song(&pre2_loop_song, /*loop=*/1);

    timer_setup_periodic(1000 /* µs */, on_systick_1ms);
    dac_setup(SAMPLE_RATE_HZ, on_dac_sample);
    enable_irq();
}

void on_systick_1ms(void)             // 1 kHz low-priority timer ISR
{
    seq_tick(1);
    synth_update();                   // refill the sample FIFO
}

void on_dac_sample(void)              // ~50 kHz high-priority DAC ISR
{
    int16_t l, r;
    synth_render_sample(&l, &r);      // O(1): pop one frame from the FIFO
    dac_write(l, r);                  // or  dac_mono((l + r) >> 1);
}
```

The sample FIFO between `synth_update()` and `synth_render_sample()`
is what makes this safe: the DAC ISR is guaranteed to find a frame
ready every time, no matter how long the previous `seq_tick()` took.
The two ISRs share only the `opl3_chip` struct and the FIFO; on a
single-core MCU no locking is needed (the producer briefly masks IRQs
around the FIFO write — see `nuked_optimized/seq_player.c`). On a
multi-core MCU put both on the same core.

### Sample-rate choices and the FIFO

The MCU build is deliberately structured so the audio ISR is **as
cheap as physically possible**: it pops one stereo frame from a small
ring and returns. All synth work — the expensive part — happens in a
lower-priority context that calls `synth_update()` to refill the ring
whenever it has spare time.

```
  low priority                                high priority
  --------------------------------            -------------------------
  seq_tick(1)        ? OPL register writes
  synth_update()     ? OPL3_Generate ? push   pop ? synth_render_sample()
                          frames into FIFO            ?
                                                    DAC / I?S / PWM
```

This is why `nuked_optimized/` deliberately calls `OPL3_Generate`
(native rate) rather than `OPL3_GenerateResampled` (any rate). The
resampler is convenient on PC but it adds per-sample interpolation
cost for no audible benefit on the MCU. Instead, **clock your audio
ISR as close to the chip's native 49 716 Hz as you reasonably can**
and let the FIFO absorb whatever jitter the lower-priority producer
introduces. That gives you bit-accurate playback for free.

If your DAC clock can't hit 49 716 Hz exactly:

- **48 000 Hz** is essentially indistinguishable and the easiest
  divider on most modern audio peripherals.
- **44 100 Hz** is fine — a fraction of a percent off pitch, no other
  audible effect.
- **24 858 Hz** (= 49 716 / 2) is the FALCON build's choice; halves the
  per-frame CPU at a tiny aliasing cost on the highest OPL voices.
- **Anything below ~22 kHz** — use `OPL3_GenerateResampled` from the
  reference `nuked/` port instead and accept the resampler cost.

### What the demo wrapper teaches you

[`demo_win/main.c`](demo_win/main.c) is the canonical real-time loop
shape, just done cooperatively (no actual ISRs). Each iteration:

1. Renders one tick's worth of samples (`SAMPLE_RATE / 1000` frames).
2. Calls `seq_tick(1)`.
3. Pushes the buffer to `audio_write`, which blocks until the WaveOut
   ring has room â€” that's what keeps the loop in real time.

On the MCU you don't need that explicit buffering; the DAC IRQ
self-paces the synth and the timer IRQ self-paces the sequencer.

---

## Capturing OPL music from a DOS game

DOSBox can sniff every write your game makes to the OPL2/OPL3 chip and
log them to a **DRO v2** file. That file is the raw, byte-perfect
register stream — same one the chip would have seen on real hardware —
and is the input to all of the converter / loop-finder tools below.

### One-time setup

1. Install DOSBox 0.74 (e.g. `winget install DOSBox.DOSBox`).
2. Drop the game into a folder under `games/` (folder is gitignored —
   you supply your own copy of the game).
3. Write a small DOSBox config that points at it. See
   [`games/doom.conf`](games/doom.conf) for a complete example. The
   parts that matter are:

   ```ini
   [sblaster]
   oplmode=opl3
   oplemu=nuked     ; (only affects DOSBox's own playback; .dro records the raw register writes regardless)
   oplrate=49716

   [dosbox]
   captures=capture ; .dro files land here

   [autoexec]
   mount c c:\silixcon-devel\opl\games\doom
   c:
   ```

### Recording

```
& "C:\Program Files (x86)\DOSBox-0.74-3\DOSBox.exe" -conf games\doom.conf
```

Inside DOSBox, start the game, get to the music you want, then:

| Key           | Action                                   |
| ------------- | ---------------------------------------- |
| `Ctrl+Alt+F7` | Start / stop OPL capture (writes `.dro`) |
| `Ctrl+F10`    | Release / grab the mouse                 |
| Type `EXIT`   | Quit DOSBox                              |

Each capture appears in `capture/` as `<gamename>_NNN.dro`.

> **Tip:** silence the SFX channels first (`-nomonsters -nosound` in
> DOOM, mute SFX in the in-game options elsewhere) so only the music
> ends up in the recording.

---

## The `tools/` folder — DRO triage and conversion

Once you have a `.dro`, the offline programs in `tools/` turn it into a
clean, embeddable C header. They share the same DRO v2 reader and can
be chained pipeline-style.

### The two you'll always use

| Tool                  | What it does                                                                                                                                                                                           |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `tools\dro_loop3.exe` | Finds the loop point (start of the second repetition) and writes a trimmed `.dro` containing exactly one loop iteration. Heuristic: scans the OPL register stream for the longest self-similar suffix. |
| `tools\dro2hdr.exe`   | Converts a `.dro` into a packed `opl_song` C header that drops straight into [`songs/`](songs/). The payload is byte-identical to the DRO data section, so a 35 KB DRO becomes a 35 KB header.         |

Typical pipeline for a looping song (Prehistorik 2 title music):

```
tools\dro_loop3.exe  capture\pre2_000.dro  capture\pre2_loop.dro
tools\dro2hdr.exe    capture\pre2_loop.dro songs\pre2_loop_song.h pre2_loop_song
make
```

For a one-shot song (DOOM setup music) the loop step is unnecessary —
just slice off the intro/outro silence with `dro_slice` (below) and
feed the result to `dro2hdr`.

### Triage / inspection helpers

These don't modify the file; they help you decide _where_ to trim.

| Tool                                                | Purpose                                                                                                                    |
| --------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- |
| `tools\dro_analyze.exe file.dro`                    | One-page summary: length, register-write density, channels touched, percussion usage. Run this first on any new capture.   |
| `tools\dro_dump.exe file.dro [start_ev] [count]`    | Pretty-print the decoded register-write stream with timestamps. Used to eyeball where a section starts.                    |
| `tools\dro_segments.exe file.dro`                   | Splits the timeline at long silent gaps; useful for separating stinger / loop / outro within a single capture.             |
| `tools\dro_perc.exe file.dro`                       | Lists every percussion strike with its timestamp — handy for finding bar boundaries in songs whose drums lock to the beat. |
| `tools\dro_trigs.exe file.dro`                      | Reports register writes that look like loop / cue triggers (e.g. abrupt instrument changes).                               |
| `tools\dro_loopfind.exe file.dro loop_start_ms [N]` | Given a candidate loop start, scores how well the tail repeats it. Use to validate / nudge what `dro_loop3` produced.      |

### Surgery helpers

These rewrite the file.

| Tool                                                   | Purpose                                                                                                                         |
| ------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------- |
| `tools\dro_slice.exe in.dro out.dro start_ms end_ms`   | Cuts out a `[start_ms, end_ms)` window. Use to crop intros/outros once `dro_analyze` or `dro_dump` told you where to cut.       |
| `tools\dro_evrange.exe in.dro out.dro start_ev end_ev` | Same idea but in event-index space.                                                                                             |
| `tools\dro_loop.exe`, `dro_loop2.exe`, `dro_loop3.exe` | Three generations of the loop-finder; `dro_loop3` is the current default. The older ones are kept for diffing on tricky inputs. |

### Building the tools

The tools are plain C99 with no dependencies; rebuild any of them with:

```
gcc -O2 -Wall -Wextra -std=c99 tools\<name>.c -o tools\<name>.exe
```

---

## Acknowledgements & licenses

- **Nuked-OPL3** (`nuked/opl3.[ch]`) â€” Â© Nuke.YKT, **LGPL 2.1+**.
  Unmodified upstream. See the header in `nuked/opl3.h` for credits to
  the MAME team, OPLx decap project, and others whose work made the
  emulation possible.
- **Game music captures** — the `.dro` / generated `.h` files in this
  repo are derived from games whose music remains © their respective
  rightsholders (Titus Interactive for _Prehistorik 2_, id Software for
  _DOOM_, Apogee for _Wacky Wheels_). They are included for personal /
  historical / educational use; do not redistribute. The original game
  binaries themselves are not in the repo — supply your own copy under
  `games/`.
- Everything else (sequencer, demo, tools) â€” do whatever you like.
