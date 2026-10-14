#include "helpers/signal_helpers.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <dsp/DepthCurve.h>
#include <dsp/ReductionCurve.h>

using namespace testing_helpers;
using ParamIDs::BandParam;
using vs::BandShape;

namespace
{
vs::BandSettings band (BandShape shape, float hz, float gainDb = 0.0f, float q = 1.0f)
{
    vs::BandSettings b;
    b.enabled = true;
    b.shape = shape;
    b.freqHz = hz;
    b.gainDb = gainDb;
    b.q = q;
    return b;
}

float weight (const vs::BandArray& bands, float hz) { return vs::DepthCurve::weightAt (bands, hz); }

// the processor's default bands are the first three; the integration tests start from those
void setBand (PluginProcessor& p, int n, BandParam which, float value) { setParam (p, ParamIDs::bandParamID (n, which), value); }

constexpr size_t kTotal = 96000;
constexpr double kToneHz = 3000.0;

std::vector<float> noiseWithTone()
{
    auto x = whiteNoise ((int) kTotal, 0.05f);
    addSine (x, kToneHz, 0.01f);
    return x;
}

double toneReductionDb (PluginProcessor& plugin)
{
    const auto in = noiseWithTone();
    const auto out = run (plugin, in);
    std::vector<float> aligned (in.size(), 0.0f);
    std::copy (in.begin(), in.end() - 1024, aligned.begin() + 1024);
    return toDb (tonePower (aligned, 48000, 48000, kToneHz) / tonePower (out, 48000, 48000, kToneHz));
}

void useStrongSettings (PluginProcessor& plugin, float depth)
{
    setParam (plugin, ParamIDs::depth, depth);
    setParam (plugin, ParamIDs::detail, 70.0f);
    setParam (plugin, ParamIDs::maxCut, 40.0f);
}
} // namespace

TEST_CASE ("Depth curve shapes", "[depthcurve]")
{
    vs::BandArray bands {};

    SECTION ("no enabled bands is exactly neutral")
    {
        for (const auto hz : { 30.0f, 300.0f, 3000.0f, 15000.0f })
            CHECK (juce::exactlyEqual (weight (bands, hz), 1.0f));

        bands[0] = band (BandShape::bell, 3000.0f, 12.0f);
        bands[0].enabled = false;
        CHECK (juce::exactlyEqual (weight (bands, 3000.0f), 1.0f));
    }

    SECTION ("a bell boosts around its centre only")
    {
        bands[0] = band (BandShape::bell, 3000.0f, 12.0f, 1.0f);
        CHECK (weight (bands, 3000.0f) == Catch::Approx (3.98f).margin (0.02));
        CHECK (weight (bands, 100.0f) == Catch::Approx (1.0f).margin (0.01));
        CHECK (weight (bands, 15000.0f) == Catch::Approx (1.0f).margin (0.1));
    }

    SECTION ("a negative bell lowers sensitivity")
    {
        bands[0] = band (BandShape::bell, 3000.0f, -12.0f, 1.0f);
        CHECK (weight (bands, 3000.0f) == Catch::Approx (0.251f).margin (0.01));
    }

    SECTION ("cuts remove sensitivity on their side, and steeper cuts are steeper")
    {
        bands[0] = band (BandShape::highCut, 6000.0f, 0.0f, 4.0f);
        CHECK (weight (bands, 1000.0f) > 0.99f);
        CHECK (weight (bands, 6000.0f) == Catch::Approx (0.707f).margin (0.01));
        CHECK (weight (bands, 12000.0f) < 0.1f);

        bands[0] = band (BandShape::lowCut, 200.0f, 0.0f, 4.0f);
        CHECK (weight (bands, 5000.0f) > 0.99f);
        CHECK (weight (bands, 100.0f) < 0.1f);

        auto gentle = band (BandShape::highCut, 6000.0f, 0.0f, 1.0f);
        auto steep = band (BandShape::highCut, 6000.0f, 0.0f, 4.0f);
        CHECK (vs::DepthCurve::bandMagnitude (steep, 12000.0f) < vs::DepthCurve::bandMagnitude (gentle, 12000.0f));
    }

    SECTION ("shelves and tilt")
    {
        bands[0] = band (BandShape::highShelf, 4000.0f, 12.0f, 1.0f);
        CHECK (weight (bands, 100.0f) == Catch::Approx (1.0f).margin (0.02));
        CHECK (weight (bands, 16000.0f) > 3.0f);

        bands[0] = band (BandShape::lowShelf, 400.0f, -12.0f, 1.0f);
        CHECK (weight (bands, 50.0f) < 0.3f);
        CHECK (weight (bands, 5000.0f) == Catch::Approx (1.0f).margin (0.02));

        bands[0] = band (BandShape::tilt, 1000.0f, 12.0f, 1.0f);
        CHECK (weight (bands, 1000.0f) == Catch::Approx (1.0f).margin (0.001));
        CHECK (weight (bands, 15000.0f) > 2.0f);
        CHECK (weight (bands, 70.0f) < 0.5f);
    }

    SECTION ("band reject notches the sensitivity")
    {
        bands[0] = band (BandShape::bandReject, 3000.0f, 0.0f, 2.0f);
        CHECK (weight (bands, 3000.0f) < 0.01f);
        CHECK (weight (bands, 100.0f) > 0.95f);
    }

    SECTION ("a bandpass isolates its zone")
    {
        bands[0] = band (BandShape::bandpass, 3000.0f, 0.0f, 2.0f);
        CHECK (weight (bands, 3000.0f) == Catch::Approx (1.0f).margin (0.01));
        CHECK (weight (bands, 500.0f) < 0.15f);
        CHECK (weight (bands, 12000.0f) < 0.15f);
    }

    SECTION ("bandpass nodes run in parallel")
    {
        bands[0] = band (BandShape::bandpass, 1000.0f, 0.0f, 3.0f);
        bands[1] = band (BandShape::bandpass, 8000.0f, 0.0f, 3.0f);
        CHECK (weight (bands, 1000.0f) == Catch::Approx (1.0f).margin (0.01));
        CHECK (weight (bands, 8000.0f) == Catch::Approx (1.0f).margin (0.01));
        CHECK (weight (bands, 2800.0f) < 0.2f);
    }

    SECTION ("other bands still shape what the bandpass lets through")
    {
        bands[0] = band (BandShape::bandpass, 3000.0f, 0.0f, 1.0f);
        bands[1] = band (BandShape::bell, 3000.0f, 6.0f, 1.0f);
        CHECK (weight (bands, 3000.0f) == Catch::Approx (1.995f).margin (0.02));
    }

    SECTION ("the result is bounded")
    {
        for (auto& b : bands)
            b = band (BandShape::bell, 3000.0f, 24.0f, 1.0f);
        CHECK (weight (bands, 3000.0f) <= vs::DepthCurve::maxWeight);
    }
}

TEST_CASE ("DepthCurve only recomputes when the bands change", "[depthcurve]")
{
    vs::DepthCurve curve;
    const auto cfg = vs::FrameConfig::forSampleRate (kSampleRate);
    curve.prepare (cfg);

    vs::BandArray bands {};
    curve.setBands (bands);
    CHECK (juce::exactlyEqual (curve.getWeights()[128], 1.0f));

    bands[0] = band (BandShape::bell, 3000.0f, 12.0f);
    curve.setBands (bands);
    CHECK (curve.getWeights()[128] == Catch::Approx (3.98f).margin (0.05)); // bin 128 = 3 kHz

    bands[0].enabled = false;
    curve.setBands (bands);
    CHECK (juce::exactlyEqual (curve.getWeights()[128], 1.0f));
}

TEST_CASE ("The reduction curve scales with the depth weights", "[depthcurve]")
{
    const auto cfg = vs::FrameConfig::forSampleRate (kSampleRate);
    std::vector<float> power ((size_t) cfg.numBins, 1.0e-5f);
    power[128] = 1.0e-2f; // a 30 dB resonance at 3 kHz

    auto reductionAt128 = [&] (float w, float maxCut) {
        std::vector<float> weights ((size_t) cfg.numBins, w);
        vs::ReductionCurve curve;
        curve.prepare (cfg);
        vs::ReductionParams p;
        p.depth01 = 0.3f;
        p.maxCutDb = maxCut;
        curve.setParams (p);
        for (int i = 0; i < 100; ++i)
            curve.process (power.data(), weights.data());
        return curve.getReductionDb()[128];
    };

    const auto none = reductionAt128 (0.0f, 40.0f);
    const auto one = reductionAt128 (1.0f, 40.0f);
    const auto two = reductionAt128 (2.0f, 40.0f);
    INFO ("weights 0/1/2 give " << none << "/" << one << "/" << two << " dB");
    CHECK (juce::exactlyEqual (none, 0.0f));
    CHECK (one > 2.0f);
    CHECK (two > 1.5f * one);
    CHECK (reductionAt128 (2.0f, 3.0f) <= 3.001f); // max cut still caps it
}

TEST_CASE ("Depth curve parameters", "[depthcurve][params]")
{
    PluginProcessor plugin;

    for (int n = 1; n <= ParamIDs::numBands; ++n)
        for (int p = 0; p < ParamIDs::numBandParams; ++p)
            CHECK (plugin.apvts.getParameter (ParamIDs::bandParamID (n, (BandParam) p)) != nullptr);

    // default: low cut, neutral bell, high cut are active
    for (int n = 1; n <= 3; ++n)
        CHECK (getParam (plugin, ParamIDs::bandParamID (n, BandParam::active)) > 0.5f);
    for (int n = 4; n <= 8; ++n)
        CHECK (getParam (plugin, ParamIDs::bandParamID (n, BandParam::active)) < 0.5f);

    CHECK (getParam (plugin, ParamIDs::bandParamID (1, BandParam::freq)) == Catch::Approx (80.0f).margin (0.5f));
    CHECK (juce::roundToInt (getParam (plugin, ParamIDs::bandParamID (2, BandParam::shape))) == (int) BandShape::bell);

    SECTION ("state round trip")
    {
        setBand (plugin, 4, BandParam::active, 1.0f);
        setBand (plugin, 4, BandParam::shape, (float) (int) BandShape::bandpass);
        setBand (plugin, 4, BandParam::freq, 6300.0f);
        setBand (plugin, 4, BandParam::gain, -7.5f);

        juce::MemoryBlock state;
        plugin.getStateInformation (state);

        PluginProcessor other;
        other.setStateInformation (state.getData(), (int) state.getSize());
        CHECK (getParam (other, ParamIDs::bandParamID (4, BandParam::active)) > 0.5f);
        CHECK (juce::roundToInt (getParam (other, ParamIDs::bandParamID (4, BandParam::shape))) == (int) BandShape::bandpass);
        CHECK (getParam (other, ParamIDs::bandParamID (4, BandParam::freq)) == Catch::Approx (6300.0f).margin (5.0f));
        CHECK (getParam (other, ParamIDs::bandParamID (4, BandParam::gain)) == Catch::Approx (-7.5f).margin (0.2f));
    }
}

TEST_CASE ("The depth curve steers detection in the plugin", "[depthcurve][integration]")
{
    SECTION ("default curve reduces the 3 kHz resonance")
    {
        PluginProcessor plugin;
        useStrongSettings (plugin, 100.0f);
        CHECK (toneReductionDb (plugin) > 6.0);
    }

    SECTION ("a steep low cut above the resonance leaves it alone")
    {
        PluginProcessor plugin;
        useStrongSettings (plugin, 100.0f);
        setBand (plugin, 1, BandParam::freq, 6000.0f);
        setBand (plugin, 1, BandParam::q, 6.0f);
        const auto reduction = toneReductionDb (plugin);
        INFO ("reduction " << reduction << " dB");
        CHECK (reduction < 1.0);
    }

    SECTION ("a bell boost at the resonance increases the reduction")
    {
        PluginProcessor flat;
        useStrongSettings (flat, 25.0f);
        const auto base = toneReductionDb (flat);

        PluginProcessor boosted;
        useStrongSettings (boosted, 25.0f);
        setBand (boosted, 2, BandParam::gain, 12.0f);
        const auto more = toneReductionDb (boosted);

        INFO ("flat " << base << " dB, +12 dB bell " << more << " dB");
        CHECK (more > base + 3.0);
    }

    SECTION ("a bandpass elsewhere protects the resonance")
    {
        PluginProcessor plugin;
        useStrongSettings (plugin, 100.0f);
        setBand (plugin, 4, BandParam::active, 1.0f);
        setBand (plugin, 4, BandParam::shape, (float) (int) BandShape::bandpass);
        setBand (plugin, 4, BandParam::freq, 8000.0f);
        setBand (plugin, 4, BandParam::q, 4.0f);
        const auto reduction = toneReductionDb (plugin);
        INFO ("reduction " << reduction << " dB");
        CHECK (reduction < 3.0); // the unprotected tone loses > 6 dB
    }
}
