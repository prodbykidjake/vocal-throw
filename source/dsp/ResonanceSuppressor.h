#pragma once

#include "MinPhaseFilter.h"
#include "ReductionCurve.h"
#include "SpectralAnalyzer.h"
#include <array>
#include <atomic>

namespace vs
{
struct SuppressorParams
{
    bool hard = false;
    float depth = 40.0f;   // 0..100
    float detail = 50.0f;  // 0..100
    float attack = 50.0f;  // 0..100
    float release = 50.0f; // 0..100
    float maxCutDb = 12.0f;
    float wetTrimDb = 0.0f;
    float mix = 100.0f; // percent
    float outGainDb = 0.0f;
    bool midSide = false;
    float link = 100.0f; // percent
    bool delta = false;
    bool bypass = false;
};

/** The whole effect: (optional M/S) -> analysis -> reduction curve -> minimum-phase dynamic filter ->
    wet trim -> mix -> output gain. The dry path is delayed by the lookahead so it lines up with the wet
    path. Everything is allocated in prepare(); process() is safe for the audio thread. */
class ResonanceSuppressor
{
public:
    static constexpr int maxChannels = 2;
    static constexpr int numDisplayPoints = 128;

    void prepare (double sampleRate, int numChannels);
    void reset();

    int getLatencySamples() const noexcept { return cfg.latency; }
    const FrameConfig& getFrameConfig() const noexcept { return cfg; }

    void setParams (const SuppressorParams& p) noexcept;
    void process (juce::AudioBuffer<float>& buffer) noexcept;

    /** Copies the latest max-over-channels reduction (dB, positive) at numDisplayPoints log-spaced
        frequencies (20 Hz..20 kHz). Safe to call from the message thread. */
    void getReductionDisplay (float* dest) const noexcept;
    static float displayPointFrequency (int index) noexcept;

private:
    void runAnalysisHop() noexcept;

    FrameConfig cfg;
    SuppressorParams params;
    bool prepared = false;
    int activeChannels = 0;
    int hopCounter = 0;

    std::array<SpectralAnalyzer, maxChannels> analyzers;
    std::array<ReductionCurve, maxChannels> curves;
    std::array<MinPhaseFilter, maxChannels> filters;
    std::vector<float> linkedPower;

    std::array<std::vector<float>, maxChannels> delayBuf;
    int delayMask = 0;
    int delayWrite = 0;

    juce::SmoothedValue<float> mixSmoothed, trimSmoothed, outGainSmoothed, bypassSmoothed;

    std::array<int, numDisplayPoints> displayLo {}, displayHi {};
    std::array<std::atomic<float>, numDisplayPoints> display {};
};
} // namespace vs
