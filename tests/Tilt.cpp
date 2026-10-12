#include "helpers/signal_helpers.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <dsp/ReductionCurve.h>

using namespace testing_helpers;

namespace
{
constexpr float kLowHz = 400.0f;
constexpr float kHighHz = 8000.0f;

/** Flat noise floor with an optional pair of peaks/humps, as linear power. */
std::vector<float> makeSpectrum (const vs::FrameConfig& cfg, float peakDb, float humpSigmaOct)
{
    std::vector<float> power ((size_t) cfg.numBins);
    for (int k = 0; k < cfg.numBins; ++k)
    {
        const auto f = std::max ((float) k * cfg.binHz(), 1.0f);
        auto db = -50.0f;
        for (const auto centre : { kLowHz, kHighHz })
        {
            const auto oct = std::log2 (f / centre);
            db += peakDb * std::exp (-oct * oct / (2.0f * humpSigmaOct * humpSigmaOct));
        }
        power[(size_t) k] = std::pow (10.0f, db / 10.0f);
    }
    return power;
}

int binFor (const vs::FrameConfig& cfg, float hz) { return (int) std::round (hz / cfg.binHz()); }

vs::ReductionParams baseParams()
{
    vs::ReductionParams p;
    p.depth01 = 1.0f;
    p.detail01 = 0.5f;
    p.attack01 = 0.5f;
    p.release01 = 0.5f;
    p.maxCutDb = 40.0f;
    return p;
}
} // namespace

TEST_CASE ("Tilt parameters exist and default to zero", "[tilt][params]")
{
    PluginProcessor plugin;
    for (const auto* id : { ParamIDs::detailLow, ParamIDs::detailHigh, ParamIDs::attackLow, ParamIDs::attackHigh, ParamIDs::releaseLow, ParamIDs::releaseHigh })
    {
        REQUIRE (plugin.apvts.getParameter (id) != nullptr);
        CHECK (getParam (plugin, id) == Catch::Approx (0.0f).margin (0.01f));
    }
}

TEST_CASE ("Attack tilt slows only the region it is aimed at", "[tilt]")
{
    const auto cfg = vs::FrameConfig::forSampleRate (kSampleRate);
    const auto quiet = makeSpectrum (cfg, 0.0f, 0.05f);
    const auto peaks = makeSpectrum (cfg, 30.0f, 0.02f);

    auto reductionAfterTwoFrames = [&] (float attackLow, float attackHigh) {
        vs::ReductionCurve curve;
        curve.prepare (cfg);
        auto p = baseParams();
        p.attackLow = attackLow;
        p.attackHigh = attackHigh;
        curve.setParams (p);

        for (int i = 0; i < 20; ++i)
            curve.process (quiet.data());
        for (int i = 0; i < 2; ++i)
            curve.process (peaks.data());

        return std::pair { curve.getReductionDb()[binFor (cfg, kLowHz)], curve.getReductionDb()[binFor (cfg, kHighHz)] };
    };

    const auto [flatLow, flatHigh] = reductionAfterTwoFrames (0.0f, 0.0f);
    const auto [slowLow, slowHigh] = reductionAfterTwoFrames (1.0f, 0.0f);
    const auto [fastLow, fastHigh] = reductionAfterTwoFrames (-1.0f, 0.0f);
    const auto [slowTopLow, slowTopHigh] = reductionAfterTwoFrames (0.0f, 1.0f);

    INFO ("flat " << flatLow << "/" << flatHigh << "  low+ " << slowLow << "/" << slowHigh << "  low- " << fastLow << "/" << fastHigh << "  high+ " << slowTopLow << "/" << slowTopHigh);
    CHECK (flatLow > 1.0f);
    CHECK (slowLow < 0.6f * flatLow);                           // slower attack in the lows
    CHECK (fastLow > flatLow);                                  // negative tilt is faster
    CHECK (slowHigh == Catch::Approx (flatHigh).margin (0.05)); // ... without touching the highs
    CHECK (slowTopHigh < 0.6f * flatHigh);                      // and the other way round
    CHECK (slowTopLow == Catch::Approx (flatLow).margin (0.05));
}

TEST_CASE ("Release tilt slows the recovery only where aimed", "[tilt]")
{
    const auto cfg = vs::FrameConfig::forSampleRate (kSampleRate);
    const auto quiet = makeSpectrum (cfg, 0.0f, 0.05f);
    const auto peaks = makeSpectrum (cfg, 30.0f, 0.02f);

    auto remainingAfterTwoFrames = [&] (float releaseLow, float releaseHigh) {
        vs::ReductionCurve curve;
        curve.prepare (cfg);
        auto p = baseParams();
        p.releaseLow = releaseLow;
        p.releaseHigh = releaseHigh;
        curve.setParams (p);

        for (int i = 0; i < 200; ++i)
            curve.process (peaks.data());
        for (int i = 0; i < 2; ++i)
            curve.process (quiet.data());

        return std::pair { curve.getReductionDb()[binFor (cfg, kLowHz)], curve.getReductionDb()[binFor (cfg, kHighHz)] };
    };

    const auto [flatLow, flatHigh] = remainingAfterTwoFrames (0.0f, 0.0f);
    const auto [slowLow, slowHigh] = remainingAfterTwoFrames (1.0f, 0.0f);

    INFO ("flat " << flatLow << "/" << flatHigh << "  low+ " << slowLow << "/" << slowHigh);
    CHECK (slowLow > flatLow + 1.0f);
    CHECK (slowHigh == Catch::Approx (flatHigh).margin (0.05));
}

TEST_CASE ("Detail tilt changes selectivity only where aimed", "[tilt]")
{
    const auto cfg = vs::FrameConfig::forSampleRate (kSampleRate);
    // humps (sigma 0.12 oct): a wide envelope sees them as resonances, a narrow one follows them
    const auto humps = makeSpectrum (cfg, 12.0f, 0.12f);

    auto settledReduction = [&] (float detailHigh) {
        vs::ReductionCurve curve;
        curve.prepare (cfg);
        auto p = baseParams();
        p.detailHigh = detailHigh;
        curve.setParams (p);
        for (int i = 0; i < 100; ++i)
            curve.process (humps.data());
        return std::pair { curve.getReductionDb()[binFor (cfg, kLowHz)], curve.getReductionDb()[binFor (cfg, kHighHz)] };
    };

    const auto [flatLow, flatHigh] = settledReduction (0.0f);
    const auto [tiltLow, tiltHigh] = settledReduction (1.0f);

    INFO ("flat " << flatLow << "/" << flatHigh << "  high tilt " << tiltLow << "/" << tiltHigh);
    CHECK (flatHigh > 0.5f);
    CHECK (tiltHigh < flatHigh - 0.3f);
    CHECK (tiltLow == Catch::Approx (flatLow).margin (0.01));
}

TEST_CASE ("Tilt settings survive a state round trip", "[tilt][state]")
{
    PluginProcessor a;
    setParam (a, ParamIDs::attackLow, 60.0f);
    setParam (a, ParamIDs::detailHigh, -40.0f);

    juce::MemoryBlock state;
    a.getStateInformation (state);

    PluginProcessor b;
    b.setStateInformation (state.getData(), (int) state.getSize());
    CHECK (getParam (b, ParamIDs::attackLow) == Catch::Approx (60.0f).margin (0.2f));
    CHECK (getParam (b, ParamIDs::detailHigh) == Catch::Approx (-40.0f).margin (0.2f));
}
