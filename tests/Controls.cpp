#include "helpers/signal_helpers.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <dsp/DepthCurve.h>
#include <dsp/ReductionCurve.h>
#include <dsp/ResonanceSuppressor.h>

using namespace testing_helpers;
using ParamIDs::BandParam;
using vs::BandShape;

namespace
{
constexpr size_t kTotal = 96000;
constexpr double kToneHz = 3000.0;
constexpr int kLatency = 1024;

struct Stereo
{
    std::vector<float> left, right;
};

/** L = R = input through the plugin, both channels returned. */
Stereo runBoth (PluginProcessor& plugin, const std::vector<float>& input)
{
    plugin.setPlayConfigDetails (2, 2, kSampleRate, kBlockSize);
    plugin.prepareToPlay (kSampleRate, kBlockSize);

    Stereo out { std::vector<float> (input.size()), std::vector<float> (input.size()) };
    juce::AudioBuffer<float> buffer (2, kBlockSize);
    juce::MidiBuffer midi;
    for (size_t pos = 0; pos + (size_t) kBlockSize <= input.size(); pos += (size_t) kBlockSize)
    {
        for (int c = 0; c < 2; ++c)
            std::copy (input.begin() + (long) pos, input.begin() + (long) pos + kBlockSize, buffer.getWritePointer (c));
        plugin.processBlock (buffer, midi);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + kBlockSize, out.left.begin() + (long) pos);
        std::copy (buffer.getReadPointer (1), buffer.getReadPointer (1) + kBlockSize, out.right.begin() + (long) pos);
    }
    return out;
}

std::vector<float> noiseWithTone()
{
    auto x = whiteNoise ((int) kTotal, 0.05f);
    addSine (x, kToneHz, 0.01f);
    return x;
}

double reductionDb (const std::vector<float>& in, const std::vector<float>& out)
{
    std::vector<float> aligned (in.size(), 0.0f);
    std::copy (in.begin(), in.end() - kLatency, aligned.begin() + kLatency);
    return toDb (tonePower (aligned, 48000, 48000, kToneHz) / tonePower (out, 48000, 48000, kToneHz));
}

void useStrongSettings (PluginProcessor& plugin, float depth = 100.0f)
{
    setParam (plugin, ParamIDs::depth, depth);
    setParam (plugin, ParamIDs::detail, 70.0f);
    setParam (plugin, ParamIDs::maxCut, 40.0f);
}

vs::BandSettings band (BandShape shape, float hz, float gainDb, float q, float balance)
{
    vs::BandSettings b;
    b.enabled = true;
    b.shape = shape;
    b.freqHz = hz;
    b.gainDb = gainDb;
    b.q = q;
    b.balance = balance;
    return b;
}
} // namespace

TEST_CASE ("New control parameters exist with their defaults", "[controls][params]")
{
    PluginProcessor plugin;
    CHECK (getParam (plugin, ParamIDs::selectivity) == Catch::Approx (46.0f).margin (0.1f));
    CHECK (getParam (plugin, ParamIDs::stereoBalance) == Catch::Approx (0.0f).margin (0.1f));
    CHECK (juce::roundToInt (getParam (plugin, ParamIDs::quality)) == 0);
    CHECK (getParam (plugin, ParamIDs::qualityLock) > 0.5f);

    SECTION ("they survive a state round trip")
    {
        setParam (plugin, ParamIDs::selectivity, 80.0f);
        setParam (plugin, ParamIDs::stereoBalance, -20.0f);
        setParam (plugin, ParamIDs::quality, 2.0f);
        setParam (plugin, ParamIDs::qualityLock, 0.0f);
        setParam (plugin, ParamIDs::bandParamID (2, BandParam::balance), 30.0f);

        juce::MemoryBlock state;
        plugin.getStateInformation (state);
        PluginProcessor other;
        other.setStateInformation (state.getData(), (int) state.getSize());

        CHECK (getParam (other, ParamIDs::selectivity) == Catch::Approx (80.0f).margin (0.2f));
        CHECK (getParam (other, ParamIDs::stereoBalance) == Catch::Approx (-20.0f).margin (0.6f));
        CHECK (juce::roundToInt (getParam (other, ParamIDs::quality)) == 2);
        CHECK (getParam (other, ParamIDs::qualityLock) < 0.5f);
        CHECK (getParam (other, ParamIDs::bandParamID (2, BandParam::balance)) == Catch::Approx (30.0f).margin (0.6f));
    }

    SECTION ("band gain is limited to +-18 dB")
    {
        setParam (plugin, ParamIDs::bandParamID (2, BandParam::gain), 40.0f);
        CHECK (getParam (plugin, ParamIDs::bandParamID (2, BandParam::gain)) == Catch::Approx (18.0f).margin (0.01f));
    }
}

TEST_CASE ("Selectivity decides how tall a peak has to be", "[controls][selectivity]")
{
    const auto cfg = vs::FrameConfig::forSampleRate (kSampleRate);
    std::vector<float> power ((size_t) cfg.numBins, 1.0e-5f);
    power[128] *= 4.0f; // a 6 dB bump at 3 kHz

    auto settled = [&] (float selectivity01) {
        vs::ReductionCurve curve;
        curve.prepare (cfg);
        vs::ReductionParams p;
        p.depth01 = 1.0f;
        p.maxCutDb = 40.0f;
        p.selectivity01 = selectivity01;
        curve.setParams (p);
        for (int i = 0; i < 200; ++i)
            curve.process (power.data());
        return curve.getReductionDb()[128];
    };

    const auto loose = settled (0.05f);
    const auto normal = settled (0.46f);
    const auto strict = settled (0.95f);
    INFO ("loose " << loose << ", normal " << normal << ", strict " << strict << " dB");
    CHECK (loose > normal);
    CHECK (normal > 0.5f);
    CHECK (juce::exactlyEqual (strict, 0.0f)); // too small a bump to count

    SECTION ("the default matches the old fixed 2 dB knee")
    {
        CHECK (settled (0.46f) == Catch::Approx (settled (std::sqrt (1.5f / 7.5f))).margin (0.3));
    }
}

TEST_CASE ("Stereo balance moves the reduction between the channels", "[controls][balance]")
{
    const auto in = noiseWithTone();

    auto reductions = [&] (float balance) {
        PluginProcessor plugin;
        useStrongSettings (plugin);
        setParam (plugin, ParamIDs::stereoBalance, balance);
        const auto out = runBoth (plugin, in);
        return std::pair { reductionDb (in, out.left), reductionDb (in, out.right) };
    };

    const auto [evenL, evenR] = reductions (0.0f);
    const auto [rightL, rightR] = reductions (50.0f);
    const auto [leftL, leftR] = reductions (-50.0f);
    INFO ("even " << evenL << "/" << evenR << "  +50 " << rightL << "/" << rightR << "  -50 " << leftL << "/" << leftR);

    CHECK (evenL > 6.0);
    CHECK (std::abs (evenL - evenR) < 1.0);
    CHECK (std::abs (rightL) < 1.0); // +50 leaves L/Mid alone ...
    CHECK (rightR > 6.0);            // ... and works on R/Side
    CHECK (std::abs (leftR) < 1.0);
    CHECK (leftL > 6.0);
}

TEST_CASE ("Per-band balance", "[controls][balance]")
{
    vs::BandArray bands {};

    SECTION ("a bell with balance +1 only acts on the right channel")
    {
        bands[0] = band (BandShape::bell, 3000.0f, 12.0f, 1.0f, 1.0f);
        CHECK (vs::DepthCurve::weightAt (bands, 3000.0f, 0) == Catch::Approx (1.0f).margin (0.001));
        CHECK (vs::DepthCurve::weightAt (bands, 3000.0f, 1) == Catch::Approx (3.98f).margin (0.02));
    }

    SECTION ("and balance -1 only on the left")
    {
        bands[0] = band (BandShape::bell, 3000.0f, 12.0f, 1.0f, -1.0f);
        CHECK (vs::DepthCurve::weightAt (bands, 3000.0f, 0) == Catch::Approx (3.98f).margin (0.02));
        CHECK (vs::DepthCurve::weightAt (bands, 3000.0f, 1) == Catch::Approx (1.0f).margin (0.001));
    }

    SECTION ("a half balance halves the dB on the de-emphasised channel")
    {
        bands[0] = band (BandShape::bell, 3000.0f, 12.0f, 1.0f, 0.5f);
        CHECK (vs::DepthCurve::weightAt (bands, 3000.0f, 0) == Catch::Approx (1.995f).margin (0.02)); // 6 dB
        CHECK (vs::DepthCurve::weightAt (bands, 3000.0f, 1) == Catch::Approx (3.98f).margin (0.02));
    }

    SECTION ("zero balance is identical on both channels")
    {
        bands[0] = band (BandShape::bell, 3000.0f, 9.0f, 1.0f, 0.0f);
        for (const auto hz : { 200.0f, 3000.0f, 9000.0f })
            CHECK (vs::DepthCurve::weightAt (bands, hz, 0) == Catch::Approx (vs::DepthCurve::weightAt (bands, hz, 1)));
    }

    SECTION ("a bandpass zone is processed less on the de-emphasised channel, not widened")
    {
        bands[0] = band (BandShape::bandpass, 3000.0f, 0.0f, 2.0f, 1.0f);
        CHECK (vs::DepthCurve::weightAt (bands, 3000.0f, 0) == Catch::Approx (0.0f).margin (0.001));
        CHECK (vs::DepthCurve::weightAt (bands, 3000.0f, 1) == Catch::Approx (1.0f).margin (0.01));
        CHECK (vs::DepthCurve::weightAt (bands, 500.0f, 0) == Catch::Approx (0.0f).margin (0.001));
    }

    SECTION ("a cut with balance leaves the other channel neutral")
    {
        bands[0] = band (BandShape::lowCut, 1000.0f, 0.0f, 4.0f, -1.0f);
        CHECK (vs::DepthCurve::weightAt (bands, 100.0f, 0) < 0.05f);
        CHECK (vs::DepthCurve::weightAt (bands, 100.0f, 1) == Catch::Approx (1.0f).margin (0.001));
    }

    SECTION ("DepthCurve exposes the per-channel arrays")
    {
        vs::DepthCurve curve;
        const auto cfg = vs::FrameConfig::forSampleRate (kSampleRate);
        curve.prepare (cfg);
        bands[0] = band (BandShape::bell, 3000.0f, 12.0f, 1.0f, 1.0f);
        curve.setBands (bands);
        CHECK (curve.getWeights (0)[128] == Catch::Approx (1.0f).margin (0.001));
        CHECK (curve.getWeights (1)[128] == Catch::Approx (3.98f).margin (0.05));
    }

    SECTION ("in the plugin, a boosted band with balance only strengthens one channel")
    {
        const auto in = noiseWithTone();
        PluginProcessor plugin;
        useStrongSettings (plugin, 25.0f);
        setParam (plugin, ParamIDs::bandParamID (2, BandParam::gain), 12.0f);
        setParam (plugin, ParamIDs::bandParamID (2, BandParam::balance), 50.0f);
        const auto out = runBoth (plugin, in);
        const auto l = reductionDb (in, out.left);
        const auto r = reductionDb (in, out.right);
        INFO ("left " << l << " dB, right " << r << " dB");
        CHECK (r > l + 3.0);
    }
}

TEST_CASE ("Quality changes the refresh rate, not the latency", "[controls][quality]")
{
    SECTION ("hop size per quality")
    {
        vs::ResonanceSuppressor suppressor;
        suppressor.prepare (kSampleRate, 2);
        const auto latency = suppressor.getLatencySamples();

        vs::SuppressorParams p;
        p.quality = 0;
        suppressor.setParams (p);
        CHECK (suppressor.getFrameConfig().hopSize == 512);
        p.quality = 1;
        suppressor.setParams (p);
        CHECK (suppressor.getFrameConfig().hopSize == 256);
        p.quality = 2;
        suppressor.setParams (p);
        CHECK (suppressor.getFrameConfig().hopSize == 128);
        CHECK (suppressor.getLatencySamples() == latency);
    }

    SECTION ("the plugin reports the same latency and still finds the resonance at every quality")
    {
        const auto in = noiseWithTone();
        for (int quality = 0; quality < 3; ++quality)
        {
            PluginProcessor plugin;
            useStrongSettings (plugin);
            setParam (plugin, ParamIDs::quality, (float) quality);
            const auto out = runBoth (plugin, in);
            INFO ("quality " << quality);
            CHECK (plugin.getLatencySamples() == kLatency);
            CHECK (allFinite (out.left));
            CHECK (reductionDb (in, out.left) > 6.0);
        }
    }

    SECTION ("quality can change while audio runs without breaking anything")
    {
        PluginProcessor plugin;
        useStrongSettings (plugin);
        plugin.setPlayConfigDetails (2, 2, kSampleRate, kBlockSize);
        plugin.prepareToPlay (kSampleRate, kBlockSize);

        const auto in = noiseWithTone();
        juce::AudioBuffer<float> buffer (2, kBlockSize);
        juce::MidiBuffer midi;
        for (size_t pos = 0, block = 0; pos + (size_t) kBlockSize <= in.size(); pos += (size_t) kBlockSize, ++block)
        {
            setParam (plugin, ParamIDs::quality, (float) (block % 3));
            for (int c = 0; c < 2; ++c)
                std::copy (in.begin() + (long) pos, in.begin() + (long) pos + kBlockSize, buffer.getWritePointer (c));
            plugin.processBlock (buffer, midi);
            for (int i = 0; i < kBlockSize; ++i)
                REQUIRE (std::isfinite (buffer.getSample (0, i)));
        }
    }
}
