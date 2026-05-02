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
+-- sequencer/                  <-- shared OPL sequence player (PC + MCU)
|   +-- seq_player.c              tick + render API; one file, every core
|   +-- seq_player.h              public interface -- read this file first
|                                 (define -DSEQ_PLAYER_FIFO on the MCU
|                                  to get the producer/consumer split)
|
+-- cores/                      <-- chip emulators, one folder each
|   +-- nuked/                    portable reference build (PC + MCU)
|   |   +-- opl3.c     opl3.h       Nuked-OPL3 v1.8 (LGPL 2.1+, unmodified)
|   |
|   +-- nuked_optimized/          MCU-tuned variant of the Nuked port (MCU-only build)
|   |   +-- opl3.c     opl3.h       Nuked-OPL3 v1.8 + heavy Cortex-M tweaks
|   |
|   +-- opal/                     alternate, much smaller OPL3 (PC + MCU)
|   |   +-- opal.c     opal.h       Reality's Opal OPL3, public-domain (~12 KB code)
|   |   +-- opl3.h                  header-only `OPL3_*` adapter (no .c shim)
|   |   +-- LICENSE.txt
|   |
|   +-- mame/                     MAME's OPL3 (Burczynski/Satoh), full rhythm + 4-op
|   |   +-- ymf262.c   ymf262.h     Vendored from FBNeo (byte-identical to MAME's ymf262.c)
|   |   +-- mame_compat.h           Tiny shim: typedefs + no-op SCAN_VAR/ACB_* save-state
|   |   +-- opl3.h                  header-only `OPL3_*` adapter (no .c shim)
|   |
|   +-- adlibemu/                 DOSBox legacy OPL3 (Ken Silverman lineage, LGPL 2.1+)
|       +-- adlibemu_opl3.c         Vendored from ValleyBell/libvgm (8-line OPL3 shim)
|       +-- adlibemu_opl_inc.[ch]   The actual emulator (shared OPL2/OPL3 body)
|       +-- adlibemu.h              Upstream public API
|       +-- adlibemu_compat.h       Tiny shim replacing libvgm's stdtype/snddef/common_def
|       +-- opl3.h                  header-only `OPL3_*` adapter (no .c shim)
|
+-- heatshrink/                 <-- portable streaming decompressor (PC + MCU)
|   +-- heatshrink_decoder.[ch]   Atomic Object's heatshrink, unmodified
|   +-- heatshrink_encoder.[ch]   (PC-only; only dro2hdr / dro_pack pull this in)
|   +-- hs_stream.[ch]            Generic byte-stream pump on top of the decoder
|   +-- heatshrink_config.h       Compile-time tuning (static vs dynamic alloc)
|
+-- songs/                      <-- + at least one *_song.h
|   +-- opl_song_hs.h             Self-describing descriptor (hs_data + metadata)
|   +-- *_song.h                  41 ready-to-link songs, heatshrink-packed (w13/l4)
|   +-- *.dro / *.dro_hs13        Source DRO captures + runtime-loadable packed form
|   +-- *.vgz                     Original VGM source files for the above
|
+-- demo_win/                   <-- Windows-only wrapper, NOT for the MCU
|   +-- main.c                    Fake-ISR loop + curated SONGS[] list
|   +-- audio.h                   Tiny audio-sink interface
|   +-- audio_win.c               WinMM (waveOut) implementation
|   +-- dro_load.[ch]             Load .dro / .dro_hs* files at runtime (PC only)
|
+-- tools/                      <-- offline PC utilities
|   +-- vgz2dro.c     / .exe      VGM/VGZ capture -> DRO v2
|   +-- dro_opt.c     / .exe      Strip redundant register writes from a .dro
|   +-- dro_loop3.c   / .exe      Loop-point detector
|   +-- dro_pack.c    / .exe      DRO -> .dro_hs<W>l<L>  (runtime-loadable packed)
|   +-- dro2hdr.c     / .exe      DRO -> embeddable C header (with --hs for packed)
|   +-- hs_bench.c    / .exe      Sweep heatshrink window/lookahead for best ratio
|   +-- regen_songs.ps1           Re-pack every songs/*.dro into songs/*_song.h
|
+-- capture/                      DRO captures from DOSBox
+-- games/                        DOSBox configs + your copies of the games
+-- Makefile / build.bat          Windows build (one binary per core)
+-- README.md                     This file
```

## Cores: one API, multiple engines

The sequencer in [`sequencer/`](sequencer/) is core-agnostic: it
depends only on a tiny `OPL3_*` surface (`Reset`, `WriteReg`,
`WriteRegBuffered`, `Generate`, `GenerateResampled`). Each chip
emulator lives in its own folder and exposes that surface either
directly (Nuked) or via a header-only adapter (Opal). Adding a new
engine is a self-contained "new folder + adapter header" exercise; no
changes anywhere else.

Four engines are wired up today (`nuked_optimized` is an MCU-only
fork of `nuked`, not a separate engine). All emulate the same Yamaha
YMF262 (OPL3); they differ in _how faithful_ the emulation is and
what that costs in cycles and flash.

#### Cores at a glance

Footprints are gcc 13 `-Os` for x64 (`size` on the per-core `.o`); MCU
flash with `arm-none-eabi-gcc -Os -mthumb -mcpu=cortex-m4` lands within
~5 % of these. CPU cost is 60 s of `metallica` (typical OPL3 game-music
workload) @ 49 716 Hz stereo on a modern x64 laptop; see [Measured CPU
cost](#measured-cpu-cost) for the harness and other songs.

| core              | license       | runtime math                              | rhythm | 4-op | bit-exact       | `.text` (-Os) |               static `.bss` tables | per-chip RAM | ns/frame | realtime |
| ----------------- | ------------- | ----------------------------------------- | ------ | ---- | --------------- | ------------: | ---------------------------------: | -----------: | -------: | -------: |
| `nuked`           | LGPL 2.1+     | pure integer                              | yes    | yes  | **yes** (decap) |        7.5 KB |                                  0 | **20.5 KB**? |      482 |    42.7× |
| `nuked_optimized` | LGPL 2.1+     | pure integer                              | yes    | yes  | yes for OPL2    |         ~7 KB |                                  0 |      ~12 KB? |     297? |    68.8× |
| `opal`            | public domain | pure integer                              | **no** | yes  | no              |        6.4 KB |                                  0 |       5.7 KB |      220 |    89.5× |
| `adlibemu`        | LGPL 2.1+     | **`double` envelopes + libm `pow`/`sin`** | yes    | yes  | no              |       14.1 KB |           **14.6 KB** (wave + LFO) |       7.5 KB |      236 |    86.0× |
| `mame`            | BSD-style?    | integer (FP only at init)                 | yes    | yes  | no              |       11.3 KB | **58.0 KB** (`sin_tab` + `tl_tab`) |      13.8 KB |      190 |   106.7× |

? `nuked_optimized` measured on the 9-ch / OPL2 / mono profile; the full
OPL3 build sits closer to plain `nuked` in both code size and ns/frame.
Per-chip RAM drops because the unused channels/operators are compiled out.
? MAME license, BSD-compatible commercial use; see
[`cores/mame/ymf262.c`](cores/mame/ymf262.c) header.
? `nuked` keeps its log-sin/exp ROM and per-operator scratch _inside_
`opl3_chip` (rather than as file-static globals), which is why its
per-instance footprint is the largest even though `.bss` is zero. With
one chip — the usual case — total RAM is ~20.5 KB.

Add ~5–10 KB of libm (`pow`, `sin`, soft-float helpers if no FPU) on top
of `adlibemu` if your project doesn't already link it.

#### Pros / cons

- **`nuked`** — _the truth._
  - - Bit-identical to a real YMF262, including rhythm mode and 4-op.
  - - Pure integer; runs anywhere from M0 up.
  - ? Slowest of the bunch (~3–4× plain `mame`).
  - ? Function-pointer dispatch over 8 waveforms hurts on M-class cores.

- **`nuked_optimized`** — _Nuked, MCU-tuned._
  - - Same audible output as `nuked` for OPL2 content; cuts work in half.
  - - Active-slot skip list, fused L+R loop, `__SSAT`/`__USAT`, CCM placement.
  - - Compile-time profiles (OPL2 only, mono, channel cap) for tight MCUs.
  - ? Pulls in CMSIS (`cmsis_gcc.h`); not built on PC.
  - ? Trades a few per-sample bits when the OPL2/mono knobs are turned on.

- **`opal`** — _small and quick._
  - - ~12 KB code, simple integer inner loop.
  - - Fast on every target; trivial to drop into a 256 KB part.
  - ? **No rhythm/percussion mode.** id Software / Prehistorik 2 / Doom
    drum kits silently disappear.
  - ? A few envelope edge cases differ from real silicon.

- **`adlibemu`** — _DOSBox lineage, the architectural odd one out._
  - - Plays everything (full OPL3, rhythm, 4-op).
  - - LGPL 2.1+ (link-friendly even for closed firmware).
  - - Direct ancestor of `dbopl`; useful as a DOSBox-era reference.
  - ? **Uses `double` for envelope state and calls libm's `pow()`**
    on every attack/decay/release-rate-changing register write.
    Per-sample envelope updates are FP, not integer.
  - ? **Pulls in libm** (`pow`, `sin`); table init alone does ~256 `sin`
    and ~512 `pow` calls.
  - ? Fine on x64 / M7 / M4F (consider `s/double/float/`); **not viable
    on M0/M0+** (every per-sample envelope step lowers to soft-double).

- **`mame`** — _flash-for-cycles._
  - - Fastest in the repo; full OPL3, rhythm, 4-op.
  - - Inner loop is two unconditional table lookups (no branches, no fn ptrs).
  - - Pure integer at runtime; `double` only at one-shot table init.
  - ? ~58 KB of pre-flattened tables (`sin_tab` 32 KB + `tl_tab` 26 KB).
    Tight on 128/256 KB parts; comfortable on F4/F7/H7.
  - ? Not bit-exact (pre-Nuked envelope/phase model). Audibly indistinguishable.

#### How an OPL3 emulator spends its time

Every per-sample mix loop does, for each of the chip's 36 operators:
"compute current phase, look up sin(phase), apply the envelope's
log-domain attenuation, exp-back to linear, sum into the channel
accumulator". An OPL3 ROM stores log-sin and exp lookup tables for
1/4 of a sine period each (a few hundred bytes), and the silicon
reconstructs the rest by quadrant flips and a barrel shift. **The
core's whole performance story is how it handles those two table
lookups per operator per sample.**

#### `nuked/` — bit-exact decap reference

[`cores/nuked/opl3.c`](cores/nuked/opl3.c) is **Nuked-OPL3 v1.8** by Nuke.YKT,
verified byte-for-byte against the upstream
<https://github.com/nukeykt/Nuked-OPL3> master. It mirrors the real
silicon: ~1.5 KB of `logsinrom` + `exprom` plus per-operator
quadrant flips, a function-pointer dispatch over the 8 OPL
waveforms, a data-dependent shift in the exp stage, and signed XOR
masking for negative half-cycles. **Output is bit-identical to a
real YMF262.** Cost: roughly 25–30 cycles per operator on x64,
~3–4× that on Cortex-M4. Use it as the audible reference and
to settle "is this song playing right?" disputes.

#### `nuked_optimized/` — same chip, MCU-tuned (MCU-only build)

[`cores/nuked_optimized/opl3.c`](cores/nuked_optimized/opl3.c) keeps Nuked's
audio model and ROM contents but reworks the _traversal_: an active-
slot index list (skip operators whose envelope is OFF), cached
envelope/phase increments, fused L+R mix loop, CCM/RAM placement
attributes, and Cortex-M4 intrinsics (`__SSAT`/`__USAT` instead of
branchy clamps, `__builtin_ctz` for the eg trailing-zero scan).
Adds opt-in compile flags: `OPL_FORCE_OPL2=1` (drops the OPL3
upper bank, halves the operator count for OPL2 material), `OPL_MAX_CHANNELS<18`
(silently ignores channels above the limit), `OPL_MONO=1`. With the
default profile (9-ch / OPL2 / mono) it does roughly half the work
of plain Nuked on the same song and still sounds bit-identical
_for OPL2 content_. Not built on PC because it pulls in
`cmsis_gcc.h`; bring it into your firmware project alongside that
dependency. The non-bit-exact compromises are listed in the banner
at the top of the file.

#### `opal/` — small and fast, but rhythm-mode incomplete

[`cores/opal/opal.c`](cores/opal/opal.c) is Reality's **Opal OPL3** (public
domain, pure-C port from libADLMIDI). It uses small tables (~10 KB
total) with on-the-fly waveform synthesis — same engineering tradeoff
as Nuked but a much lighter inner loop. ~12 KB code. **Caveat:**
the rhythm/percussion mode (register `0xBD` BD/SD/TT/TC/HH strikes)
is silently dropped, and a few envelope edge cases differ subtly.
Excellent fallback when you don't need percussion (most non-id-Software
material is fine). Refuses to play Prehistorik 2's drum-heavy title.

#### `mame/` — MAME's OPL3, full-featured, fastest of the lot

[`cores/mame/ymf262.c`](cores/mame/ymf262.c) is Jarek Burczynski / Tatsuyuki
Satoh's MAME OPL3, vendored from FBNeo (byte-identical to MAME's
upstream `ymf262.c`). Pure C99 once a tiny `mame_compat.h` shim
replaces the framework `#include`s. Full 18 channels including
4-op mode and rhythm/percussion, so it plays everything Nuked plays
(though not bit-identically — pre-Nuked envelope/phase model).

The reason it's the **fastest** core in this repo is the table
strategy: instead of mirroring the silicon's compact ROMs, MAME
pre-flattens `sin_tab` (32 KB, all 8 OPL waveforms expanded across
the full period) and `tl_tab` (26 KB, exp + total-level attenuation
already combined). The entire FM operator collapses to:

```c
p = (env << 4) + sin_tab[wave + ((phase + pm) >> SH) & MASK];
return (p >= TL_TAB_LEN) ? 0 : tl_tab[p];
```

— two table lookups, no branches, no function pointers, no
data-dependent shifts. Roughly 8 cycles per operator on x64 vs
Nuked's ~25.

Tradeoff: ~90 KB total flash (50–60 KB of which is those two
tables). Fine on F4 and up; tight on 256 KB parts. Audio path is
100% integer. `double` is used only at one-shot table init (and for
the timer-period scalar, only relevant if the song uses OPL timer
reads — DRO captures don't).

#### `adlibemu/` — DOSBox's legacy OPL3 (Ken Silverman lineage)

[`cores/adlibemu/adlibemu_opl_inc.c`](cores/adlibemu/adlibemu_opl_inc.c) is the
original **ADLIBEMU** by Ken Silverman (1998-2001), extended to OPL3
by the DOSBox team (2002-2010) and vendored from
[ValleyBell/libvgm](https://github.com/ValleyBell/libvgm). DOSBox
kept this around as the "compat" core after replacing it with `dbopl`;
it's the direct ancestor of `dbopl` and the closest in-tree analogue
to it that's available as **pure C** under a friendly license
(LGPL 2.1+, link-friendly even for closed-source firmware).

**The architectural odd-one-out** (see also the [pros/cons](#pros--cons)
block above). Every other core in this repo is pure integer (or, in
`mame`'s case, integer at runtime with `double` used only at one-shot
table init). adlibemu instead uses **`double` precision** for the
envelope state (`amp`, `vol`, `step_amp`, `sustain_level`, attack-curve
coefficients `a0..a3`, the per-op `decaymul`/`releasemul`) and calls
**`pow()` from libm on every register write that re-derives an
attack/decay/release rate** (`change_attackrate` etc. — every
`0x60`/`0x80` write hits libm). The per-sample wave/phase math is
fixed-point `Bit32s`, but the envelope is recomputed in FP each sample.
libm gets pulled in for chip table init too (~256 `sin()` + ~512 `pow()`
calls during `init_tables`), adding ~5–10 KB of flash on top of the
~30 KB code if your project doesn't already link libm.

Useful as **a sanity check on the others** (when something sounds
weird in `mame`, A/B against `adlibemu` to see if the same
compromises were made in DOSBox-era emulation) and as a stand-in for
`dbopl` benchmarks without the GPL/C++ pain. About 25% slower than
`mame` on x64; would lose more ground on M4F because of the FP
envelope updates, and is unbuildable on M0.

#### Measured CPU cost

<a id="measured-cpu-cost"></a>

Each `opl_demo_<core>.exe` accepts `--bench [seconds]`, which runs
the exact same per-sample render + 1 ms tick loop as live playback
but skips audio output and times it with `QueryPerformanceCounter`.
The number you get is the cost of `synth_render_sample` +
`seq_tick`, i.e. exactly what would run inside the MCU's DAC and
SysTick ISRs.

Sample run on a modern x64 laptop, 60 s of `metallica` (heavy
melodic load) at 49 716 Hz stereo, 4 runs averaged (variance ? 2%):

| core              | ns per stereo frame | realtime ratio | host CPU% |
| ----------------- | ------------------: | -------------: | --------: |
| `nuked`           |                 482 |          42.7× |     2.34% |
| `nuked_optimized` |                 297 |          68.8× |     1.45% |
| `adlibemu`        |                 236 |          86.0× |     1.16% |
| `opal`            |                 220 |          89.5× |     1.12% |
| `mame`            |                 190 |         106.7× |     0.94% |

(`nuked_optimized` measured separately on a 9-ch / OPL2 / mono
profile build; the others are full 18-ch OPL3 stereo.)

Don't read "`mame` beats `nuked`" as a statement about the chips —
it's a statement about which inner loop x64 happens to schedule
best. On Cortex-M the gap will narrow somewhat (`mame` uses a few
64-bit shifts that x64 does in 1 cycle but M4 emulates in several),
but the ranking is stable. Order of magnitude on STM32F407 @
168 MHz at 49 716 Hz: `mame`/`opal` ? 25-35% of one core,
`nuked_optimized` (default 9-ch / OPL2 / mono profile) ? 30-40%,
plain `nuked` ? 50-70%. `adlibemu` lands in the `opal`/`mame` band
on raw cycles but the FP envelope updates push it noticeably higher
on M4F. All four are real-time on M7 with margin (`adlibemu` only
if you have an FPU — see its section).

#### Picking a core

| If…                                                                  | Use                                                                                   |
| -------------------------------------------------------------------- | ------------------------------------------------------------------------------------- |
| You want to verify "did I capture this song right?"                  | `nuked` — the audio reference                                                         |
| The song uses rhythm-mode percussion (id Software, Prehistorik 2, …) | `mame`, `adlibemu`, `nuked`, or `nuked_optimized` (not `opal`)                        |
| You're on an MCU with ?512 KB flash                                  | `mame` — fastest and most complete                                                    |
| You're on an MCU with ?256 KB flash                                  | `opal` (no percussion) or `nuked_optimized` (default profile)                         |
| You're targeting strict bit-accuracy on an MCU                       | `nuked_optimized` with all defaults disabled, or plain `nuked` if you have the cycles |
| You want a DOSBox-era second opinion on tone (M4F or higher)         | `adlibemu` — same algorithmic family as the early DOSBox SoundBlaster emulation       |
| You need to A/B sound quality                                        | Build all four PC binaries, listen — `--once` makes it easy                           |

#### Why not other engines?

- **`dbopl`** (DOSBox). Uses a similar flat-table strategy to `mame`;
  benchmarks roughly tied. C++-heavy (templates, member function
  pointers, namespaces, `this+1`/`this+2` channel-array arithmetic)
  so porting to plain C is real work. **GPLv2 forces the entire
  combined binary to GPLv2** — a serious commercial liability if
  your firmware is closed-source. We ship `adlibemu` instead, which
  is the same algorithmic family (DOSBox kept both around) without
  the GPL/C++ pain.
- **`ymfm`** (Aaron Giles). Modern C++17 unified Yamaha FM family,
  BSD-3-Clause. Broader chip coverage than this repo needs (we only
  want OPL3). Comparable speed to `mame`/`dbopl`. Porting cost: high
  (templates, std::array, references). Worth it only if you need
  OPL2/OPN/OPN2 etc. side-by-side.
- **`hatari` OPL** and similar legacy ports. Older, slower, less
  faithful than any of the above. Not recommended.

If you ever feel cycle-starved on the MCU with `mame`, the
productive moves (in order of payoff vs effort) are: place the hot
tables in CCM/DTCM RAM if your part has it (2× on memory-bound
parts); pre-bake `sin_tab`/`tl_tab` as `static const` to drop the
libm dependency (~10 KB flash); precompute `fn_tab` for a fixed
sample rate (4 KB RAM saved); add `__SSAT` to the final clamps; add
an active-channel skip list. None of those touch `op_calc`. Together
they should put `mame` solidly under 20% of an STM32F407.

---

## Quick start (Windows)

Requires MinGW gcc (anything â‰¥ 8 works).

```
make                              (or:  build.bat)
opl_demo_nuked.exe --list         show all built-in songs
opl_demo_nuked.exe                loop the default song (Prehistorik 2 title)
opl_demo_nuked.exe eric --once    play one of the curated entries once and exit
opl_demo_nuked.exe metallica      loop "Master of Puppets"

opl_demo_nuked.exe path\to\file.dro       play any DOSBox DRO v2 capture
opl_demo_nuked.exe path\to\file.dro_hs13  play a heatshrink-packed DRO at runtime
opl_demo_mame.exe  metallica --bench 30   render 30 s of song with no audio,
                                          print render time / CPU% (no real-time pacing)
```

The build produces one binary per PC-ready core:
`opl_demo_nuked.exe`, `opl_demo_opal.exe`, and
`opl_demo_mame.exe`. All three take the same command line; the
banner prints `core: <name>` so you know which engine you're hearing.

The curated short-name list lives at the top of
[`demo_win/main.c`](demo_win/main.c) (one `#include` + one row in
`SONGS[]` per song); add a row to expose any of the 41 packed songs
in `songs/`.

---

## How it works

### Synth core

[`cores/nuked/opl3.c`](cores/nuked/opl3.c) and [`cores/nuked/opl3.h`](cores/nuked/opl3.h) are
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

[`sequencer/seq_player.h`](sequencer/seq_player.h) — read this. The whole API is:

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

For flash-constrained targets there is a second descriptor
`opl_song_hs` (defined in [`songs/opl_song_hs.h`](songs/opl_song_hs.h))
whose payload is a **heatshrink-compressed** DRO stream — typically
30–40 % of the plain size. The runtime entry point is
`seq_play_stream(const opl_song_stream*, int loop)` instead of
`seq_play_song`, fed by the generic byte pump in
[`heatshrink/hs_stream.h`](heatshrink/hs_stream.h). Same sequencer,
same ISRs — only the byte source differs. See _Compressed songs_
below.

That's it. Two real-time entry points:

- **`seq_tick(1)`** in a 1 ms timer ISR â€” walks the song, fires due
  events with `OPL3_WriteRegBuffered`. O(1) amortised; cheap.
- **`synth_render_sample(&l, &r)`** in your DAC half-buffer-empty IRQ â€”
  produces one PCM frame.

### Songs

A song is a `static const opl_song` or `static const opl_song_hs` (in
a `*_song.h` file under [`songs/`](songs/)). All 41 songs shipped
with this repo are the heatshrink-packed `opl_song_hs` flavour;
plain `opl_song` is still supported for cases where you don't want
the decoder dependency.

The whole pipeline from a fresh game recording to a playable header
is one shell command per stage:

```
tools\vgz2dro.exe   songs\My_Song.vgz   songs\my_song.dro   (skip if you have .dro already)
tools\dro_loop3.exe songs\my_song.dro   songs\my_song.dro   (loop-point trim, optional)
tools\dro_opt.exe   songs\my_song.dro   songs\my_song.dro   (drop redundant writes, optional)
tools\dro2hdr.exe   songs\my_song.dro   songs\my_song_song.h  my_song  --hs
```

The `--hs` flag tells `dro2hdr` to heatshrink-pack the DRO payload
(default window=13, lookahead=4 — "w13/l4" — ~8 KB decoder window,
best-ratio choice for OPL register streams). Drop `--hs` to emit a
plain `opl_song` instead.

To re-pack **every** `songs/*.dro` in one go (e.g. after editing
encoder settings):

```
powershell -File tools\regen_songs.ps1
```

Then add one `#include` and one `SONGS[]` row in
[`demo_win/main.c`](demo_win/main.c) to expose the new song under a
friendly short name.

### Compressed songs (`opl_song_hs` + `hs_stream`)

Two delivery channels share the same player:

| Channel                    | Producer                           | Runtime                                                                         |
| -------------------------- | ---------------------------------- | ------------------------------------------------------------------------------- |
| **Embedded in flash**      | `dro2hdr --hs` -> `*_song.h`       | `#include` it, point an `hs_stream` at `song->hs_data`, call `seq_play_stream`. |
| **Loaded at runtime (PC)** | `dro_pack` -> `file.dro_hs<W>l<L>` | [`demo_win/dro_load.c`](demo_win/dro_load.c) `dro_load_hs()` does the slurp.    |

Both go through:

```
  bytes ?? hs_stream_next ??> heatshrink_decoder ??> seq_play_stream ??> OPL3
```

[`heatshrink/hs_stream.h`](heatshrink/hs_stream.h) is the only new
piece — a 3-function pump that knows nothing about OPL or DRO. It is
portable C99, ~80 lines, and is what you ship on the MCU alongside
`heatshrink_decoder.c`.

Loop-point detection (snipping out exactly one repetition of a song)
is done by `tools/dro_loop3.exe`, which finds the section markers in
the OPL register stream — see the comments in
[`tools/dro_loop3.c`](tools/dro_loop3.c) for the heuristic.

---

## Porting to a microcontroller

### Files to copy / link

| File                                                                | Why                                           |
| ------------------------------------------------------------------- | --------------------------------------------- |
| `<core>/opl3.c`, `<core>/opl3.h` (or `opal/opal.c` + `opal/opl3.h`) | Synth                                         |
| `sequencer/seq_player.c`, `sequencer/seq_player.h`                  | Sequencer (build with `-DSEQ_PLAYER_FIFO`)    |
| `songs/opl_song_hs.h`                                               | Descriptor type for packed songs              |
| `songs/<your_song>_song.h`                                          | The music                                     |
| `heatshrink/heatshrink_decoder.c`, `.h`                             | Streaming decompressor (only if using `--hs`) |
| `heatshrink/heatshrink_common.h`, `heatshrink_config.h`             | Decoder build-time config                     |
| `heatshrink/hs_stream.c`, `.h`                                      | Generic byte pump on top of the decoder       |

`<core>` is one of `nuked/`, `nuked_optimized/`, or `opal/`. The
sequencer is the same file in all three cases; you select the engine
by putting one core's folder on the include path and linking that
folder's `.c` files.

For the heatshrink decoder use the **static-allocation** flags so it
takes no malloc and one BSS-resident instance:

```
-DHEATSHRINK_DYNAMIC_ALLOC=0
-DHEATSHRINK_STATIC_WINDOW_BITS=13
-DHEATSHRINK_STATIC_LOOKAHEAD_BITS=4
-DHEATSHRINK_STATIC_INPUT_BUFFER_SIZE=64
```

That costs ~8.2 KB of BSS for the decoder window plus ~1.5 KB of
flash for the decoder code itself; in exchange every `*_song.h` in
this repo shrinks to 30–40 % of its plain size.

If you don't want to ship heatshrink at all, regenerate the songs
you need without `--hs` (`tools/dro2hdr.exe in.dro out.h sym`) and
call `seq_play_song()` instead of `seq_play_stream()`. The plain
path has zero new dependencies on top of the original sequencer.

Plus `cmsis_gcc.h` (from your CMSIS pack) and a small byte FIFO that
exposes the `fifo_init` / `fifo_put_buf` / `fifo_get_buf` /
`FIFO_FREECOUNT` interface used by `sequencer/seq_player.c` when
built with `-DSEQ_PLAYER_FIFO`.
Drop in your project's existing FIFO or vendor any single-producer /
single-consumer ring of your choice.

(If you'd rather start from the unmodified reference port, swap
`nuked_optimized/` for `nuked/` everywhere above; you then don't need
the MCU optimizations described below but everything still builds.
For a cheaper / smaller engine without rhythm-mode percussion, swap
in `opal/` instead.)

That's everything. No malloc, no stdio, no `FILE *`, no float math, no
threads. Pure C99.

### Footprint (`nuked_optimized/` on Cortex-M4, `-Os`)

- **Flash, code:** ~50 KB (Nuked-OPL3 + seq_player), plus ~1.5 KB if
  you link in the heatshrink decoder + `hs_stream`.
- **Flash, song (plain `opl_song`):** ~2 bytes per OPL register write —
  e.g. Prehistorik 2 loop ~41 KB, DOOM E1M1 ~34 KB, Wacky Wheels
  intro ~2.5 KB. Songs scale with length and density.
- **Flash, song (packed `opl_song_hs`, w13/l4):** typically
  30–40 % of the plain size. Across the 41 songs that ship in this
  repo the average ratio is **31 %** (2.7 MB plain ? 842 KB packed).
  Decoder needs ~8.2 KB BSS for its 8 KB sliding window.
- **RAM:** `sizeof(opl3_chip)` ? 14 KB (the bulk is the 1024-entry
  write buffer; you can shrink it by editing `OPL_WRITEBUF_SIZE` in
  `opl3.h` if you don't use buffered writes). Plus a few KB for the
  sample FIFO (`SEQ_FIFO_FRAMES` in `sequencer/seq_player.c`, default ~4 KB)
  and a few hundred bytes of player state.
- **CPU:** `OPL3_Generate` is roughly 350–500 cycles per stereo frame
  on Cortex-M4. At the native 49 716 Hz that's about 17–25 MHz of
  effective CPU — comfortable on STM32F4/F7/H7, RP2040, ESP32, etc.
  (The reference `nuked/` port is somewhat heavier and goes through
  `OPL3_GenerateResampled` instead; see _Sample-rate choices_ below.)

### Skeleton (`nuked_optimized/` on MCU, packed song)

```c
#include "seq_player.h"
#include "opl_song_hs.h"
#include "pre2_loop_song.h"           /* defines &pre2_loop (opl_song_hs) */
#include "heatshrink_decoder.h"
#include "hs_stream.h"

#define SAMPLE_RATE_HZ 49716   /* OPL3 native rate -- run as close to this as your DAC allows */

/*  One static decoder + one static stream is enough for the whole
    runtime; both are reused for every song you play.            */
static heatshrink_decoder g_dec;
static hs_stream          g_hs;
static opl_song_stream    g_stream;

static int  hs_byte (void* u)         { (void)u; return hs_stream_next(&g_hs); }
static void hs_rewind(void* u)        { (void)u; hs_stream_rewind(&g_hs);
                                        for (int i = 0; i < 26; i++) hs_stream_next(&g_hs); }

static void play(const opl_song_hs* s, int loop)
{
    hs_stream_init(&g_hs, s->hs_data, s->hs_len, &g_dec);
    for (int i = 0; i < 26; i++) hs_stream_next(&g_hs);   /* skip DRO header */
    g_stream = (opl_song_stream){
        .next_byte = hs_byte, .rewind = hs_rewind, .user = NULL,
        .codemap_len = s->codemap_len, .short_code = s->short_code,
        .long_code   = s->long_code,   .total_ms   = s->total_ms,
    };
    seq_play_stream(&g_stream, loop);
}

void boot(void)
{
    synth_init(SAMPLE_RATE_HZ);
    play(&pre2_loop, /*loop=*/1);

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
around the FIFO write — see `sequencer/seq_player.c`). On a
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

### The ones you'll always use

| Tool                  | What it does                                                                                                                                                                                            |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `tools\vgz2dro.exe`   | Converts a VGM/VGZ capture (e.g. from vgmrips.net) into DOSBox DRO v2 format — handy because most archived OPL music online is `.vgz`, not `.dro`.                                                      |
| `tools\dro_opt.exe`   | Drops register writes that don't change anything observable (re-writing the same value, writes shadowed by an immediately following one). Typically shaves 5–15 % off a capture before any compression. |
| `tools\dro_loop3.exe` | Finds the loop point (start of the second repetition) and writes a trimmed `.dro` containing exactly one loop iteration. Heuristic: scans the OPL register stream for the longest self-similar suffix.  |
| `tools\dro2hdr.exe`   | Converts a `.dro` into an embeddable C header. Pass `--hs` (default w13/l4) to heatshrink-pack the payload and emit an `opl_song_hs`; without it you get the plain `opl_song` form.                     |
| `tools\dro_pack.exe`  | Same heatshrink encoder as `dro2hdr --hs`, but writes a runtime-loadable `*.dro_hs<W>l<L>` file instead of a `.h`. Use this when you want to ship songs as data files (e.g. on an SD card).             |
| `tools\hs_bench.exe`  | Sweeps a `.dro` through every reasonable `(window, lookahead)` combination and reports the smallest output. Use it once per song corpus to pick the global default; w13/l4 is rarely beaten.            |

Typical pipeline for a looping song captured as VGZ:

```
tools\vgz2dro.exe   songs\My_Song.vgz   songs\my_song.dro
tools\dro_loop3.exe songs\my_song.dro   songs\my_song.dro
tools\dro_opt.exe   songs\my_song.dro   songs\my_song.dro
tools\dro2hdr.exe   songs\my_song.dro   songs\my_song_song.h  my_song  --hs
```

Then wire it into the demo by adding `#include "my_song_song.h"` and a
`SONGS[]` row in [`demo_win/main.c`](demo_win/main.c). To re-pack the
whole `songs/` folder in one shot run
`powershell -File tools\regen_songs.ps1`.

For a one-shot song the `dro_loop3` step is unnecessary — just slice
off the intro/outro silence with `dro_slice` (below) and feed the
result to `dro2hdr --hs`.

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

Most tools are plain C99 with no dependencies; rebuild any of them with:

```
gcc -O2 -Wall -Wextra -std=c99 tools\<name>.c -o tools\<name>.exe
```

The three that touch heatshrink (`dro2hdr`, `dro_pack`, `hs_bench`)
need the encoder linked in too:

```
gcc -O2 -Wall -Wextra -std=c99 -Iheatshrink -DHEATSHRINK_DYNAMIC_ALLOC=1 ^
    tools\dro2hdr.c heatshrink\heatshrink_encoder.c -o tools\dro2hdr.exe
```

---

## Acknowledgements & licenses

- **Nuked-OPL3** (`nuked/opl3.[ch]`) — © Nuke.YKT, **LGPL 2.1+**.
  Unmodified upstream. See the header in `nuked/opl3.h` for credits to
  the MAME team, OPLx decap project, and others whose work made the
  emulation possible.
- **heatshrink** (`heatshrink/heatshrink_*.[ch]`) — © Atomic Object,
  **ISC license**. Unmodified upstream from
  <https://github.com/atomicobject/heatshrink>. The `hs_stream.[ch]`
  pump on top of it is original to this repo and is under the same
  permissive terms as everything else here.
- **Game music captures** — the `.dro` / generated `.h` files in this
  repo are derived from games whose music remains © their respective
  rightsholders (Titus Interactive for _Prehistorik 2_, id Software for
  _DOOM_, Apogee for _Wacky Wheels_). They are included for personal /
  historical / educational use; do not redistribute. The original game
  binaries themselves are not in the repo — supply your own copy under
  `games/`.
- Everything else (sequencer, demo, tools) â€” do whatever you like.
