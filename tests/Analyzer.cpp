#include "helpers/dsp_test_utils.h"
#include <analysis/SignalAnalyzer.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

namespace
{
constexpr double fs = 48000.0;

Features analyse (const juce::AudioBuffer<float>& b)
{
    return SignalAnalyzer::analyse (b.getReadPointer (0), b.getReadPointer (b.getNumChannels() > 1 ? 1 : 0), b.getNumSamples(), fs);
}

int bandNear (float hz)
{
    int best = 0;
    for (int i = 0; i < Features::numBands; ++i)
        if (std::abs (std::log2 (Features::bandCentre (i) / hz)) < std::abs (std::log2 (Features::bandCentre (best) / hz)))
            best = i;
    return best;
}
}

TEST_CASE ("Analyzer rejects silence and too-short audio", "[analysis]")
{
    CHECK_FALSE (analyse (test::dc (2, 48000 * 3, 0.0f)).valid);
    CHECK_FALSE (analyse (test::noise (2, 4000, 1)).valid);
    CHECK_FALSE (analyse (test::dc (2, 48000 * 3, 1.0e-5f)).valid); // below the -60 dB gate
}

TEST_CASE ("Analyzer level measurements are correct on known signals", "[analysis]")
{
    const auto f = analyse (test::sine (2, 48000 * 3, fs, 1000.0, 0.5f));
    REQUIRE (f.valid);
    CHECK (f.peakDb == Approx (-6.02f).margin (0.05f));
    CHECK (f.rmsDb == Approx (-9.03f).margin (0.05f));
    CHECK (f.crestDb == Approx (3.01f).margin (0.1f));
    CHECK (f.stereoCorrelation > 0.999f);
    CHECK (f.widthIndex < 0.01f);
    CHECK (f.dynamicRangeDb < 0.5f);
    CHECK (f.clippingEvents == 0);
}

TEST_CASE ("Analyzer detects clipping runs, not single peaks", "[analysis]")
{
    auto b = test::sine (2, 48000 * 2, fs, 200.0, 0.3f);
    b.setSample (0, 1000, 1.0f); // a lone full-scale sample
    CHECK (analyse (b).clippingEvents == 0);
    for (int i = 5000; i < 5010; ++i)
        b.setSample (0, i, 1.0f);
    CHECK (analyse (b).clippingEvents == 1);
}

TEST_CASE ("Analyzer stereo measurements", "[analysis]")
{
    auto wide = test::noise (2, 48000 * 3, 5);
    const auto f = analyse (wide);
    CHECK (std::abs (f.stereoCorrelation) < 0.1f);
    CHECK (f.widthIndex > 0.8f);

    auto mono = test::noise (2, 48000 * 3, 5);
    mono.copyFrom (1, 0, mono, 0, 0, mono.getNumSamples());
    CHECK (analyse (mono).stereoCorrelation > 0.999f);
}

TEST_CASE ("Analyzer spectrum maps frequencies to the right bands", "[analysis]")
{
    SECTION ("white noise rises 3 dB per octave in constant-relative-bandwidth bands")
    {
        const auto f = analyse (test::noise (1, 48000 * 6, 11));
        REQUIRE (f.valid);
        CHECK (f.bandDb[(size_t) bandNear (8000.0f)] - f.bandDb[(size_t) bandNear (1000.0f)] == Approx (9.0f).margin (1.5f));
        CHECK (f.bandDb[(size_t) bandNear (1000.0f)] - f.bandDb[(size_t) bandNear (125.0f)] == Approx (9.0f).margin (1.5f));
    }
    SECTION ("a sine lands in its own band")
    {
        for (float hz : { 100.0f, 440.0f, 3150.0f, 9000.0f })
        {
            const auto f = analyse (test::sine (1, 48000 * 3, fs, hz, 0.3f));
            const auto peakBand = (int) (std::max_element (f.bandDb.begin(), f.bandDb.end()) - f.bandDb.begin());
            CAPTURE (hz);
            CHECK (std::abs (peakBand - bandNear (hz)) <= 1);
        }
    }
    SECTION ("group levels distinguish a bass-heavy from a bright signal")
    {
        auto dark = test::sine (1, 48000 * 3, fs, 80.0, 0.4f);
        auto bright = test::sine (1, 48000 * 3, fs, 6000.0, 0.4f);
        const auto fd = analyse (dark), fb = analyse (bright);
        CHECK (fd.groupDb (60, 200) > fd.groupDb (5000, 10000) + 30.0f);
        CHECK (fb.groupDb (5000, 10000) > fb.groupDb (60, 200) + 30.0f);
    }
}

TEST_CASE ("Analyzer reads dynamics and onsets", "[analysis]")
{
    // steady tone: little dynamic range, no onsets
    CHECK (analyse (test::sine (1, 48000 * 4, fs, 500.0, 0.3f)).onsetStrengthDb < 0.5f);

    // 4 Hz amplitude pulses (25 ms at full level, 20 dB lower in between): strong onsets and a large dynamic range
    auto pulsed = test::sine (1, 48000 * 4, fs, 500.0, 0.5f);
    for (int i = 0; i < pulsed.getNumSamples(); ++i)
    {
        const double phase = std::fmod (i * 4.0 / fs, 1.0);
        pulsed.setSample (0, i, pulsed.getSample (0, i) * (float) (phase < 0.1 ? 1.0 : 0.1));
    }
    const auto f = analyse (pulsed);
    REQUIRE (f.valid);
    CHECK (f.onsetStrengthDb > 6.0f);
    CHECK (f.dynamicRangeDb > 8.0f);
    CHECK (f.crestDb > 8.0f);
}

TEST_CASE ("Capture ring keeps the last audible audio and skips silence", "[analysis]")
{
    CaptureRing ring;
    auto tone = test::sine (2, 480, fs, 1000.0, 0.5f);
    for (int i = 0; i < 400; ++i) // 4 s of signal
        ring.push (tone, 2);
    juce::AudioBuffer<float> silence (2, 480);
    silence.clear();
    for (int i = 0; i < 4000; ++i) // 40 s of silence afterwards: must not push the signal out of the ring
        ring.push (silence, 2);

    const auto f = SignalAnalyzer::analyseRecent (ring, fs, 3.0f);
    REQUIRE (f.valid);
    CHECK (f.peakDb == Approx (-6.02f).margin (0.1f));
    CHECK (f.analysedSeconds == Approx (3.0f).margin (0.01f));

    CaptureRing empty;
    CHECK_FALSE (SignalAnalyzer::analyseRecent (empty, fs, 3.0f).valid);
}

TEST_CASE ("Capture ring survives being lapped many times", "[analysis]")
{
    CaptureRing ring;
    auto tone = test::sine (2, 480, fs, 440.0, 0.4f);
    for (int i = 0; i < 5000; ++i) // ~50 s, ring holds ~11 s
        ring.push (tone, 2);
    std::vector<float> l (390000), r (390000);
    const int n = ring.copyRecent (l.data(), r.data(), 390000);
    CHECK (n == 390000);
    float peak = 0;
    for (int i = 0; i < n; ++i)
        peak = std::max (peak, std::abs (l[(size_t) i]));
    CHECK (peak == Approx (0.4f).margin (0.01f));
}
