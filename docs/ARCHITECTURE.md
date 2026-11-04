# KickEQ architecture

Status: design + implementation spec. Version 1.0 target. Sections marked **[done]** exist on `main`;
everything else is the specification the implementation is built and tested against.

KickEQ is the first first-party audio effect of the Kick ecosystem (Kick DAW/sampler, Akai MPC Key 61,
mobile app) and the pilot of the Kick plugin template. It is developed in its own repository, builds
without the Kick sources, and ships as **VST3 + CLAP** from one code base.

## 1. Requirements

Mandatory targets (owner decision, 2026-11-04):

| Target | Notes |
|--------|-------|
| Akai MPC Key 61 | Linux ARMv7 hard-float (`arm-linux-gnueabihf`), NEON, 2 isolated cores, no GUI toolkit assumptions |
| Kick mobile app | iOS and Android (arm64; Android also armeabi-v7a). Downloaded native code is restricted by store rules, so the plugin must be **statically linkable** into the app |
| Desktop | macOS (arm64 + x86_64), Windows x64, Linux x86_64 and AArch64 |

Consequences, which drive the whole design:

1. The DSP is a platform- and UI-independent **static C++17 library** (`kickeq_core`): no dependencies beyond
   the C++ standard library, no globals/statics with constructors, no exceptions or RTTI, no heap
   allocation and no locks on the audio thread, no `-ffast-math`.
2. The GUI is a separate layer. The parameter description is data (`core/params.h`) so Kick's own UI can
   render the plugin on the MPC and on phones without any native plugin GUI.
3. The same sources build as a static library **and** as a dynamic `.clap`.
4. Metadata (id, version, licence, builds) lives in a manifest so the plugin behaves exactly like a
   downloadable plugin even when it is compiled into the app (`PluginManifest`, `StaticPluginLoader`,
   `DynamicPluginLoader` on the Kick side).

## 2. Layers and repository layout

```
core/                 kickeq_core        STATIC, C++17, no dependencies            (portable everywhere)
  include/kickeq/     params.h  biquad.h  eq_engine.h  eq_curve.h  analyzer.h  fft.h  state.h  presets.h
  src/
  tests/              unit tests (ctest, own minimal runner, no framework dependency)
clap/                 kickeq_clap_native native CLAP shell over the core; STATIC + MODULE (.clap)
                      depends only on the CLAP headers (MIT); used on MPC, mobile, static loader, CI
juce/                 desktop shell: juce::AudioProcessor + editor (spectrum, curve)
                      builds VST3 and, through clap-juce-extensions, a CLAP with clap.gui
tools/                clap_testhost (dlopen host used by conformance tests), bench (CPU benchmark)
cmake/toolchains/     armv7-linux-gnueabihf, aarch64-linux-gnu, android, ios
manifest/             kickeq.manifest.json.in
docs/                 this file
scripts/              build.sh, package.sh
```

Dependency direction: `juce/` and `clap/` depend on `core/`; `core/` depends on nothing.
Each shell is thin: it maps host parameter/event/transport APIs onto `kickeq::EqEngine` and
`kickeq::ParamTable`, nothing else.

### 2.1 Why two shells (and when they converge)

* The **native CLAP shell** is the primary format. It has no GUI toolkit, no X11/ALSA/freetype
  dependency, and is what the MPC, the mobile app and the `StaticPluginLoader` use.
* The **JUCE shell** gives desktop users the editor (spectrum + interactive curve) in VST3 and in the
  CLAP (`clap.gui`) via the well-trodden JUCE path. JUCE needs X11/fontconfig/freetype on Linux, which the
  MPC image cannot be assumed to provide, and does not fit a static iOS/Android library.
* Both shells use the identical `ParamTable`, the identical state blob (§8) and the identical engine,
  so they cannot drift in behaviour. A conformance test (§11) loads each `.clap` through `clap_testhost`
  and requires bit-identical output for the same parameter set.
* Only one of the two CLAPs is installed on a given machine (same CLAP id `com.safian.kickeq`).
  Host-side automation ids differ between the shells (JUCE hashes string ids); projects exchanged across
  machines carry the **state blob**, which is shell-independent.
* Convergence criterion: when the native shell can host an editor through its own `clap.gui`
  implementation on macOS/Windows/Linux X11 and passes the same DAW checklist, the JUCE shell is reduced
  to VST3-only or replaced by `clap-wrapper` over the native CLAP. Until then nothing is removed.

## 3. Parameters (`core/params.h`)

All parameters are described by a `constexpr` table; shells and UIs only iterate it.

```
struct ParamInfo {
  uint32_t id;            // stable numeric id (CLAP param id); never reused
  const char* strId;      // stable string id ("b2_freq"), used by JUCE
  const char* name;       // "Band 3 Frequency"
  const char* module;     // "Band 3"  (CLAP module path, UI grouping)
  Kind kind;              // Float | Bool | Enum
  Scale scale;            // Linear | Log | Db
  float min, max, def;
  const char* unit;       // "Hz", "dB", ""
  const char* const* enumLabels; int numEnumLabels;
  uint32_t flags;         // automatable, stepped, hidden-in-simple-view
};
```

Helpers: `toNormalized`, `fromNormalized`, `valueToText`, `textToValue`, `clamp`.

### 3.1 Parameter set (v1)

Global:

| id | strId | kind | range | default |
|----|-------|------|-------|---------|
| 0 | `bypass` | bool | | off |
| 1 | `out_gain` | float dB | -24 .. +24 | 0 |
| 2 | `auto_gain` | bool | | off |
| 3 | reserved: `phase_mode` (Minimum / Linear), see §4.7 | | | |

8 bands; band `b` (0..7) uses ids `0x100 + 0x10*b + k`:

| k | strId | kind | range | default |
|---|-------|------|-------|---------|
| 0 | `b<b>_on` | bool | | off |
| 1 | `b<b>_type` | enum | Bell, Low Shelf, High Shelf, Low Cut, High Cut, Notch, Band Pass | per band, below |
| 2 | `b<b>_freq` | float Hz, log | 10 .. 30000 (engine clamps to 0.49 x fs) | per band |
| 3 | `b<b>_gain` | float dB | -24 .. +24 (ignored by Low/High Cut, Notch, Band Pass) | 0 |
| 4 | `b<b>_q` | float, log | 0.1 .. 18 | 1.0 (shelves 0.71, cuts 0.707) |
| 5 | `b<b>_place` | enum | Stereo, Left, Right, Mid, Side | Stereo |
| 6 | `b<b>_slope` | enum | 6, 12, 18, 24, 36, 48 dB/oct (cuts only) | 12 |

Default layout (all bands disabled, so a fresh instance is bit-transparent):
b0 Low Cut 30 Hz, b1 Low Shelf 100 Hz, b2 Bell 250 Hz, b3 Bell 800 Hz, b4 Bell 2.5 kHz, b5 Bell 6 kHz,
b6 High Shelf 10 kHz, b7 High Cut 18 kHz.

The 4-band parameter set of the 0.1.0 spike is replaced; no released version depends on it.

### 3.2 Mapping to Kick's own UI

`ParamTable` carries everything a generic or custom UI needs (grouping, units, ranges, enum labels,
text conversion). Through CLAP it is exposed with `clap.params` (names, modules, text conversion) and
`clap.remote-controls`: pages of 8 controls (one page per band: on/type/freq/gain/Q/place/slope + a global
page) which map directly onto the MPC Q-Links. The spectrum can be drawn by the Kick UI through a vendor
extension (§5.5), so the MPC and the phone need no `clap.gui`.

## 4. DSP (`kickeq_core`)

### 4.1 Filter design

* Second-order sections, **RBJ "Audio EQ Cookbook"** formulas with the centre frequency pre-warped
  exactly (bilinear transform): the design gain at `fc` is exact; the response above ~0.3 x fs shows the
  usual bilinear frequency cramping. All coefficient computation is in `double`.
* Bell, Low/High Shelf, Notch, Band Pass: one section each. Low/High Cut: Butterworth cascades;
  slope 6 dB/oct = one first-order section, 12 = one biquad, 18 = first-order + biquad, 24/36/48 = 2/3/4
  biquads with the Butterworth pole Q values (`Q_k = 1 / (2 cos((2k-1) pi / (2n)))`); the band's Q
  parameter scales the resonance of the (last) pair only for the 12 dB/oct case and is otherwise ignored,
  documented in the UI.
* Frequency is clamped to `[10 Hz, 0.49 fs]`, Q to `[0.05, 20]`, gain to `[-48, +48] dB` inside the design
  function, independently of the parameter range, so no input can produce unstable coefficients.
* Worst case section count: 8 bands x 4 sections = 32 (all cuts at 48 dB/oct); typical 8.

### 4.2 Filter structure

Transposed direct form II, `double` state, scalar. On ARMv7 the VFP double path is used (NEON has no
double SIMD); the cost is bounded by the section count above (§9). A float/NEON path is only introduced
if the MPC benchmark misses its budget. Denormals: the shell enables FTZ/DAZ (`MXCSR` on x86, `FPSCR.FZ`
on ARM) and the filter additionally flushes state below `1e-30`.
Each section keeps independent state per channel (L, R, M, S as needed).

### 4.3 Parameter smoothing

* Targets are read once per block from the shell (atomics or event queue).
* Per band, frequency (multiplicative), gain (linear dB) and Q (multiplicative) go through a 30 ms
  smoother; coefficients are recomputed every 32 samples **only while a smoother is active**. At rest the
  audio path does no trigonometry.
* Band enable/disable and type changes crossfade over 20 ms by running the section's output into a
  wet/dry mix of that band (the filter state is zeroed only when it is fully off).
* Bypass is a 20 ms linear cross-fade between processed and dry signal [done in the spike]; in linear-phase
  mode the dry path is delayed by the reported latency.
* Block size independence: `process(n)` may be called with any `n >= 0` and any alignment; internal chunking
  uses a preallocated scratch of fixed size.

### 4.4 Stereo, mid/side

Each band has a placement: Stereo, Left, Right, Mid, Side. Because every section is linear and
time-invariant (within one coefficient set), the engine uses a fixed signal flow instead of running
a matrix per band:

```
in -> [Stereo bands, identical on L and R] -> [Left / Right bands on their channel]
   -> M/S encode -> [Mid bands on M, Side bands on S] -> M/S decode -> out
```

`M = (L + R) / 2`, `S = (L - R) / 2`, decode `L = M + S`, `R = M - S`: with no active Mid/Side band the
encode/decode pair is skipped entirely and with flat Mid/Side bands it is the identity to within one ulp.
The displayed curve for a channel view (L, R, M, S) is computed with the same flow (§4.6), so the curve
always matches what is heard. Mono input is processed as dual-mono.

### 4.5 Auto-gain

`auto_gain` applies `-G` to the output where `G` is the pink-noise-weighted RMS gain of the current
response: `G = 10 log10( sum_i w_i |H(f_i)|^2 / sum_i w_i )`, 96 log-spaced points over 20 Hz .. 20 kHz,
`w_i` = width of the log bin (equal energy per octave). It is recomputed on the control side whenever the
target coefficients change, clamped to +-12 dB, and goes through the same 30 ms smoother as `out_gain`.
Cost is negligible (96 complex evaluations per band set change).

### 4.6 Analytic response (`eq_curve.h`)

`computeResponse(ParamSnapshot, sampleRate, const float* freqs, float* magDb, float* phaseRad, n, ChannelView)`
evaluates the cascade analytically at arbitrary frequencies. It is the single source for the UI curve,
auto-gain, linear-phase IR design, and the unit tests (the biquad itself is verified against it by running
sine sweeps).

### 4.7 Linear phase (optional, `KICKEQ_LINEAR_PHASE`)

Parameter 3 `phase_mode` (Minimum / Linear) is reserved. Specification for the implementation:

* Target magnitude `|H(f)|` from §4.6 on the FFT grid, zero phase -> IFFT -> shift by `N/2` -> window
  (Blackman) -> FIR of length `N`. `N` = 2048 / 4096 / 8192 (quality), latency `N/2` samples (21 / 43 / 85
  ms at 48 kHz), reported with `clap.latency` and `clap_host_latency.changed()` followed by a restart
  request, as required by the CLAP spec.
* Uniform-partitioned FFT convolution (overlap-save), all buffers preallocated in `prepare()`.
* IR recomputation happens **off the audio thread** (worker owned by the shell: JUCE thread / `clap.thread-pool`
  or a dedicated thread; on the MPC a single low-priority thread), handed over through a lock-free
  triple buffer; the audio thread cross-fades between the old and the new convolver output over one block.
* M/S placements are supported by running the convolution on L/R, M/S as in §4.4.
* The feature is gated by a CMake option; it is `OFF` for the MPC build until the CPU benchmark shows it
  fits, and the parameter is then reported as `CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY`.
* Offline rendering (`clap.render` offline) selects the highest quality.

### 4.8 Oversampling

All v1 processing is linear, so oversampling is not used; it would only mask the bilinear cramping
near Nyquist at the cost of CPU and latency, which is the wrong trade on the MPC. If a non-linear element
(saturation, "analog" mode) is added later, it runs inside a 2x/4x polyphase half-band oversampler that
is part of that element only, with its latency included in the reported latency; the EQ sections stay at
the base rate.

## 5. Spectrum analyser

### 5.1 Data path

```
audio thread:  pre  (input)  -> mono sum -> wait-free SPSC ring (float, 2^16)   [only if analyzer enabled]
               post (output) -> mono sum -> wait-free SPSC ring
analyser thread (UI/timer thread, never the audio thread):
               pull -> Blackman-Harris window (N = 4096, hop = 1024) -> real FFT -> power
               -> ballistics (fast attack, ~300 ms release, optional peak hold)
               -> 1/6-octave smoothing -> log-frequency display bins -> SpectrumFrame
UI:            reads the latest SpectrumFrame (double-buffered, atomic index swap) at 30..60 fps
```

* `kickeq::SpectrumAnalyzer` (in `core`) owns the rings and all buffers (allocated at construction/`prepare`).
  `push(pre, post, n)` is the only audio-thread call: wait-free, drops samples on overflow, does nothing
  when `setEnabled(false)`. `pull(SpectrumFrame&)` and `process()` run on the analyser thread.
* The core creates no threads; the shell (or the Kick UI) calls `process()` from its own timer. A headless
  instance (MPC without UI) therefore has zero analyser cost.
* FFT: own radix-2 real FFT (`fft.h`), float, precomputed twiddles, verified against a naive DFT; no
  dependency on KissFFT/FFTW/vDSP. NEON-friendly layout, optional.
* Pre/post are shown simultaneously (pre filled grey, post as line); the analyser input can be L+R, Mid, or Side.

### 5.2 Display

Frequency axis 10 Hz .. 30 kHz (log), analyser range -96 .. 0 dBFS (selectable), curve range
+-6/12/24/30 dB, slope compensation 0 / 3 / 4.5 dB/oct for readable spectra. These are editor state,
not automatable parameters (stored in a state extension chunk, §8).

### 5.3 Vendor extension for non-native UIs

`com.safian.kickeq/analyzer/1` (`get_extension` on the plugin): `set_enabled(bool)`, `get_frame(float* dbBins, uint32_t n)`
returns the latest `n` display bins. Called from the host's main/UI thread. This lets the Kick UI draw
the spectrum and the EQ curve (via `eq_curve.h` + the parameter values) with its own widgets on the MPC and
on phones.

## 6. GUI (desktop shell)

* **JUCE 8 (AGPL option)** with software rendering (cached `Path`s, 30 Hz repaint of the analyser layer
  only). OpenGL is not used (broken or absent on several hosts and Linux/Wayland setups).
* Editor: analyser + curve canvas (draggable band nodes: x = frequency, y = gain, mouse wheel = Q,
  double-click adds/removes a band, right-click = type/placement menu), a per-band strip, output gain,
  auto-gain, bypass, analyser controls, preset menu. Resizable, HiDPI-aware (`clap.gui` scale handling
  per spec: do not call `set_scale` on Cocoa).
* Parameter gestures use `juce::ParameterAttachment` (`begin/endChangeGesture`) so DAWs record automation
  touch correctly.
* Spike experience: JUCE 8.0.17 + clap-juce-extensions (pinned commit `7adee3a1`) give a working VST3 +
  CLAP (`clap.gui` embedded where the host supports it) from one `AudioProcessor`; the generic editor was
  verified only with pluginval, not inside a DAW. On Linux the CLAP wrapper embeds via X11 only (no
  Wayland); the editor then opens floating or falls back to the host's generic UI. Platforms without
  `clap.gui` support (MPC, mobile) never load this layer.

### 6.1 JUCE shell implementation notes (as built)

* **Parameters** are generated from `kickeq::paramTable` (`juce/CoreParameter.h`), strId as `ParameterID` version 1,
  the three global parameters at the top level and every band as a parameter group (CLAP module / VST3 unit
  "Band N"). `CoreParameter` is a `RangedAudioParameter` whose `NormalisableRange` calls the core's
  `toNormalized` / `fromNormalized`; text is the core's `valueToText` / `textToValue`. It keeps the **plain value**
  in an atomic instead of the stock float normalised value, so a state blob loads with bit-identical values (the
  audio thread reads exactly what the native shell reads) and a host echoing the normalised value does not move it.
  The bypass parameter is the host bypass parameter.
* **Processor**: `EqEngine` (heap, once) fed from the parameter atomics at the start of each block; stereo and
  mono (`isBusesLayoutSupported`: stereo, or mono <-> mono); `ScopedNoDenormals`; tail from the engine; latency 0;
  the analyser is fed only while an editor holds it (`acquireAnalyzer` / `releaseAnalyzer`).
* **State** is the core blob (not APVTS XML). The `UI01` chunk is 5 bytes (`juce/EditorSettings.h`): layout version
  1, analyser source, analyser range index, slope compensation index, curve range index; anything missing or out of
  range falls back to the defaults. A blob from the native shell (no chunk) loads with default editor settings.
  Factory presets are the JUCE programs; `getCurrentProgram` reports the preset whose values equal the parameters.
* **CLAP wrapper**: built with `CLAP_PROCESS_EVENTS_RESOLUTION_SAMPLES 64` and `CLAP_USE_JUCE_PARAMETER_RANGES ALL`
  (hosts see Hz / dB / enum indices, not 0..1). The wrapper does not split a block exactly at a parameter event but
  in sub-blocks that are multiples of 64 samples (the events inside a sub-block are applied at its start), and it
  passes event values through a float normalised number; the conformance profile (`clap_testhost --juce`) reproduces
  that rule for its reference engine. Without events the JUCE CLAP is bit-identical to the native CLAP.
* **Editor** (`juce/Editor*.{h,cpp}`): `EqCanvas` (grid and labels cached in an `Image` per size / scale / range,
  curve paths rebuilt only when the parameters, view, sample rate or size change, analyser paths per frame),
  `BandPanel` (chips + attached controls), `KickEQEditor` (30 Hz timer: `process()` + `pull()`, curve refresh,
  control sync). Global controls sit in a header bar and the analyser / curve selectors in a toolbar above the canvas
  (not in the bottom strip) so that everything stays legible at the 720 x 420 minimum.

## 7. CLAP integration (native shell and JUCE shell)

| Extension | Behaviour |
|-----------|-----------|
| `clap.audio-ports` | 1 stereo in, 1 stereo out, in-place allowed; `audio-ports-config` adds mono<->mono |
| `clap.params` | all of §3, with `get_info`, `value_to_text`, `text_to_value`, `flush`; events processed with sample-accurate splitting (64-sample grid) |
| `clap.state` | the versioned blob of §8, main thread only |
| `clap.latency` | 0 in minimum-phase mode; `N/2` in linear-phase mode (§4.7) |
| `clap.tail` | decay time of the slowest active section: `min(10 s, 13.8 * Q / (pi * f))`, updated with `clap_host_tail.changed` when parameters change |
| `clap.render` | `offline` -> analyser off, highest linear-phase quality; `realtime` default |
| `clap.remote-controls` | band pages for the MPC Q-Links (§3.2) |
| `clap.preset-discovery` | factory presets (§8.2) |
| `clap.thread-check` | used for debug assertions when offered |
| `clap.gui` | JUCE shell only: embedded (Cocoa/Win32/X11) or floating; the native shell returns "unsupported" and the host uses its generic or Kick UI |
| `com.safian.kickeq/analyzer/1` | vendor extension (§5.3) |

CLAP features: `audio-effect equalizer stereo`. Entry `init` does no I/O.
`process` handles `frames_count == 0`, `in_place`, silence flags (`CLAP_PROCESS_CONTINUE_IF_NOT_QUIET`
when the filters have decayed), and never allocates.

## 8. State and presets

### 8.1 State blob (`state.h`)

Little-endian, versioned, shell-independent:

```
"KEQS" | u32 version (1) | u32 numParams | { u32 id, f32 plainValue } * numParams | u32 numChunks | { u32 tag, u32 size, bytes } * numChunks
```

* Unknown ids and unknown chunks are ignored; missing ids take the default (forward and backward
  compatible); out-of-range values are clamped; NaN/Inf rejected per value.
* Chunks: `"UI01"` editor state (analyser mode/range, window size).
* The JUCE shell stores/restores through the same functions (not APVTS XML).

### 8.2 Presets

Factory presets are `constexpr` parameter lists in `presets.h` (Flat, Kick Punch, Sub Clean, Vocal Presence,
Air, Telephone, ...), exposed by `clap.preset-discovery` and by the editor menu. User presets are state
blobs in files (`.kickeq`) in the platform's per-user data directory (desktop shell only).

## 9. CPU budget

Targets (to be measured and recorded in this file; `tools/bench` runs on any platform including under
QEMU for regression, and is run by the owner on the MPC hardware):

| Platform | Scenario | Budget |
|----------|----------|--------|
| MPC ARMv7, 48 kHz, 128 frames | 8 bands active (2 cuts at 24 dB/oct), stereo, analyser disabled, parameters at rest | <= 3 % of one core |
| same, all parameters moving | | <= 6 % of one core |
| Mid-range phone (arm64) | same | <= 1 % of one core |

Back-of-envelope: 14 sections x 2 channels x 48 k samples/s x ~10 flops = 13 Mflop/s; the ARMv7 VFP at
~1 GHz handles that with a large margin. The measurement replaces this estimate.

## 10. Packaging, manifest, loaders

`manifest/kickeq.manifest.json` (generated from CMake variables, completed by the release job):

```json
{ "id": "com.safian.kickeq", "name": "KickEQ", "version": "1.0.0", "kind": "effect",
  "author": "Safian Szabolcs", "license": "AGPL-3.0-or-later", "homepage": "https://github.com/Safian/KickEQ",
  "min_host_version": "1.0.0", "clap_features": ["audio-effect","equalizer","stereo"],
  "builds": [ { "platform": "linux-armv7", "url": "...", "sha256": "...", "size": 0 } ] }
```

Platform keys: `linux-armv7`, `linux-aarch64`, `linux-x86_64`, `macos-universal`, `windows-x64`
(mobile: static library, no download entry).

* `StaticPluginLoader` (Kick app, iOS/Android/MPC built-in): links `libkickeq_clap_native.a` and calls
  `kickeq_get_plugin_factory()` (exported by the static library; no dlopen).
* `DynamicPluginLoader` (MPC/desktop download): `dlopen`s the `.clap` and uses `clap_entry`.
* Both return the same `clap_plugin_factory_t`; the manifest is the same file. Switching from built-in to
  downloaded is a loader swap.

## 11. Build, test, CI

### 11.1 CMake options

`KICKEQ_BUILD_JUCE` (default ON on macOS/Windows/Linux-desktop), `KICKEQ_BUILD_CLAP_NATIVE` (ON),
`KICKEQ_BUILD_TESTS` (ON), `KICKEQ_LINEAR_PHASE`, `KICKEQ_SANITIZE`. JUCE is fetched only when the JUCE
shell is enabled. Static-only configuration (`-DKICKEQ_BUILD_JUCE=OFF`) needs just a C++17 compiler and
CMake and is what the cross builds use.

### 11.2 Tests

* **Core unit tests** (ctest, run under ASan/UBSan on Linux x86_64):
  * Frequency response of every band type vs the analytic target (gain at `fc`, DC and Nyquist limits,
    -3 dB points of cuts, slope per octave) at 44.1/48/96 kHz, from sine sweeps through the actual engine.
  * **Null test**: with all bands off, or bypass on, the output is bit-identical to the input.
    Flat bands (gain 0) differ by < -140 dBFS.
  * Parameter extremes: all ranges at min/max/off-grid, `fc` at 0.49 fs, Q 0.1 and 18, gain +-24, sample
    rates 8 kHz .. 384 kHz: finite output, bounded.
  * No NaN/Inf/denormal on silence, impulses, DC, full-scale noise, and NaN/Inf *input* (output sanitised).
  * Block-size independence (1, 7, 32, 64, 127, 4096 frames give identical output).
  * Smoothing: no click above -60 dBFS jumps when a parameter steps; M/S flow equals an explicit matrix
    reference; auto-gain on pink noise within 0.5 dB.
  * FFT vs naive DFT, analyser SPSC ring under a producer/consumer stress, state round-trip and fuzzed blobs.
* **Conformance** (`clap_testhost`): loads each `.clap` with dlopen, checks descriptor, params (round trip,
  text conversion), state save/load, latency/tail, processes buffers, compares output across shells
  bit-for-bit against the core reference.
* **Validators**: `clap-validator` on both CLAPs, `pluginval` strictness 5 on the VST3.
* The spike's 5.94 dB vs 6.05 dB at 100 Hz (max low shelf) was investigated: the biquad alone gives
  6.054 dB once settled (6.011 dB after 50 ms), so the difference was the settling time of the smoke
  host's short measurement window, not a design error. The unit tests measure after the transient.

### 11.3 CI matrix

| Job | Runner | What |
|-----|--------|------|
| core-tests | ubuntu-latest (x86_64), ubuntu-24.04-arm (AArch64), macos-latest, windows-latest | build core + native CLAP, ctest, sanitizers on Linux |
| armv7 | ubuntu-latest, `gcc-arm-linux-gnueabihf` + `qemu-user` | cross-compile with `-mfpu=neon-vfpv4 -mfloat-abi=hard`, run core tests and `clap_testhost` under QEMU, bench |
| desktop-plugins | ubuntu / macos / windows | JUCE VST3 + CLAP, pluginval, clap-validator |
| ios | macos-latest | static library build check (`CMAKE_SYSTEM_NAME=iOS`, arm64 + simulator) |
| android | ubuntu-latest | static library build check with the NDK (arm64-v8a, armeabi-v7a) |
| release | tag `v*` | package per platform, SHA-256, manifest, GitHub release |

QEMU timings are only used for regression, never as an absolute figure.

## 12. Implementation status

| Item | Status |
|------|--------|
| 4-band JUCE spike, VST3 + CLAP, pluginval 5, codesigned macOS bundles | **[done]** (`b5096b7`) |
| This document | **[done]** |
| `kickeq_core` (params, design, engine, curve, state) + unit tests | planned, in progress |
| Spectrum analyser (FFT, SPSC, frames) | planned |
| Native CLAP shell + `clap_testhost` conformance | planned |
| JUCE shell on the core + editor (`juce/`), `kickeq_editor_snapshot`, JUCE-profile conformance, cross-shell bit-exactness | **[done]** (macOS arm64; see section 6.1) |
| Presets | planned |
| CI matrix (incl. ARMv7/QEMU, iOS/Android) | planned |
| Linear phase | designed (§4.7), implementation after the CPU benchmark |
| MPC hardware benchmark | needs the device |
