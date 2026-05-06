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
> convincingly with any other technique. With ~30–80 KB of flash for
> the code (depending on which core you pick) plus whatever your
> songs need, and one spare CPU, you can have authentic OPL3 audio on
> a $3 board.

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
|                                 (inline render *and* a producer/consumer
|                                  FIFO are both compiled in; pick one
|                                  per call site)
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
|   |   +-- adlibemu_opl3.c         Vendored from ValleyBell/libvgm (8-line OPL3 shim)
|   |   +-- adlibemu_opl_inc.[ch]   The actual emulator (shared OPL2/OPL3 body)
|   |   +-- adlibemu.h              Upstream public API
|   |   +-- adlibemu_compat.h       Tiny shim replacing libvgm's stdtype/snddef/common_def
|   |   +-- opl3.h                  header-only `OPL3_*` adapter (no .c shim)
|   |
|   +-- dbopl/                    DOSBox dbopl, C port (GPLv2 -- not for closed firmware)
|   |   +-- dbopl.c                 Hand-port of DOSBox's dbopl.cpp to plain C99
|   |   +-- opl3.h                  header-only `OPL3_*` adapter (no .c shim)
|   |   +-- LICENSE.txt
|   |
|   +-- dbopl_optimized/          MCU-tuned variant of the dbopl port (MCU-only build)
|       +-- dbopl.c                 Same C port + HOT_INLINE / HOT_FUNC (CCM placement)
|       +-- opl3.h                  header-only `OPL3_*` adapter (no .c shim)
|       +-- LICENSE.txt
|
+-- heatshrink/                 <-- portable streaming decompressor (PC + MCU)
|   +-- heatshrink_decoder.[ch]   Atomic Object's heatshrink (decoder unmodified)
|   +-- heatshrink_encoder.[ch]   (PC-only; only dro2hdr / hs_bench pull this in)
|   +-- heatshrink_config.h       Decoder build-time config (locally patched: HEATSHRINK_STATIC_* are #ifndef-guarded so -D overrides reach every TU)
|   +-- hs_stream.[ch]            Generic byte-stream pump on top of the decoder
|
+-- songs/                      <-- + at least one *_song.h
|   +-- opl_song_hs.h             Self-describing descriptor (hs_data + metadata)
|   +-- *_song.h                  39 ready-to-link songs, heatshrink-packed (w13/l4)
|   +-- *.dro                     Source DRO captures (input to dro2hdr)
|   +-- *.vgz                     Original VGM source files for the above
|
+-- demo_win/                   <-- Windows-only wrapper, NOT for the MCU
|   +-- main.c                    Fake-ISR loop + curated SONGS[] list
|   +-- audio.h                   Tiny audio-sink interface
|   +-- audio_win.c               WinMM (waveOut) implementation
|   +-- dro_load.[ch]             Load a .dro file at runtime (PC only)
|
+-- tools/                      <-- offline PC utilities
|   +-- vgz2dro.c     / .exe      VGM/VGZ capture -> DRO v2
|   +-- dro_opt.c     / .exe      Strip redundant register writes from a .dro
|   +-- dro_loop3.c   / .exe      Loop-point detector
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

Five engines are wired up today (`nuked_optimized` is an MCU-only
fork of `nuked`, and `dbopl_optimized` is an MCU-only fork of `dbopl`,
so they don't count as separate engines). All emulate the same Yamaha
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
| `nuked`           | LGPL 2.1+     | pure integer                              | yes    | yes  | **yes** (decap) |        7.5 KB |                                  0 | **20.5 KB**? |      481 |    42.5× |
| `nuked_optimized` | LGPL 2.1+     | pure integer                              | yes    | yes  | yes for OPL2    |         ~7 KB |                                  0 |      ~12 KB? |     297? |    68.8× |
| `opal`            | public domain | pure integer                              | **no** | yes  | no              |        6.4 KB |                                  0 |       5.7 KB |      222 |    92.4× |
| `adlibemu`        | LGPL 2.1+     | **`double` envelopes + libm `pow`/`sin`** | yes    | yes  | no              |       14.1 KB |           **14.6 KB** (wave + LFO) |       7.5 KB |      239 |    85.3× |
| `mame`            | BSD-style?    | integer (FP only at init)                 | yes    | yes  | no              |       11.3 KB | **58.0 KB** (`sin_tab` + `tl_tab`) |      13.8 KB |      192 |   106.0× |
| `dbopl`?          | **GPLv2**?    | integer (FP only at init)                 | yes    | yes  | no              |       13.1 KB |          9.2 KB (wave+ksl+tremolo) |   **4.7 KB** |   **85** | **240×** |

? `nuked_optimized` measured on the 9-ch / OPL2 / mono profile; the full
OPL3 build sits closer to plain `nuked` in both code size and ns/frame.
Per-chip RAM drops because the unused channels/operators are compiled out.
? MAME license, BSD-compatible commercial use; see
[`cores/mame/ymf262.c`](cores/mame/ymf262.c) header.
? `nuked` keeps its log-sin/exp ROM and per-operator scratch _inside_
`opl3_chip` (rather than as file-static globals), which is why its
per-instance footprint is the largest even though `.bss` is zero. With
one chip — the usual case — total RAM is ~20.5 KB.
? `dbopl` lives at [`cores/dbopl/dbopl.c`](cores/dbopl/dbopl.c) as a
hand-port of DOSBox's `dbopl.cpp` to plain C99 (templates lowered to
`static inline` + constant args, member function pointers replaced with
free-function pointers, namespaces stripped). The C port is **~20%
faster** than the C++ original at -O2 (85 vs 105 ns/frame on x64), with
identical output. **License inheritance: linking `cores/dbopl/dbopl.c`
into your build makes the resulting binary GPLv2.** The other four cores
(LGPL/BSD/public-domain) do not have this property -- pick whichever
matches your distribution requirements. See
[`cores/dbopl/LICENSE.txt`](cores/dbopl/LICENSE.txt).

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

- **`dbopl_optimized`** — _dbopl, MCU-tuned (GPLv2)._
  - - Same audible output as `dbopl`; same WAVE_TABLEMUL inner loop.
  - - Adds `HOT_INLINE` on hot helpers and `HOT_FUNC` (CCM placement)
      on the per-sample dispatch targets (`vol_*`, `synth_*`,
      `chip_generate_block2/3`, `OPL3_Generate*`, `op_set_state`).
  - - One build flag (`-DHOT_FUNC=...`) propagates the same CCM
      placement to the sequencer and to `nuked_optimized` -- pick
      a core, set the flag once, done.
  - ? Inherits dbopl's GPLv2; not for closed-source firmware.
  - ? MCU-only build (defaults assume the linker script defines `.ccm`).

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
  - - Fastest pure-integer core that doesn't carry a viral license.
  - - Inner loop is two unconditional table lookups (no branches, no fn ptrs).
  - - Pure integer at runtime; `double` only at one-shot table init.
  - ? ~58 KB of pre-flattened tables (`sin_tab` 32 KB + `tl_tab` 26 KB).
    Tight on 128/256 KB parts; comfortable on F4/F7/H7.
  - ? Not bit-exact (pre-Nuked envelope/phase model). Audibly indistinguishable.

- **`dbopl`** — _fastest, smallest, GPL-encumbered._
  - - **Fastest core in the repo** (~2.3× plain `mame`) thanks to a
      pre-multiplied wave table that turns the inner loop into one
      load + one multiply per operator, no envelope branches in the
      hot path.
  - - **Smallest per-chip RAM** (4.7 KB — less than `opal`).
  - - Pure integer at runtime; `double` only at one-shot table init.
      Builds clean with `arm-none-eabi-gcc -Os -mthumb` (no FPU needed).
  - - Plays everything: full OPL3, rhythm, 4-op, panning.
  - ? **GPLv2** — linking it makes your binary GPL. Don't pick this
    core if you ship closed-source firmware; use `mame` (BSD) or
    `adlibemu` (LGPL) instead.
  - ? Not bit-exact (DOSBox heuristic envelope model).
  - ? **~1.6× slower on percussion-heavy songs** than on pure
    melodic content (the `0xBD` rhythm-mode dispatcher bypasses
    the WAVE\*TABLEMUL hot path). On Cortex-M this can be the
    difference between meeting and missing the audio ISR deadline
    -- prefer `mame` for percussion-heavy material on tight MCUs.
    See [Measured CPU cost](#measured-cpu-cost) for numbers.

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
compromises were made in DOSBox-era emulation) and as the
LGPL-licensed alternative to `dbopl` (same family, ~3× slower but
you can ship it in closed firmware). About 25% slower than `mame` on
x64; would lose more ground on M4F because of the FP envelope
updates, and is unbuildable on M0.

#### `dbopl/` — fastest, GPL-encumbered

[`cores/dbopl/dbopl.c`](cores/dbopl/dbopl.c) is a hand-port of
DOSBox's `dbopl.cpp` to plain C99. The original was templated C++
with member function pointers, namespaces, and cross-channel
`this+1` pointer arithmetic; the C port lowers all of that to
`static inline` bodies dispatched by free-function pointers, with
`HOT_INLINE` (`__attribute__((always_inline))`) on the hot helpers
so `gcc -O2` specialises the synth handlers as cleanly as the C++
template instantiations did. **The C port runs ~20% faster than the
C++ original** at -O2 (85 vs 105 ns/frame on x64), with byte-identical
output.

Why it's so fast: dbopl uses the WAVE*TABLEMUL strategy — the
log-sin and exp tables are pre-multiplied at init into a single
~6 KB `MulTable` + 16 KB `WaveTable`. The per-operator inner loop
collapses to one indexed load and one multiply; no envelope branches,
no function calls per sample. That's also why the per-chip RAM is
the \_smallest* in the repo (4.7 KB) despite full OPL3 / rhythm /
4-op support — the big tables live as static globals shared by all
operators, not inside each `Chip`.

**License inheritance.** dbopl is GPLv2. Linking
[`cores/dbopl/dbopl.c`](cores/dbopl/dbopl.c) into your binary makes
the combined work GPLv2. If you ship closed-source firmware, pick
[`mame`](cores/mame/ymf262.c) (BSD-style, second fastest) or
[`adlibemu`](cores/adlibemu/adlibemu_opl3.c) (LGPL 2.1+, same
algorithmic family) instead. See
[`cores/dbopl/LICENSE.txt`](cores/dbopl/LICENSE.txt) for the full
story.

#### Measured CPU cost

<a id="measured-cpu-cost"></a>

Each `opl_demo_<core>.exe` accepts `--bench [seconds]`, which runs
the exact same per-sample render + 1 ms tick loop as live playback
but skips audio output and times it with `QueryPerformanceCounter`.
The number you get is the cost of `synth_render_sample` +
`seq_tick`, i.e. exactly what would run inside the MCU's DAC and
SysTick ISRs.

Sample run on a modern x64 laptop at 49 716 Hz stereo, 30 s per
song, 3 runs averaged (variance ? 3%). Two contrasting workloads:
`metallica` is heavy melodic OPL3 with **no rhythm mode**;
`eric` (Eric Prydz - Call on Me) is **percussion-heavy** (BD + SD

- HH on every beat through the `0xBD` rhythm path).

| core              | metallica ns/frame | eric ns/frame | percussion penalty | metallica realtime | eric realtime |
| ----------------- | -----------------: | ------------: | -----------------: | -----------------: | ------------: |
| `nuked`           |                481 |           454 |              0.94x |              42.5× |         44.4× |
| `nuked_optimized` |                297 |             - |                  - |              68.8× |             - |
| `adlibemu`        |                239 |           186 |              0.78x |              85.3× |        108.4× |
| `opal`            |                222 |           229 |              1.03x |              92.4× |         88.0× |
| `mame`            |                192 |           188 |              0.98x |             106.0× |        107.2× |
| `dbopl`           |             **73** |           116 |          **1.59x** |           **272×** |      **174×** |

The surprise is **`dbopl`'s 1.6x penalty on percussion** while the
other integer cores stay flat (or even speed up, in adlibemu's case,
because its FP envelope path runs less often when rhythm-mode
operators bypass envelope re-derivation). dbopl's hot path is the
WAVE\*TABLEMUL one-load-one-multiply trick; rhythm mode (`0xBD`
BD/SD/TT/TC/HH) routes through a separate dispatcher with extra
noise-LFSR updates, hi-hat XOR mixing and snare phase logic. On
x64 those branches and 64-bit shifts are essentially free; on
Cortex-M they cost real cycles. **For percussion-heavy songs on M4,
budget ~1.5x dbopl's metallica figure** -- which can push it from
"comfortable margin" into "misses ISR deadlines", as one MCU port
of this repo found out the hard way. `mame` is the safe pick for
percussion-heavy material on tight MCUs (only 2% slower on `eric`,
still BSD-licensed, second fastest overall).

(`nuked_optimized` measured separately on a 9-ch / OPL2 / mono
profile build; the OPL2 profile silently ignores OPL3 rhythm so
the `eric` cell would be apples-to-oranges. `dbopl` is a C port of
DOSBox's `dbopl.cpp`; the C version runs ~20% faster than the
original C++ at -O2 because gcc specialises the small `HOT_INLINE`
body wrappers as cleanly as it does template instantiations -- and
free-function pointers are slightly cheaper to dispatch than C++
member-function pointers.)

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

| If…                                                                  | Use                                                                                                                                                  |
| -------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| You want to verify "did I capture this song right?"                  | `nuked` — the audio reference                                                                                                                        |
| The song uses rhythm-mode percussion (id Software, Prehistorik 2, …) | `mame` (flattest cost), `adlibemu`, `nuked`, or `nuked_optimized`; `dbopl` works but pays a ~1.6× penalty on percussion (see CPU table); not `opal`. |
| You're on an MCU and ship GPL-compatible code                        | `dbopl_optimized` — fastest, smallest per-chip RAM, CCM-placed hot path                                                                              |
| You're on an MCU with ?512 KB flash and need a permissive license    | `mame` — second fastest, BSD-style                                                                                                                   |
| You're on an MCU with ?256 KB flash                                  | `opal` (no percussion) or `nuked_optimized` (default profile)                                                                                        |
| You're targeting strict bit-accuracy on an MCU                       | `nuked_optimized` with all defaults disabled, or plain `nuked` if you have the cycles                                                                |
| You want a DOSBox-era second opinion on tone (M4F or higher)         | `adlibemu` — LGPL alternative to `dbopl` from the same family                                                                                        |
| You need to A/B sound quality                                        | Build all five PC binaries, listen — `--once` makes it easy                                                                                          |

#### Why not other engines?

- **`ymfm`** (Aaron Giles, BSD-3-Clause). Modern C++17 unified Yamaha
  FM family. Broader chip coverage than this repo needs (we only
  want OPL3). Comparable speed to `mame`/`dbopl`. Porting cost: high
  (templates, `std::array`, references). Worth it only if you need
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
opl_demo_mame.exe  metallica --bench 30   render 30 s of song with no audio,
                                          print render time / CPU% (no real-time pacing)
```

The build produces one binary per PC-ready core:
`opl_demo_nuked.exe`, `opl_demo_opal.exe`, `opl_demo_mame.exe`,
`opl_demo_adlibemu.exe`, and `opl_demo_dbopl.exe`. All five take
the same command line; the banner prints `core: <name>` so you
know which engine you're hearing.

The curated short-name list lives at the top of
[`demo_win/main.c`](demo_win/main.c) (one `#include` + one row in
`SONGS[]` per song); add a row to expose any of the 39 packed songs
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

/* render path A -- inline; call directly in the audio ISR */
void synth_render_sample(int16_t *l, int16_t *r);

/* render path B -- producer/consumer FIFO; call A from the audio ISR
                    OR call B (and refill) from a low-priority task */
void synth_update_fifo(void);                          // producer
void synth_get_fifo_sample(int16_t *l, int16_t *r);    // consumer
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

That's it. Two real-time entry points (plus an optional third for the
FIFO render path):

- **`seq_tick(1)`** in a 1 ms timer ISR — walks the song, fires due
  events with `OPL3_WriteRegBuffered`. O(1) amortised; cheap.
- **`synth_render_sample(&l, &r)`** in your DAC half-buffer-empty IRQ —
  produces one PCM frame inline (calls `OPL3_GenerateResampled`).
  Simplest possible code path; fine when the audio IRQ has the cycles.
- **`synth_update_fifo()` + `synth_get_fifo_sample(&l, &r)`** — if you
  prefer to keep the audio IRQ to one FIFO pop and run the heavy
  synth in a lower-priority context, call `synth_update_fifo()` after
  each `seq_tick(1)` and `synth_get_fifo_sample()` from the DAC IRQ.
  On underrun the previous frame is held to avoid clicks. Both render
  paths are always compiled in; pick whichever you call from your
  audio ISR and ignore the other.

Both paths share two short critical sections inside `seq_player.c`
that must not be preempted by the audio ISR (the OPL register write
in `seq_tick`, and the ring slot/head publish in `synth_update_fifo`).
They are bracketed with `SEQ_ISR_DISABLE()` / `SEQ_ISR_ENABLE()`
macro hooks that default to no-ops; on a Cortex-M target wire them
to CMSIS on the compile command line:

```
-DSEQ_ISR_DISABLE=__disable_irq -DSEQ_ISR_ENABLE=__enable_irq
```

or, to mask only the audio IRQ vector:

```c
#define SEQ_ISR_DISABLE() NVIC_DisableIRQ(MY_AUDIO_IRQn)
#define SEQ_ISR_ENABLE()  NVIC_EnableIRQ(MY_AUDIO_IRQn)
```

### Songs

A song is a `static const opl_song` or `static const opl_song_hs` (in
a `*_song.h` file under [`songs/`](songs/)). All 39 songs shipped
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

There is exactly **one** delivery channel: `dro2hdr --hs` reads a
`.dro` and emits a `*_song.h` containing an `opl_song_hs` struct
(metadata) plus a heatshrink-compressed `codemap+stream` payload.
`#include` the header, point an `hs_stream` at `song->hs_data`,
call `seq_play_stream`. No malloc, no file I/O, no header parsing.

The payload is **header-stripped at compile time**: every field the
player needs at runtime (`codemap_len`, `short_code`, `long_code`,
`total_ms`) is already a field of the emitted `opl_song_hs` struct,
so the original 26-byte DRO v2 file header would be dead weight
inside the compressed stream. The first decompressed byte is
`codemap[0]`.

For PC-side ad-hoc playback the demo also accepts a `.dro` path on
the command line ([`demo_win/dro_load.c`](demo_win/dro_load.c)).
That path is uncompressed -- if you want compression at PC runtime,
build a `*_song.h` and link it. Keeping a single representation
end-to-end keeps the demo, and the docs, simple.

The decode pipeline:

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

| File                                                                | Why                                                     |
| ------------------------------------------------------------------- | ------------------------------------------------------- |
| `<core>/opl3.c`, `<core>/opl3.h` (or `opal/opal.c` + `opal/opl3.h`) | Synth                                                   |
| `sequencer/seq_player.c`, `sequencer/seq_player.h`                  | Sequencer (inline + FIFO render paths both compiled in) |
| `songs/opl_song_hs.h`                                               | Descriptor type for packed songs                        |
| `songs/<your_song>_song.h`                                          | The music                                               |
| `heatshrink/heatshrink_decoder.c`, `.h`                             | Streaming decompressor (only if using `--hs`)           |
| `heatshrink/heatshrink_common.h`, `heatshrink_config.h`             | Decoder build-time config                               |
| `heatshrink/hs_stream.c`, `.h`                                      | Generic byte pump on top of the decoder                 |

`<core>` is any of `nuked/`, `nuked_optimized/`, `opal/`,
`mame/`, `adlibemu/`, `dbopl/`, or `dbopl_optimized/`. The sequencer
is the same file in every case; you select the engine by putting one
core's folder on the include path and linking that folder's `.c` files.
See [Cores at a glance](#cores-at-a-glance) for footprint / license
/ feature tradeoffs.

For the heatshrink decoder use the **static-allocation** flags so it
takes no malloc and one BSS-resident instance:

```
-DHEATSHRINK_DYNAMIC_ALLOC=0
-DHEATSHRINK_STATIC_WINDOW_BITS=13
-DHEATSHRINK_STATIC_LOOKAHEAD_BITS=4
-DHEATSHRINK_STATIC_INPUT_BUFFER_SIZE=64
```

> **Critical: every TU that sees `heatshrink_decoder.h` must be
> compiled with the same `HEATSHRINK_STATIC_WINDOW_BITS` value used
> when the song was packed.** The struct embeds a fixed-size window
> buffer whose size is derived from this macro, so a mismatch
> silently shrinks the buffer in some translation units while the
> decoder code keeps writing past it. The result is a song that
> plays correctly for the first few hundred bytes (small backrefs
> stay within the truncated window) and then degrades into garbage
> as soon as the encoder emits a backref past the truncated window
> size. Put the `-D` flags into your top-level Makefile's global
> `CFLAGS`/`UDEFS`, not in a per-file rule.

That costs ~8.2 KB of BSS for the decoder window plus ~1.5 KB of
flash for the decoder code itself; in exchange every `*_song.h` in
this repo shrinks to 30–40 % of its plain size.

### CCM / fast-RAM placement (`HOT_FUNC`)

The `_optimized` cores (`nuked_optimized/`, `dbopl_optimized/`) and
the sequencer's audio-ISR-side render helpers all funnel their hot
functions through a single overridable macro:

```
#ifndef HOT_FUNC
  #if defined(__GNUC__) || defined(__clang__)
    #define HOT_FUNC __attribute__((section(".ccm")))
  #else
    #define HOT_FUNC
  #endif
#endif
```

Default on GCC/Clang is `.ccm` (STM32F3/F4/G4 Core-Coupled Memory);
on other toolchains it silently degrades to a no-op and the function
lands in flash like normal. Override it once at the top-level
Makefile to point at whatever fast-RAM section your linker script
defines, e.g.

```
'-DHOT_FUNC=__attribute__((section(".ccm")))'
```

or disable it entirely with `-DHOT_FUNC=`. The same flag covers
[`sequencer/seq_player.c`](sequencer/seq_player.c) (`synth_render_sample`,
`synth_update_fifo`, `synth_get_fifo_sample`),
[`cores/nuked_optimized/opl3.c`](cores/nuked_optimized/opl3.c)
(~26 hot functions) and
[`cores/dbopl_optimized/dbopl.c`](cores/dbopl_optimized/dbopl.c)
(~22 hot functions) -- pick a core, set the flag once, done.

Why it matters on Cortex-M: when DMA, the audio ISR and the synth
render loop all contend for the single flash bus, the synth gets
stalled waiting for instruction fetches. Moving the per-sample hot
path into CCM removes the contention; on STM32G4 @ 144 MHz this is
the difference between "keeps up" and "drops the occasional sample
in dense passages".

If your linker script has no `.ccm` section the link will fail at
the orphan-section warning -- either add one, change the section
name (`-DHOT_FUNC='__attribute__((section(".my_fast_ram")))'`), or
disable the placement (`-DHOT_FUNC=`).

If you don't want to ship heatshrink at all, regenerate the songs
you need without `--hs` (`tools/dro2hdr.exe in.dro out.h sym`) and
call `seq_play_song()` instead of `seq_play_stream()`. The plain
path has zero new dependencies on top of the original sequencer.

The FIFO render path is built in unconditionally and uses a local
lock-free SPSC ring of stereo frames (no external `fifo.h`, no CMSIS
dependency, no malloc). Tune its size with `-DSEQ_FIFO_FRAMES=N`
(power of two; defaults to 1024 frames ? 20 ms @ 49 716 Hz). Wire
the `SEQ_ISR_DISABLE` / `SEQ_ISR_ENABLE` hooks to whatever your
platform uses to mask the audio IRQ — see the snippet at the top of
this section.

(If you'd rather start from the unmodified reference port, swap
`nuked_optimized/` for `nuked/` everywhere above; you then don't need
the MCU optimizations described below but everything still builds.
For a cheaper / smaller engine without rhythm-mode percussion, swap
in `opal/` instead.)

That's everything. No malloc, no stdio, no `FILE *`, no float math, no
threads. Pure C99.

### Footprint

Per-core `.text` / `.bss` / per-chip RAM / cycles-per-frame are in
[Cores at a glance](#cores-at-a-glance) and the
[Measured CPU cost](#measured-cpu-cost) table; pick a core there first
and bring its numbers down here. The fixed costs that are the
**same across cores** are:

- **Flash, sequencer:** ~3 KB (`seq_player.c`, both render paths
  compiled in).
- **Flash, heatshrink decoder + `hs_stream`:** ~1.5 KB code +
  ~8.2 KB BSS for the 8 KB sliding window (`HEATSHRINK_STATIC_*`
  flags above). Skip both if you only use plain `opl_song`.
- **Flash, song (plain `opl_song`):** ~2 bytes per OPL register
  write — e.g. Prehistorik 2 loop ~41 KB, DOOM E1M1 ~34 KB, Wacky
  Wheels intro ~2.5 KB. Songs scale with length and density.
- **Flash, song (packed `opl_song_hs`, w13/l4):** typically
  30–40 % of the plain size. Across the 39 songs that ship in this
  repo the average ratio is **~31 %**.
- **RAM, sample FIFO:** `SEQ_FIFO_FRAMES * 4` bytes (default 1024
  frames = 4 KB). Tune with `-DSEQ_FIFO_FRAMES=N`.
- **RAM, player state:** a few hundred bytes inside `seq_player.c`.

The one variable cost is `sizeof(opl3_chip)` and the per-frame
cycle count, both of which are core-dependent — see the tables
above. As a rough guide, the cheapest combo (`dbopl` + plain
`opl_song`) fits in **~30 KB code + ~5 KB RAM per chip** and
renders at ~85 ns/frame on x64.

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
static void hs_rewind(void* u)        { (void)u; hs_stream_rewind(&g_hs); }

static void play(const opl_song_hs* s, int loop)
{
    hs_stream_init(&g_hs, s->hs_data, s->hs_len, &g_dec);
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
    synth_update_fifo();              // refill the sample FIFO
}

void on_dac_sample(void)              // ~50 kHz high-priority DAC ISR
{
    int16_t l, r;
    synth_get_fifo_sample(&l, &r);    // O(1): pop one frame from the FIFO
    dac_write(l, r);                  // or  dac_mono((l + r) >> 1);
}
```

The sample FIFO between `synth_update_fifo()` and
`synth_get_fifo_sample()` is what makes this safe: the DAC ISR is
guaranteed to find a frame ready every time, no matter how long the
previous `seq_tick()` took. The two ISRs share only the `opl3_chip`
struct and the FIFO; on a single-core MCU the `SEQ_ISR_DISABLE` /
`SEQ_ISR_ENABLE` hooks (see _Files to copy / link_ above) handle
the two short critical sections. On a multi-core MCU put both ISRs
on the same core.

If you'd rather skip the FIFO and call the synth straight from the
DAC IRQ, use `synth_render_sample(&l, &r)` in `on_dac_sample` and
remove the `synth_update_fifo()` call from `on_systick_1ms`. The ISR
guard hooks still apply (they protect the OPL register write inside
`seq_tick` from the inline render in the DAC ISR).

### Sample-rate choices and the FIFO

The recommended MCU layout keeps the audio ISR **as cheap as
physically possible**: it pops one stereo frame from a small ring and
returns. All synth work — the expensive part — happens in a
lower-priority context that calls `synth_update_fifo()` to refill the ring
whenever it has spare time.

```
  low priority                                high priority
  --------------------------------            -------------------------
  seq_tick(1)        ? OPL register writes
  synth_update_fifo()? OPL3_Generate ? push   pop ? synth_get_fifo_sample()
                          frames into FIFO            ?
                                                    DAC / I?S / PWM
```

This is why the FIFO render path is what you want on the MCU: the
chip is clocked at its native 49 716 Hz and the audio ISR just pops
ready frames. The choice of generator (resampled vs native) is a
compile-time switch in [`sequencer/seq_player.c`](sequencer/seq_player.c)
that applies to **both** `synth_render_sample` and `synth_update_fifo`:

- `-DSEQ_RESAMPLE=1` (default) -> `OPL3_GenerateResampled()` at the
  `sample_rate_hz` you passed to `synth_init()`. Convenient on PC.
- `-DSEQ_RESAMPLE=0` -> `OPL3_Generate()` at the chip's native rate.
  Cheaper, bit-exact w.r.t. the underlying core, recommended for
  embedded targets that can clock the DAC near 49 716 Hz.

Keeping both render paths on the same generator means you can A/B
the inline path against the FIFO path without an apples-to-oranges
resampler difference.

If your DAC clock can't hit 49 716 Hz exactly:

- **48 000 Hz** is essentially indistinguishable and the easiest
  divider on most modern audio peripherals.
- **44 100 Hz** is fine — a fraction of a percent off pitch, no other
  audible effect.
- **24 858 Hz** (= 49 716 / 2) is the FALCON build's choice; halves the
  per-frame CPU at a tiny aliasing cost on the highest OPL voices.
- **Anything below ~22 kHz** — leave `SEQ_RESAMPLE=1` (the default)
  and let `OPL3_GenerateResampled` do the work.

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
| `tools\dro_dump.exe file.dro [start_ev] [count]`    | Pretty-print the decoded register-write stream with timestamps. Used to eyeball where a section starts.                    |
| `tools\dro_segments.exe file.dro`                   | Splits the timeline at long silent gaps; useful for separating stinger / loop / outro within a single capture.             |
| `tools\dro_perc.exe file.dro`                       | Lists every percussion strike with its timestamp — handy for finding bar boundaries in songs whose drums lock to the beat. |
| `tools\dro_trigs.exe file.dro`                      | Reports register writes that look like loop / cue triggers (e.g. abrupt instrument changes).                               |
| `tools\dro_loopfind.exe file.dro loop_start_ms [N]` | Given a candidate loop start, scores how well the tail repeats it. Use to validate / nudge what `dro_loop3` produced.      |

### Surgery helpers

These rewrite the file.

| Tool                                                   | Purpose                                                                                                                         |
| ------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------- |
| `tools\dro_slice.exe in.dro out.dro start_ms end_ms`   | Cuts out a `[start_ms, end_ms)` window. Use to crop intros/outros once `dro_dump` told you where to cut.                        |
| `tools\dro_evrange.exe in.dro out.dro start_ev end_ev` | Same idea but in event-index space.                                                                                             |
| `tools\dro_loop.exe`, `dro_loop2.exe`, `dro_loop3.exe` | Three generations of the loop-finder; `dro_loop3` is the current default. The older ones are kept for diffing on tricky inputs. |

### Building the tools

Most tools are plain C99 with no dependencies; rebuild any of them with:

```
gcc -O2 -Wall -Wextra -std=c99 tools\<name>.c -o tools\<name>.exe
```

The two that touch heatshrink (`dro2hdr`, `hs_bench`) need the
encoder linked in too:

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
  **ISC license**. Decoder/encoder taken unmodified from
  <https://github.com/atomicobject/heatshrink>; `heatshrink_config.h`
  carries one local patch that `#ifndef`-guards the
  `HEATSHRINK_STATIC_*` macros so `-D` overrides actually reach every
  translation unit (see commit `bcb7ded` for the failure mode it
  fixes). The `hs_stream.[ch]` pump on top of it is original to this
  repo and is under the same permissive terms as everything else here.
- **Game music captures** — the `.dro` / generated `.h` files in this
  repo are derived from games whose music remains © their respective
  rightsholders (Titus Interactive for _Prehistorik 2_, id Software for
  _DOOM_, Apogee for _Wacky Wheels_). They are included for personal /
  historical / educational use; do not redistribute. The original game
  binaries themselves are not in the repo — supply your own copy under
  `games/`.
- Everything else (sequencer, demo, tools) â€” do whatever you like.
