# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## About This Project

**Stack Buddy** by Playtime Audio: an audio effect (AU, VST3, Standalone) that goes in an insert slot on an ad-lib track. The vocal passes through untouched and Stack Buddy adds a stack of copies on top: each copy is re-pitched by a few cents (or an octave with High Hype), delayed a little and panned, then everything runs through Tone, Dirt, a tempo-synced ping-pong Echo and Reverb. Harmony mode adds one or two pitch-tracked copies a 3rd/5th away, snapped to a chosen key and scale. Duck dips the echo and reverb returns while the dry vocal is loud; Throw opens the echo/reverb sends wide (with longer feedback, ignoring Duck) so single words can be thrown by automation. Mono in / stereo out is the main layout (mono/mono and stereo/stereo also work). With one voice and the effects at zero it is bit-for-bit transparent (tested). It started as an instrument (MIDI-played sampler) and was converted to an effect at the user's request.

Derived from the [Pamplejuce](https://github.com/sudara/pamplejuce) template (CMake, C++23, CI/CD). The user works in Logic Pro, so macOS AU is the primary target. Builds default to **Debug**; build **Release** for real-time use (dropouts, high CPU). Code signing, Windows and Linux CI are intentionally off for now (see `.github/workflows/build_and_test.yml`).

### Source layout (`source/`)
- `StackEngine` - the real-time stacker. Each copy reads a shared delay line faster or slower than it's written (re-pitching); when a read head runs out of room it crossfades to a new position chosen by a correlation search (WSOLA), so joins don't cancel on tonal vocals. No JUCE processor dependency, so it is unit tested directly (pitch accuracy, clean octave, silence, bounds)
- `Effects.h` - one-pole lowpass (with `track()` bypass), ping-pong echo (send/return with adjustable feedback, so a Throw's tail outlives the send), soft clipper
- `PitchTracker.h` - YIN pitch detector on the input, decimated to ~11 kHz, every 64 decimated samples. Holds the last pitch through gaps
- `Harmony.h` - scale maths: semitones from the sung note to the note N scale steps away in a key (off-key notes snap to the nearest scale note). The harmony copies follow the singer's exact pitch, shifted by whole semitones, so vibrato is kept and nothing is hard-tuned
- `PluginProcessor` - parameters (APVTS), buses, chunked processing through preallocated scratch buffers, effects chain, presets, state. Reverb runs wet-only (JUCE's Reverb doubles the dry signal at dryLevel 1). Dirt is level-compensated. The audio thread publishes input peaks into an atomic ring (`getInputHistory`) for the UI
- `PluginEditor` - the UI from the Figma design (Figma Make file "Stack Buddy Audio Plugin"), adapted for an effect: live INPUT scope instead of a sample (with the stack drawn as ghost layers behind it), no keyboard, header switch is Stack On (bypass with a smoothed crossfade), output meter, animated voice bubbles. The Harmony strip (04), Duck knob and Throw button were added after the Figma design, in its style (Harmony's colour is `Design::cyan`). Everything is laid out at 1180 x 690 design pixels (`Design::width/height`) inside a `Content` component that is scaled to the window
- `DesignTokens` - colours (sampled from the design), Space Grotesk Medium/Bold embedded from `assets/fonts` (OFL licence kept next to them), layout constants
- `DesignLookAndFeel` - knob, switch, pill button and preset box drawing

### Developer tools (hidden Catch2 tests)
- UI preview without a DAW: `STACKBUDDY_SNAPSHOT_DIR=<folder> ./Builds/Tests "[snapshot]"` (run under Xvfb on Linux) writes waiting/listening/small PNGs of the editor
- Offline render: `STACKBUDDY_RENDER_IN=in.wav STACKBUDDY_RENDER_OUT=out.wav STACKBUDDY_PRESET="Wide Stack" ./Builds/Tests "[render]"` (preset name, or "Bypass")
- Benchmark (Release): worst case, 6 voices + octave + 2 harmonies + all effects, is about 0.2 ms per 512-sample block at 48 kHz on an Apple Silicon Mac

### Ideas not built yet
Formant preservation for the octave and harmony copies, a Low Hype octave-down copy, auto key detection.

### Linux build notes
Building on Linux (cloud sessions) needs JUCE's system packages: `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev libxi-dev libxrender-dev libxcomposite-dev libasound2-dev libfreetype-dev libfontconfig1-dev mesa-common-dev libgl1-mesa-dev libcurl4-openssl-dev`. AU only builds on macOS.

## Build Commands

```bash
# Configure (run once, or after CMakeLists.txt changes)
cmake -B Builds -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build Builds --config Debug

# Run tests (from project root)
ctest --test-dir Builds --verbose --output-on-failure

# Or run tests directly
./Builds/Tests

# Run a single test by name
./Builds/Tests "[test name]"

# Run benchmarks
./Builds/Benchmarks
```

For faster builds, add Ninja: `cmake -B Builds -G Ninja -DCMAKE_BUILD_TYPE=Debug`

On macOS for universal binary: `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`

## Project Structure

- `source/` - Plugin source code (PluginProcessor, PluginEditor)
- `tests/` - Catch2 test files
- `benchmarks/` - Catch2 benchmark files
- `cmake/` - CMake modules (Tests.cmake, Benchmarks.cmake, Assets.cmake, etc.)
- `modules/` - Git submodules: clap-juce-extensions, melatonin_inspector
- `JUCE/` - JUCE framework (git submodule)
- `assets/` - Binary resources (auto-included via juce_add_binary_data)
- `packaging/` - Installer resources and scripts

## Architecture

**SharedCode Library**: The `SharedCode` INTERFACE library links plugin source code to both the main plugin target and the Tests target, avoiding ODR violations.

**CMake Modules**:
- `PamplejuceVersion.cmake` - Reads VERSION file, optional auto-bump patch level
- `PamplejuceFunctions.cmake` - One include that defines the `pamplejuce_*` functions below (and turns off their legacy include-time behavior)
- `PamplejuceIPP.cmake` - Intel IPP integration (optional)

Target setup happens via explicit function calls in `CMakeLists.txt`, not at include time:
- `pamplejuce_add_assets()` - Includes all files in assets/ as binary data (from `Assets.cmake`)
- `pamplejuce_add_tests()` - Configures the Catch2 test target (from `Tests.cmake`)
- `pamplejuce_add_benchmarks()` - Configures the Catch2 benchmark target (from `Benchmarks.cmake`)
- `pamplejuce_shared_code_defaults()` - C++23, fast math (from `SharedCodeDefaults.cmake`)
- `pamplejuce_xcode_prettify()` - Xcode folder/scheme cleanup (from `XcodePrettify.cmake`)

Note: after `include(PamplejuceFunctions)`, a plain `include(Tests)` (or Assets, etc.) is a no-op - call the function instead.

**Test Discovery**: Uses `catch_discover_tests()` with `PRE_TEST` discovery mode for Xcode compatibility.

## Key Configuration

Edit `CMakeLists.txt` to customize:
- `PROJECT_NAME` - Internal name (no spaces)
- `PRODUCT_NAME` - Display name in DAWs (can have spaces)
- `COMPANY_NAME` - Used for bundle name
- `BUNDLE_ID` - macOS bundle identifier
- `FORMATS` - Plugin formats to build (Standalone AU VST3 AUv3)
- `PLUGIN_MANUFACTURER_CODE` / `PLUGIN_CODE` - 4-character plugin IDs

Version is read from the `VERSION` file in project root.

## Code Quality

Always resolve any compile warnings encountered during builds. Warnings should be treated as errors and fixed before considering a task complete.

Note: LSP/clangd often reports false positive diagnostic errors (like "undeclared identifier", "file not found") because it doesn't have full context of the JUCE module system. Ignore these unless the actual build fails.

## Includes

JUCE modules include common standard library headers (`<vector>`, `<algorithm>`, `<string>`, `<memory>`, etc.) so you don't need to add those explicitly in JUCE code. Adding them is harmless but redundant.

## Threading Model

JUCE plugins have two main threads:

- **Audio thread**: Runs `processBlock` — must be realtime-safe (see below). Never block, allocate, or lock.
- **Message thread**: Runs UI callbacks, parameter listeners, and timer callbacks. Owns the `MessageManager`.

To communicate between them:
- **Simple values**: Use `std::atomic` or JUCE's `AudioParameterFloat`/`AudioParameterBool` (which are atomic under the hood)
- **Larger data**: Use a lock-free queue (e.g. `moodycamel::ReaderWriterQueue`) to pass data from message → audio thread
- **Audio → UI updates**: Use `juce::AsyncUpdater` or `juce::Timer` on the message thread to poll state — never call UI code from the audio thread

## Realtime Safety

For anything in the audio thread / hot DSP path (e.g. `processBlock`):
- Allocate in constructors or `prepareToPlay`, not while rendering audio
- Avoid dynamic allocations and container growth (`std::vector::push_back`, map insertion, string building)
- Prefer fixed-size storage (`std::array`, preallocated buffers, fixed-capacity queues)
- Keep operations deterministic and lock-free where possible

## Adding Dependencies

**JUCE Modules** live in `modules/` as git submodules. Add with `git submodule add`, then `add_subdirectory` and link to `SharedCode` in `CMakeLists.txt`. Some useful ones:

- [melatonin_inspector](https://github.com/sudara/melatonin_inspector) — runtime component debugger (already included)
- [melatonin_blur](https://github.com/sudara/melatonin_blur) — fast cross-platform blurs for C++ UI (shadows, glows, frosted glass)
- [melatonin_perfetto](https://github.com/sudara/melatonin_perfetto) — performance tracing with Perfetto, great for profiling `processBlock` and paint calls
- [gin](https://github.com/FigBug/gin) — large collection of utilities (DSP, UI components, LookAndFeel, etc.)

**Non-JUCE C++ libraries** should be added via [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) which is already configured. CPM downloads and caches dependencies at configure time — no submodule needed:

```cmake
CPMAddPackage("gh:nlohmann/json@3.11.3")
target_link_libraries(SharedCode INTERFACE nlohmann_json::nlohmann_json)
```

Some useful CPM libraries:
- [nlohmann/json](https://github.com/nlohmann/json) — JSON parsing/serialization
- [cameron314/readerwriterqueue](https://github.com/cameron314/readerwriterqueue) — lock-free single-producer/single-consumer queue, ideal for audio↔message thread communication

## Code Style

Uses `.clang-format` with Allman-style braces, 4-space indentation, no column limit.
