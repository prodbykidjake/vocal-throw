# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## About This Project

**Vox Smoove** (Playbox Audio) is a dynamic resonance suppressor effect plugin. It finds resonant peaks in the incoming signal every analysis frame and pulls them down with a minimum-phase dynamic filter. It looks like an EQ but isn't one: Depth, Detail, Attack, Release and Max cut steer an automatic detector rather than drawing static bands.

The project is derived from the [Pamplejuce](https://github.com/sudara/pamplejuce) template — a JUCE audio plugin template using CMake, C++23, and modern CI/CD. The template provides the build system and project structure. The plugin-specific logic lives in `source/`.

Builds default to **Debug** (faster builds, better debugging). If you're making music with the plugin and hear dropouts or high CPU, build **Release** (`-DCMAKE_BUILD_TYPE=Release`).

CI (`.github/workflows/build_and_test.yml`) currently builds Linux only; macOS/Windows entries, IPP and all code-signing steps are commented out and need re-enabling before a release.

### DSP overview (`source/dsp/`)

Signal flow: `in (or sidechain key) -> [L/R or M/S] -> SpectralAnalyzer -> ReductionCurve (+ DepthCurve weights) -> MinPhaseFilter -> wet trim -> mix -> out gain`. The dry path is delayed by the lookahead (`FrameConfig::latency`, half an analysis window = 1024 samples at 48 kHz, reported to the host) so it lines up with the wet path.

- `FrameConfig` - FFT/hop/kernel sizes derived from the sample rate (window is ~43 ms at any rate)
- `SpectralAnalyzer` - Hann-windowed STFT, power spectrum normalised so a full-scale sine = 1.0
- `ReductionCurve` - bins that stick out of a log-frequency-smoothed copy of the spectrum become target dB reduction. Soft = relative only (level independent); Hard also scales by absolute level. Per-bin attack/release, faster at high frequencies. The six tilt params (Detail/Attack/Release, low and high) reshape the per-bin envelope width and time constants like gentle shelves. Tuning constants are at the top of `ReductionCurve.cpp`
- `DepthCurve` - up to 8 EQ-style bands (low/high cut, low/high shelf, bell, bandpass, band reject, tilt) turned into a per-bin multiplier on detection sensitivity. It steers the detector and never touches the audio. Non-bandpass bands multiply; enabled bandpass bands run in parallel and set the base weight. Weights are only recomputed when a band changes
- `MinPhaseFilter` - turns the reduction curve into a minimum-phase FIR (cepstral method) once per hop, crossfading between kernels
- `ResonanceSuppressor` - owns the above per channel, stereo mode/link, smoothed mix/trim/gain, delta and bypass, the optional sidechain key (detection runs on the key; key listen plays it delayed by the latency), and publishes a reduction snapshot and key level for the editor via atomics

Parameters are defined in `source/Parameters.cpp` (APVTS). Each depth-curve band is 6 params (`bandN_active/enabled/shape/freq/gain/q`, see `ParamIDs::bandParamID`); `active` means the node exists, `enabled` means it isn't bypassed. The processor has an optional mono/stereo sidechain input bus (off by default) and caches raw parameter pointers so the audio thread never looks parameters up by name.

The editor (`source/PluginEditor.*`, `source/ui/`) polls the reduction snapshot on a timer. `DepthCurveOverlay` draws the depth curve over `ReductionGraph` and edits the band params through the host (drag a node = frequency + depth, wheel = Q/slope, double-click empty = add a bell, double-click node = bypass, right-click = shape menu/delete).

Not yet implemented: per-band stereo focus and band listen, linear-phase/quality modes, low-latency mode, presets and A/B, multichannel.

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

# Tests that open the editor need a display; on a headless box use: xvfb-run -a ./Builds/Tests

# Save a PNG of the editor (from the editor test) to look at the UI
VOX_SNAPSHOT_PATH=/tmp/editor.png ./Builds/Tests "[editor]"
```

Preferred setup is VS Code with the CMake Tools extension and Ninja: `cmake -B Builds -G Ninja -DCMAKE_BUILD_TYPE=Debug`. The first time you clone, run `git submodule update --init --recursive`.

On macOS for universal binary: `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`

## Project Structure

- `source/` - Plugin source code (PluginProcessor, PluginEditor, Parameters, `dsp/`, `ui/`)
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
