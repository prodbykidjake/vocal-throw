#include "helpers/dsp_test_utils.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <dsp/ParametricEQ.h>

using Catch::Approx;

TEST_CASE ("EQ band responses match the design targets at all sample rates", "[eq]")
{
    const auto fs = GENERATE (44100.0, 48000.0, 96000.0, 192000.0);
    CAPTURE (fs);

    SECTION ("bell: exact gain at centre, flat far away")
    {
        const auto c = BiquadCoeffs::peak (fs, 1000.0, 6.0, 1.0);
        CHECK (c.magnitudeDb (1000.0, fs) == Approx (6.0).margin (0.01));
        CHECK (std::abs (c.magnitudeDb (50.0, fs)) < 0.3);
        CHECK (std::abs (c.magnitudeDb (15000.0, fs)) < 0.8);
    }
    SECTION ("bell cut")
    {
        CHECK (BiquadCoeffs::peak (fs, 3000.0, -9.0, 2.0).magnitudeDb (3000.0, fs) == Approx (-9.0).margin (0.01));
    }
    SECTION ("low shelf")
    {
        const auto c = BiquadCoeffs::lowShelf (fs, 200.0, 6.0, 0.707);
        CHECK (c.magnitudeDb (20.0, fs) == Approx (6.0).margin (0.4));
        CHECK (c.magnitudeDb (200.0, fs) == Approx (3.0).margin (0.3)); // midpoint of the shelf
        CHECK (std::abs (c.magnitudeDb (8000.0, fs)) < 0.2);
    }
    SECTION ("high shelf")
    {
        const auto c = BiquadCoeffs::highShelf (fs, 6000.0, -6.0, 0.707);
        CHECK (std::abs (c.magnitudeDb (100.0, fs)) < 0.2);
        CHECK (c.magnitudeDb (6000.0, fs) == Approx (-3.0).margin (0.3));
        CHECK (c.magnitudeDb (std::min (20000.0, 0.45 * fs), fs) < -4.0);
    }
    SECTION ("high-pass: -3 dB at cutoff, 12 dB/oct")
    {
        const auto c = BiquadCoeffs::highPass (fs, 100.0, ParametricEQ::hpQ);
        CHECK (c.magnitudeDb (100.0, fs) == Approx (-3.01).margin (0.1));
        CHECK (c.magnitudeDb (50.0, fs) == Approx (-12.3).margin (0.5));
        CHECK (std::abs (c.magnitudeDb (2000.0, fs)) < 0.1);
    }
}

TEST_CASE ("EQ coefficients are stable across the whole parameter space", "[eq]")
{
    for (double fs : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
        for (double f : { 1.0, 20.0, 100.0, 1000.0, 10000.0, 20000.0, 90000.0 }) // includes out-of-range values
            for (double g : { -12.0, -0.5, 0.0, 0.5, 12.0 })
                for (double q : { 0.05, 0.3, 1.0, 8.0, 50.0 })
                    for (const auto& c : { BiquadCoeffs::peak (fs, f, g, q), BiquadCoeffs::lowShelf (fs, f, g, q),
                                           BiquadCoeffs::highShelf (fs, f, g, q), BiquadCoeffs::highPass (fs, f, q) })
                    {
                        REQUIRE (std::isfinite (c.b0 + c.b1 + c.b2 + c.a1 + c.a2));
                        REQUIRE (c.isStable());
                    }
}

TEST_CASE ("EQ audio matches the analytic response", "[eq]")
{
    const auto fs = GENERATE (44100.0, 96000.0);
    EqParams p;
    p.hpEnabled = true;
    p.hpFreq = 60.0f;
    p.bands[1] = { 800.0f, 5.0f, 1.2f };
    p.bands[2] = { 4000.0f, -4.0f, 2.0f };
    p.bands[3] = { 9000.0f, 3.0f, 0.7f };

    for (double f : { 40.0, 150.0, 800.0, 2500.0, 4000.0, 9000.0, 14000.0 })
    {
        ParametricEQ eq;
        eq.prepare (fs);
        eq.setParams (p, true);
        auto buf = test::sine (1, (int) fs, fs, f, 0.1f);
        eq.process (buf, 1, 0, buf.getNumSamples());
        const auto measured = test::rmsDb (buf, 0, (int) fs / 2, (int) fs / 2) - test::rmsDb (test::sine (1, (int) fs, fs, f, 0.1f), 0, (int) fs / 2, (int) fs / 2);
        CHECK (measured == Approx (ParametricEQ::responseDb (p, f, fs)).margin (0.05));
    }
}

TEST_CASE ("EQ with flat settings is bit-exact", "[eq]")
{
    ParametricEQ eq;
    eq.prepare (48000.0);
    auto buf = test::noise (2, 4096, 3);
    const auto ref = buf;
    eq.process (buf, 2, 0, 4096);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 4096; ++i)
            REQUIRE (juce::exactlyEqual (buf.getSample (ch, i), ref.getSample (ch, i)));
}

TEST_CASE ("EQ survives parameter fuzzing without NaN or runaway output", "[eq][stability]")
{
    juce::Random rng (42);
    for (double fs : { 44100.0, 192000.0 })
    {
        ParametricEQ eq;
        eq.prepare (fs);
        for (int block = 0; block < 400; ++block)
        {
            EqParams p;
            p.hpEnabled = rng.nextBool();
            p.hpFreq = 20.0f + rng.nextFloat() * 380.0f;
            for (auto& b : p.bands)
                b = { 20.0f + rng.nextFloat() * 19980.0f, (rng.nextFloat() - 0.5f) * 24.0f, 0.3f + rng.nextFloat() * 7.7f };
            eq.setParams (p);
            auto buf = test::noise (2, 256, (unsigned) block);
            eq.process (buf, 2, 0, 256);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                {
                    REQUIRE (std::isfinite (buf.getSample (ch, i)));
                    REQUIRE (std::abs (buf.getSample (ch, i)) < 60.0f);
                }
        }
    }
}
