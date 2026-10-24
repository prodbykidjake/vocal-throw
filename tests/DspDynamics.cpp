#include "helpers/dsp_test_utils.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <dsp/Compressor.h>
#include <dsp/Saturator.h>
#include <juce_dsp/juce_dsp.h>

using Catch::Approx;

namespace
{
// Runs a constant (DC) level through the compressor and returns the gain reduction (dB) at the end of each block.
std::vector<float> grTrace (Compressor& c, float level, int blocks, int blockSize = 48)
{
    std::vector<float> trace;
    for (int b = 0; b < blocks; ++b)
    {
        auto buf = test::dc (1, blockSize, level);
        c.process (buf, 1, 0, blockSize);
        trace.push_back (-juce::Decibels::gainToDecibels (buf.getSample (0, blockSize - 1) / level));
    }
    return trace;
}
}

TEST_CASE ("Compressor static curve", "[compressor]")
{
    CHECK (Compressor::reductionDb (-30.0f, -18.0f, 4.0f, 0.0f) == Approx (0.0f));
    CHECK (Compressor::reductionDb (-8.0f, -18.0f, 4.0f, 0.0f) == Approx (7.5f)); // 10 dB over, 4:1
    CHECK (Compressor::reductionDb (-8.0f, -18.0f, 1.0f, 0.0f) == Approx (0.0f));
    CHECK (Compressor::reductionDb (0.0f, -18.0f, 1000.0f, 0.0f) == Approx (18.0f).margin (0.05)); // ~limiter

    SECTION ("soft knee is continuous and monotonic")
    {
        float prev = 0.0f;
        for (float level = -40.0f; level <= 0.0f; level += 0.05f)
        {
            const auto r = Compressor::reductionDb (level, -18.0f, 4.0f, 12.0f);
            CHECK (r >= prev - 1.0e-4f);
            CHECK (r - prev < 0.2f); // no jumps
            prev = r;
        }
        CHECK (Compressor::reductionDb (-24.0f, -18.0f, 4.0f, 12.0f) == Approx (0.0f).margin (1.0e-4)); // knee start
        CHECK (Compressor::reductionDb (-18.0f, -18.0f, 4.0f, 12.0f) == Approx (1.125f).margin (0.01)); // 12/(2*12)*... at threshold
    }
}

TEST_CASE ("Compressor reaches the expected steady-state gain reduction", "[compressor]")
{
    Compressor c;
    c.prepare (48000.0);
    c.setParams ({ -18.0f, 4.0f, 5.0f, 100.0f, 0.0f, 0.0f, 1.0f }, true);
    const auto trace = grTrace (c, 0.5f, 400); // -6.02 dBFS: 12.02 dB over threshold -> 9.01 dB at 4:1
    CHECK (trace.back() == Approx (9.01f).margin (0.05));
    CHECK (c.getGainReductionDb() == Approx (9.01f).margin (0.05));
}

TEST_CASE ("Compressor attack and release follow their time constants", "[compressor]")
{
    Compressor c;
    c.prepare (48000.0);
    c.setParams ({ -18.0f, 4.0f, 10.0f, 100.0f, 0.0f, 0.0f, 1.0f }, true);

    const auto attack = grTrace (c, 0.5f, 400, 48); // 1 ms blocks
    const float target = 9.01f;
    CHECK (attack[9] == Approx (target * 0.632f).margin (0.35f)); // ~10 ms
    CHECK (attack[49] == Approx (target * 0.993f).margin (0.2f)); // ~50 ms

    const auto release = grTrace (c, 0.01f, 400, 48); // -40 dBFS: below threshold, GR decays
    CHECK (release[113] == Approx (target * 0.368f).margin (0.4f)); // 14 ms detector hold + one 100 ms time constant
    CHECK (release[399] < 0.2f);
}

TEST_CASE ("Compressor controls behave", "[compressor]")
{
    SECTION ("makeup gain applies with no compression")
    {
        Compressor c;
        c.prepare (48000.0);
        c.setParams ({ 0.0f, 1.0f, 10.0f, 100.0f, 0.0f, 6.0f, 1.0f }, true);
        auto buf = test::dc (1, 256, 0.1f);
        c.process (buf, 1, 0, 256);
        CHECK (buf.getSample (0, 255) == Approx (0.1f * 1.9953f).margin (1.0e-4));
    }
    SECTION ("mix 0 is bit-exact dry")
    {
        Compressor c;
        c.prepare (48000.0);
        c.setParams ({ -40.0f, 10.0f, 1.0f, 50.0f, 0.0f, 12.0f, 0.0f }, true);
        auto buf = test::noise (2, 2048, 5);
        const auto ref = buf;
        c.process (buf, 2, 0, 2048);
        for (int i = 0; i < 2048; ++i)
            REQUIRE (juce::exactlyEqual (buf.getSample (0, i), ref.getSample (0, i)));
    }
    SECTION ("detector is stereo linked: both channels get identical gain")
    {
        Compressor c;
        c.prepare (48000.0);
        c.setParams ({ -30.0f, 4.0f, 1.0f, 100.0f, 0.0f, 0.0f, 1.0f }, true);
        auto buf = test::dc (2, 4800, 0.0f);
        for (int i = 0; i < 4800; ++i)
        {
            buf.setSample (0, i, 0.8f);
            buf.setSample (1, i, 0.1f);
        }
        c.process (buf, 2, 0, 4800);
        CHECK (buf.getSample (0, 4799) / 0.8f == Approx (buf.getSample (1, 4799) / 0.1f).epsilon (1.0e-5));
        CHECK (buf.getSample (0, 4799) < 0.8f);
    }
    SECTION ("parallel mix blends dry and compressed")
    {
        Compressor c;
        c.prepare (48000.0);
        c.setParams ({ -18.0f, 20.0f, 1.0f, 100.0f, 0.0f, 0.0f, 0.5f }, true);
        const auto trace = grTrace (c, 0.5f, 100);
        // wet gain ~ -11.8 dB (0.257), mix 0.5 -> (1 + 0.257) / 2 = 0.628 -> 4.04 dB reduction
        CHECK (trace.back() == Approx (4.04f).margin (0.3f));
    }
}

TEST_CASE ("Compressor is stable at extreme settings", "[compressor][stability]")
{
    juce::Random rng (7);
    for (double fs : { 44100.0, 192000.0 })
    {
        Compressor c;
        c.prepare (fs);
        for (int block = 0; block < 300; ++block)
        {
            c.setParams ({ -60.0f + rng.nextFloat() * 60.0f, 1.0f + rng.nextFloat() * 19.0f, 0.1f + rng.nextFloat() * 199.0f,
                           5.0f + rng.nextFloat() * 1995.0f, rng.nextFloat() * 24.0f, rng.nextFloat() * 24.0f, rng.nextFloat() },
                         rng.nextBool());
            auto buf = test::noise (2, 128, (unsigned) block, 1.0f);
            if (block % 7 == 0)
                buf.clear(); // silence: log of zero must not blow up
            c.process (buf, 2, 0, 128);
            for (int i = 0; i < 128; ++i)
            {
                REQUIRE (std::isfinite (buf.getSample (0, i)));
                REQUIRE (std::abs (buf.getSample (0, i)) < 20.0f);
            }
        }
    }
}

TEST_CASE ("Saturator keeps reference-level loudness constant across drive", "[saturator]")
{
    for (float drive : { 0.0f, 3.0f, 6.0f, 12.0f, 18.0f, 24.0f })
    {
        Saturator s;
        s.prepare (48000.0);
        s.setParams ({ drive, 1.0f }, true);
        auto buf = test::sine (1, 48000, 48000.0, 1000.0, Saturator::referencePeak);
        s.process (buf, 1, 0, 48000);
        const auto outDb = test::rmsDb (buf, 0, 0, 48000);
        const auto inDb = 20.0 * std::log10 (Saturator::referencePeak / std::numbers::sqrt2);
        CAPTURE (drive);
        CHECK (outDb == Approx (inDb).margin (0.05));
    }
}

TEST_CASE ("Saturator basics", "[saturator]")
{
    SECTION ("mix 0 is bit-exact dry")
    {
        Saturator s;
        s.prepare (48000.0);
        s.setParams ({ 24.0f, 0.0f }, true);
        auto buf = test::noise (2, 1024, 9);
        const auto ref = buf;
        s.process (buf, 2, 0, 1024);
        for (int i = 0; i < 1024; ++i)
            REQUIRE (juce::exactlyEqual (buf.getSample (1, i), ref.getSample (1, i)));
    }
    SECTION ("odd symmetry (no DC, no even harmonics)")
    {
        Saturator s1, s2; // separate instances: ADAA keeps per-instance history
        for (auto* s : { &s1, &s2 })
        {
            s->prepare (48000.0);
            s->setParams ({ 12.0f, 1.0f }, true);
        }
        auto a = test::noise (1, 512, 1), b = a;
        b.applyGain (-1.0f);
        s1.process (a, 1, 0, 512);
        s2.process (b, 1, 0, 512);
        for (int i = 0; i < 512; ++i)
            REQUIRE (a.getSample (0, i) == Approx (-b.getSample (0, i)).margin (1.0e-6));
    }
    SECTION ("loud input stays bounded")
    {
        Saturator s;
        s.prepare (48000.0);
        s.setParams ({ 24.0f, 1.0f }, true);
        auto buf = test::dc (1, 256, 10.0f);
        s.process (buf, 1, 0, 256);
        CHECK (buf.getSample (0, 255) < 1.0f);
    }
    SECTION ("drive changes are smoothed")
    {
        Saturator s;
        s.prepare (48000.0);
        s.setParams ({ 0.0f, 1.0f }, true);
        s.setParams ({ 24.0f, 1.0f });
        auto buf = test::sine (1, 2048, 48000.0, 200.0, 0.25f);
        s.process (buf, 1, 0, 2048);
        float maxStep = 0.0f;
        for (int i = 1; i < 2048; ++i)
            maxStep = std::max (maxStep, std::abs (buf.getSample (0, i) - buf.getSample (0, i - 1)));
        CHECK (maxStep < 0.1f);
    }
}

TEST_CASE ("Saturator (ADAA, no oversampling) aliasing is measured and bounded", "[saturator][aliasing]")
{
    // 5.1 kHz sine at 48 kHz. Every FFT bin that is not a true harmonic of f0 below Nyquist is aliasing
    // (harmonics above Nyquist fold back). The saturator uses ADAA instead of oversampling (zero latency,
    // low CPU). Aliasing is still audible at extreme drive on bright material; 2x/4x oversampling would need latency reporting/compensation and is a tracked upgrade. The bounds are regression guards, not quality claims.
    constexpr double fs = 48000.0;
    constexpr int order = 14, n = 1 << order;
    constexpr double f0 = 5109.375; // exactly on an FFT bin so there is no leakage

    struct Case { float drive; double maxAliasDb; };
    for (auto c : { Case { 3.0f, -55.0 }, Case { 6.0f, -45.0 }, Case { 12.0f, -30.0 }, Case { 24.0f, -15.0 } })
    {
        Saturator s;
        s.prepare (fs);
        s.setParams ({ c.drive, 1.0f }, true);
        auto buf = test::sine (1, n, fs, f0, 0.5f);
        s.process (buf, 1, 0, n);

        std::vector<float> data ((size_t) n * 2, 0.0f);
        for (int i = 0; i < n; ++i)
            data[(size_t) i] = buf.getSample (0, i) * (float) (0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * i / n));
        juce::dsp::FFT fft (order);
        fft.performFrequencyOnlyForwardTransform (data.data());

        const auto bin = [&] (double f) { return (int) std::lround (f / fs * n); };
        std::vector<bool> isHarmonic ((size_t) n / 2, false);
        for (int h = 1; h * f0 < fs / 2; ++h)
            for (int d = -3; d <= 3; ++d)
                isHarmonic[(size_t) (bin (h * f0) + d)] = true;

        double aliasPower = 0.0;
        for (int k = 1; k < n / 2; ++k)
            if (! isHarmonic[(size_t) k])
                aliasPower += (double) data[(size_t) k] * data[(size_t) k];
        const auto aliasDb = 10.0 * std::log10 (std::max (aliasPower, 1.0e-18)) - 20.0 * std::log10 (data[(size_t) bin (f0)]);
        WARN ("Saturator drive " << c.drive << " dB, 5.1 kHz sine: total aliasing " << aliasDb << " dB re fundamental");
        CHECK (aliasDb < c.maxAliasDb);
    }
}

TEST_CASE ("Compressor adds no audible distortion on sustained notes at any attack/release", "[compressor][distortion]")
{
    // Regression: the first detector followed the rectified waveform and made the gain wobble at 2x the note frequency
    // (measured -42 dB THD at 60 Hz with 10/100 ms and -19 dB with 0.1/5 ms). With the held peak detector it must be inaudible.
    constexpr double fs = 48000.0;
    constexpr int order = 15, n = 1 << order;
    for (double f0 : { 42.0, 55.0, 77.0, 120.0, 250.0, 1000.0 })
        for (auto [att, rel] : { std::pair { 10.0f, 100.0f }, std::pair { 1.0f, 20.0f }, std::pair { 0.1f, 5.0f }, std::pair { 30.0f, 300.0f } })
        {
            Compressor c;
            c.prepare (fs);
            c.setParams ({ -24.0f, 4.0f, att, rel, 6.0f, 0.0f, 1.0f }, true);
            auto b = test::sine (1, (int) fs * 2, fs, f0, 0.5f);
            for (int s = 0; s < b.getNumSamples(); s += 480)
                c.process (b, 1, s, 480);

            std::vector<float> d ((size_t) n * 2, 0.0f);
            const int off = b.getNumSamples() - n;
            for (int i = 0; i < n; ++i)
                d[(size_t) i] = b.getSample (0, off + i) * (float) (0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * i / n));
            juce::dsp::FFT fft (order);
            fft.performFrequencyOnlyForwardTransform (d.data());
            auto bin = [&] (double f) { return (int) std::lround (f / fs * n); };
            auto power = [&] (double f) { double sum = 0; for (int k = bin (f) - 3; k <= bin (f) + 3; ++k) sum += (double) d[(size_t) k] * d[(size_t) k]; return sum; };
            double harmonics = 0;
            for (int h = 2; h <= 8 && h * f0 < fs / 2; ++h)
                harmonics += power (h * f0);
            const auto thdDb = 10.0 * std::log10 (harmonics / power (f0) + 1.0e-20);
            CAPTURE (f0, att, rel);
            CHECK (thdDb < -80.0);
        }
}
