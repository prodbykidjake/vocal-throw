#include "helpers/dsp_test_utils.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <dsp/Meters.h>

using Catch::Approx;

namespace
{
void feed (LevelMeter& m, juce::AudioBuffer<float>& b, int channels)
{
    for (int start = 0; start + 512 <= b.getNumSamples(); start += 512)
    {
        juce::AudioBuffer<float> view (b.getArrayOfWritePointers(), channels, start, 512);
        m.process (view, channels);
    }
}
}

TEST_CASE ("LevelMeter reports sample peak, true peak and RMS", "[meters]")
{
    LevelMeter m;
    m.prepare (48000.0);

    SECTION ("plain sine")
    {
        auto b = test::sine (2, 48000 * 4, 48000.0, 1000.0, 0.5f);
        feed (m, b, 2);
        CHECK (m.takeSamplePeak (0) == Approx (0.5f).margin (0.001));
        CHECK (m.takeTruePeak (0) >= m.takeSamplePeak (0)); // second take is 0 (reset) -> still true
        CHECK (m.getRms (0) == Approx (0.5f / std::numbers::sqrt2_v<float>).margin (0.005));
    }
    SECTION ("true peak exceeds sample peak for inter-sample peaks")
    {
        // fs/4 sine at 45 degrees: every sample is +-0.707 but the waveform peaks at 1.0
        auto b = test::sine (1, 48000, 48000.0, 12000.0, 1.0f, std::numbers::pi / 4.0);
        feed (m, b, 1);
        CHECK (m.takeSamplePeak (0) == Approx (0.7071f).margin (0.002));
        const auto tp = m.takeTruePeak (0);
        CHECK (tp > 0.95f);
        CHECK (tp < 1.05f);
    }
    SECTION ("take() clears the accumulator")
    {
        auto b = test::sine (1, 1024, 48000.0, 1000.0, 0.8f);
        feed (m, b, 1);
        CHECK (m.takeSamplePeak (0) > 0.7f);
        CHECK (juce::exactlyEqual (m.takeSamplePeak (0), 0.0f));
    }
}

TEST_CASE ("LevelMeter stereo correlation and width", "[meters]")
{
    LevelMeter m;
    m.prepare (48000.0);

    auto mono = test::noise (2, 96000, 1);
    mono.copyFrom (1, 0, mono, 0, 0, 96000);
    feed (m, mono, 2);
    CHECK (m.getCorrelation() > 0.99f);
    CHECK (m.getWidthIndex() < 0.02f);

    m.reset();
    auto inverted = mono;
    inverted.applyGain (1, 0, 96000, -1.0f);
    feed (m, inverted, 2);
    CHECK (m.getCorrelation() < -0.99f);

    m.reset();
    auto wide = test::noise (2, 96000, 2);
    feed (m, wide, 2);
    CHECK (std::abs (m.getCorrelation()) < 0.1f);
    CHECK (m.getWidthIndex() > 0.8f);
}

TEST_CASE ("LoudnessMeter K-weighting is calibrated", "[meters][loudness]")
{
    const auto fs = GENERATE (44100.0, 48000.0);
    LoudnessMeter l;
    l.prepare (fs);
    // BS.1770: a full-scale 1 kHz sine on one channel reads about -3.01 LUFS; on two channels about 0 LUFS.
    auto mono = test::sine (1, (int) fs * 10, fs, 1000.0, 1.0f);
    for (int s = 0; s + 480 <= mono.getNumSamples(); s += 480)
        l.process (juce::AudioBuffer<float> (mono.getArrayOfWritePointers(), 1, s, 480), 1);
    CHECK (l.getLufs() == Approx (-3.01f).margin (0.15f));

    LoudnessMeter l2;
    l2.prepare (fs);
    auto stereo = test::sine (2, (int) fs * 10, fs, 1000.0, 0.1f);
    for (int s = 0; s + 480 <= stereo.getNumSamples(); s += 480)
        l2.process (juce::AudioBuffer<float> (stereo.getArrayOfWritePointers(), 2, s, 480), 2);
    CHECK (l2.getLufs() == Approx (-20.0f).margin (0.15f)); // 0 LUFS - 20 dB

    LoudnessMeter quiet;
    quiet.prepare (fs);
    CHECK (quiet.getLufs() <= -100.0f); // silence reads as very low, not NaN
}

TEST_CASE ("K-weighting discounts low frequencies", "[meters][loudness]")
{
    LoudnessMeter lo, mid;
    lo.prepare (48000.0);
    mid.prepare (48000.0);
    auto a = test::sine (1, 480000, 48000.0, 50.0, 0.5f), b = test::sine (1, 480000, 48000.0, 1000.0, 0.5f);
    for (int s = 0; s + 480 <= a.getNumSamples(); s += 480)
    {
        lo.process (juce::AudioBuffer<float> (a.getArrayOfWritePointers(), 1, s, 480), 1);
        mid.process (juce::AudioBuffer<float> (b.getArrayOfWritePointers(), 1, s, 480), 1);
    }
    CHECK (lo.getLufs() < mid.getLufs() - 2.0f);
}
