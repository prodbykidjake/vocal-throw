# SignalPath — product, architecture and status

This is the single source of truth for what SignalPath does today, what is verified, and what is not.
"Verified" means a test I ran and saw pass (see *Testing*). Anything not listed as verified is not.

## 1. What it is

A single-track mixing plugin (EQ → compressor → saturation) with a goal-driven advisor. The user says what they
want to hear (Clarity, Warmth, ...), SignalPath measures the signal, and proposes small, reversible parameter
changes with a plain-language explanation. The audio engine is ordinary DSP and works with the advisor ignored.
No cloud, no ML. All analysis is local and deterministic.

Three modes share one engine and one set of parameters; they differ only in what the UI shows:

| Mode | Shows | Behaviour |
|---|---|---|
| Guided | Goals, measurements, interpretation, "in short" concept tip, manual steps | Preview → Apply / Reject / Undo |
| Automatic | Goals + compact recommendation | Optionally applies suggestions immediately (Settings, off by default) |
| Creative | Full chain + detailed controls open by default | Same recommendations, every control editable |

## 2. Architecture

```
source/
  params/Parameters.h     APVTS layout + ID contract (IDs are saved-session API: never rename)
  dsp/                    Biquad, ParametricEQ, Compressor, Saturator, GainStage, Meters (level/true-peak/K-weighted loudness)
  engine/Chain.h          Input gain -> EQ -> Compressor -> Saturation -> Output gain; click-free per-module enable
  engine/RecommendationSession.h   analyse / preview / apply / reject on top of the advisor (message thread)
  engine/PresetManager.*  factory + user (.spreset XML) presets
  analysis/               CaptureFifo (audio->UI lock-free), SignalAnalyzer (pure function of audio -> Features)
  advisor/                Advisor (pure function: goal, source, intensity, Features, current params -> Recommendation)
  ui/ + PluginEditor.*    Design-system widgets and panels on a 1380x914 canvas scaled to the window
  PluginProcessor.*       Bypass crossfade, loudness-matched audition, A/B slots, undo, state, meters, CPU load
```

Design decisions that matter:

* **Recommendations only write normal parameters** through an undo-enabled path, so they are visible, editable,
  automatable, undoable, and identical across the three modes.
* **The audio thread never allocates, locks, or looks up by name.** Parameter pointers are resolved in the
  constructor; blocks larger than announced are chunked; the analysis tap is a lock-free FIFO.
* **Zero latency.** No look-ahead, no oversampling. Bypass crossfades dry/wet (10 ms) and is bit-exact once settled.
* **The advisor says "no" by default.** It returns "no change" when measurements don't show a need, and never
  overrides a compressor/saturator/EQ band the user is already using.
* **Loudness-matched comparison**: while the chain is active the plugin measures K-weighted loudness in and out;
  with *Match loudness when bypassed* on, the original is played at the processed loudness so A/B is level-fair.
  This is off by default because it makes bypass non-transparent.

## 3. Goals (spec section 9)

Common conditions for every goal: needs >= 1.0 s of signal above -60 dBFS; limits: EQ band <= +/-6 dB,
ratio <= 4:1, targeted gain reduction <= 6 dB, saturation drive <= 8 dB and mix <= 60%, no makeup gain added;
a module already in use is left alone. "Typical" levels come from a per-source table of expected spectral
balance (**provisional heuristics, to be tuned with the listening library**); a deviation only counts as
evidence beyond 3 dB. Evaluation column = what the automated tests check; listening evaluation is still to do.

| Goal | Means | Relevant measurements | Modules | Allowed range | No change when | Explains |
|---|---|---|---|---|---|---|
| Clarity | Less cloudy, more defined | 200-500 Hz vs typical; 2-5 kHz vs typical | EQ bell 2 (cut), bell 3 (lift) | cut <= 4 dB at strongest low-mid band; lift <= 2.5 dB at 3.5 kHz | Both regions typical | Low-mid build-up can read as mud, may be character |
| Warmth | Fuller, rounder | 200-500 Hz lean | EQ bell 2 @ 250 Hz, light saturation | boost <= 3 dB; drive 1.5-4.5 dB @ 35% | Low-mids typical (taste) or already full | Lean low-mids can sound cold, may be intentional |
| Punch | Impact of transients | crest factor, onset steepness, bass (drums/bass) | Compressor (25 ms attack), EQ low shelf | ratio 2-4, GR 2-5 dB; shelf <= 2.5 dB | Crest < 8 dB (already compact) or unclear dynamics | Slow attack lets the hit through |
| Presence | Forward, focused | 2-5 kHz lean, narrow resonance | EQ bell 3 | lift <= 3.5 dB | Typical/strong upper-mids, or a narrow peak present (refers to Reduced harshness) | Where hearing is most sensitive |
| Density | Thicker, steadier | dynamic range (P95-P10), crest | Compressor | ratio 2.5, GR 2-4 dB | Range < 5 dB and crest < 9 dB | Evens loud/quiet gap |
| Smoothness | Less jagged | dynamic range, 5-10 kHz | Soft compressor, high shelf cut | ratio 2, GR 1.5-3 dB; cut <= 3 dB | Neither shows a cause | Level jumps and edgy highs |
| Brightness | Air and sparkle | 5-10 kHz and 10-18 kHz lean | EQ high shelf @ 8 kHz | lift <= 4 dB | Highs typical/strong, or clipping present | Boosts also raise noise |
| Body | Weight, fullness | 60-200 Hz, 200-500 Hz lean | EQ low shelf @ 120 Hz | lift <= 3.5 dB | Bass/low-mids typical or strong | Lean low end can sound thin |
| Dynamic control | More consistent level | dynamic range, crest | Compressor | ratio 2-4, GR 2-5 dB | Range < 6 dB and crest < 12 dB | Turns down loud parts |
| Reduced harshness | Less edge | strongest narrow peak in 2-8 kHz (above both neighbourhoods); upper-mid level | EQ bell 3 at the measured frequency | cut <= 5 dB, Q 2.5 (broad variant Q 0.8, <= 3 dB) | No peak >= 4 dB and upper-mids typical | A peak can be harsh or just a held note |

Evidence levels (not a probability): strong = two independent measurements beyond threshold or one far beyond;
moderate = one beyond threshold; weak = none, and weak never changes anything. The UI does not show a confidence
percentage because none has been validated.

## 4. Status

| Area | Status |
|---|---|
| Audio path: gain, bypass (crossfaded, bit-exact), EQ, compressor, saturator, meters, loudness | **Implemented and unit-tested** |
| Saturator anti-aliasing | **Partial**: first-order ADAA (~6 dB better than plain tanh). Still audible alias at extreme drive on bright sources (measured: -55 dB @ 3 dB drive, -17.8 dB @ 24 dB drive for a 5.1 kHz sine, re fundamental). Oversampling needs latency compensation and is a tracked upgrade |
| Analysis + 10-goal advisor | **Implemented and tested** with synthetic signals and a randomized property test; **not validated on real music** |
| Undo/redo, A/B, presets (5 factory + user), session save/restore | **Implemented and tested** |
| Loudness-matched audition | **Implemented and tested** (K-weighted, approximate 3 s window) |
| UI (all three modes) | **Implemented**; rendered and visually inspected on Linux, interaction tested by hand-written tests only at the logic level |
| Chain reordering | **Not implemented** (architecture is fixed-order; no UI shown for it) |
| Fonts | System fonts. Inter / DM Mono (used in the design) are **not embedded yet** |
| macOS (arm64+x86_64), AU, VST3, Standalone | **Compiles on a GitHub macos-14 runner; unit tests ran there** (first run: 58/59 passed, the one failure was a real bug, fixed: 0 dB parameters read as -5e-7 dB under fast-math). Unsigned/ad-hoc-signed test builds are published as a CI artifact. Not yet loaded in a DAW by anyone |
| Windows, AUv3, CLAP | **Not built or tested** |
| DAW testing | **None.** pluginval only (see below) |
| Listening tests, usability tests, source-profile tuning | **Not done** (need people and audio) |
| Licensing (JUCE), installer, website, support material | **Not done** |

## 5. Testing (what I actually ran)

Linux, GCC, JUCE 9.0.1:

* `Tests`: 57 test cases, ~1.9 million assertions, all passing in Debug and Release. Covers: filter responses at
  44.1/48/96/192 kHz, stability fuzzing of every module and of the whole plugin, compressor static curve and
  attack/release timing, saturator level compensation and measured aliasing, meters and K-weighted loudness
  calibration, analyzer on known signals, advisor rules + safety-bound property test + end-to-end
  (audio -> analysis -> advice -> EQ response), undo/redo, A/B, presets, state round-trip, click-free toggles, UI render.
* `pluginval` 1.x, VST3, strictness 5 and 10 (maximum), headless via Xvfb: **SUCCESS**.
* CPU (Release, stereo 48 kHz, 512-sample blocks, one core): ~78 us/block passthrough (0.7%), ~172 us/block with
  EQ (4 bands + HP) + compressor + saturation (1.6%). Measured on a cloud VM; treat as indicative only.

Reproduce: `cmake -B Builds -G Ninja && cmake --build Builds --target Tests && ./Builds/Tests`.
UI snapshots: `SIGNALPATH_SNAPSHOT_DIR=/some/dir ./Builds/Tests "[ui]"`.

## 6. Trying a build (unsigned macOS test build)

Every push to `signalpath-foundation` runs the macOS workflow. When tests pass it uploads
`SignalPath-<version>-macOS-unsigned` (universal VST3 + AU + Standalone + `INSTALL-macOS.txt`) as an Actions artifact.
It is ad-hoc signed, not notarized: follow the install notes (remove the quarantine flag, copy to the Plug-Ins folders).

## 7. Release preparation (not started)

Developer ID signing, notarization (`notarytool`), installer pkg, licence review (JUCE), website, support docs.
The old template steps for these were removed from the workflow; restore them from git history when the Apple secrets exist.

## 8. Next steps (highest value first)

1. Build and run on macOS (AU + VST3), load in your DAW(s), run pluginval/auval there.
2. Assemble the listening library and tune the per-source "typical balance" table; run goal-by-goal listening tests at matched loudness.
3. Embed Inter and DM Mono; check the UI at 100%/200% scale on real displays.
4. Saturation quality option (2x/4x oversampling with latency reporting and a delayed dry path).
5. Beginner usability test using the Guided mode.
6. Release: JUCE licence, signing/notarization, installer, docs, presets, website.
