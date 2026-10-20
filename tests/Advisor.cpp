#include "helpers/dsp_test_utils.h"
#include <PluginProcessor.h>
#include <advisor/Advisor.h>
#include <analysis/SignalAnalyzer.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <dsp/ParametricEQ.h>

using Catch::Approx;

namespace
{
struct Nom
{
    float bass, lowMid, mid, upperMid, high, air;
};
// Mirrors the advisor's nominal table so "typical" features really are typical.
Nom nominal (Source s)
{
    switch (s)
    {
        case Source::vocal: return { -8, 1, 0, -1, -8, -16 };
        case Source::drums: return { 2, -1, -1, -2, -5, -10 };
        case Source::bass: return { 8, 2, -8, -14, -22, -30 };
        case Source::guitar: return { -5, 1, 1, -1, -9, -18 };
        case Source::keys: return { -3, 1, 0, -3, -10, -18 };
        case Source::other: return { -4, 0, 0, -2, -8, -16 };
    }
    return {};
}

Features makeFeatures (Source s, Nom dev = {})
{
    const auto n = nominal (s);
    Features f;
    f.valid = true;
    f.analysedSeconds = f.activeSeconds = 6.0f;
    f.peakDb = -6.0f;
    f.rmsDb = -13.0f;
    f.crestDb = 7.0f;
    f.dynamicRangeDb = 4.0f;
    f.onsetStrengthDb = 1.0f;
    for (int i = 0; i < Features::numBands; ++i)
    {
        const auto fc = Features::bandCentre (i);
        float level = -80.0f;
        if (fc < 60) level = n.bass + dev.bass - 12;
        else if (fc < 200) level = n.bass + dev.bass;
        else if (fc < 500) level = n.lowMid + dev.lowMid;
        else if (fc < 2000) level = n.mid + dev.mid;
        else if (fc < 5000) level = n.upperMid + dev.upperMid;
        else if (fc < 10000) level = n.high + dev.high;
        else level = n.air + dev.air;
        f.bandDb[(size_t) i] = level - 30.0f;
    }
    return f;
}

ParamValues defaults()
{
    PluginProcessor p;
    return p.getParamValues();
}

const ChangeGroup* findGroup (const Recommendation& r, Module m)
{
    for (const auto& g : r.changes)
        if (g.module == m)
            return &g;
    return nullptr;
}

float to (const Recommendation& r, const std::string& id, float fallback = -999.0f)
{
    const auto v = r.targetValues();
    const auto it = v.find (id);
    return it == v.end() ? fallback : it->second;
}

int bandNear (float hz)
{
    int best = 0;
    for (int i = 0; i < Features::numBands; ++i)
        if (std::abs (std::log2 (Features::bandCentre (i) / hz)) < std::abs (std::log2 (Features::bandCentre (best) / hz)))
            best = i;
    return best;
}

constexpr Goal allGoals[] = { Goal::clarity, Goal::warmth, Goal::punch, Goal::presence, Goal::density, Goal::smoothness,
                              Goal::brightness, Goal::body, Goal::dynamicControl, Goal::reducedHarshness };
constexpr Source allSources[] = { Source::vocal, Source::drums, Source::bass, Source::guitar, Source::keys, Source::other };
}

TEST_CASE ("Advisor refuses to guess without enough signal", "[advisor]")
{
    Features f;
    for (auto g : allGoals)
    {
        const auto r = advise (g, Source::vocal, 0.5f, f, defaults());
        CHECK (r.outcome == Outcome::insufficientSignal);
        CHECK (r.changes.empty());
        CHECK_FALSE (r.rationale.empty());
    }
}

TEST_CASE ("Advisor makes no change on a typical signal", "[advisor]")
{
    const auto cur = defaults();
    for (auto s : allSources)
        for (auto g : allGoals)
        {
            const auto r = advise (g, s, 0.7f, makeFeatures (s), cur);
            CAPTURE (goalName (g), sourceName (s));
            CHECK (r.outcome == Outcome::noChangeNeeded);
            CHECK (r.changes.empty());
            CHECK_FALSE (r.rationale.empty()); // always explains why nothing happened
        }
}

TEST_CASE ("Clarity cuts excess low-mids and scales with intensity", "[advisor]")
{
    const auto f = makeFeatures (Source::vocal, { 0, 9, 0, 0, 0, 0 });
    const auto cur = defaults();
    const auto lo = advise (Goal::clarity, Source::vocal, 0.3f, f, cur);
    const auto hi = advise (Goal::clarity, Source::vocal, 0.9f, f, cur);
    REQUIRE (lo.outcome == Outcome::suggested);
    REQUIRE (hi.outcome == Outcome::suggested);
    CHECK (to (hi, ParamIDs::eqGain[1]) < to (lo, ParamIDs::eqGain[1])); // deeper cut at higher intensity
    CHECK (to (hi, ParamIDs::eqGain[1]) >= -4.0f);
    CHECK (to (hi, ParamIDs::eqFreq[1]) >= 200.0f);
    CHECK (to (hi, ParamIDs::eqFreq[1]) <= 450.0f);
    CHECK (hi.evidence == Evidence::strong);
    CHECK_FALSE (hi.measurements.empty());
    CHECK_FALSE (hi.interpretations.empty());
    CHECK_FALSE (hi.manualSteps.empty());
    CHECK (advise (Goal::clarity, Source::vocal, 0.0f, f, cur).changes.empty()); // intensity 0 = no change
}

TEST_CASE ("Warmth adds low-mid weight only when the low-mids are lean", "[advisor]")
{
    const auto cur = defaults();
    const auto lean = advise (Goal::warmth, Source::vocal, 0.6f, makeFeatures (Source::vocal, { 0, -8, 0, 0, 0, 0 }), cur);
    REQUIRE (lean.outcome == Outcome::suggested);
    CHECK (to (lean, ParamIDs::eqGain[1]) > 0.0f);
    CHECK (to (lean, ParamIDs::satEnable) > 0.5f);

    const auto full = advise (Goal::warmth, Source::vocal, 0.6f, makeFeatures (Source::vocal, { 0, 8, 0, 0, 0, 0 }), cur);
    CHECK (full.outcome == Outcome::noChangeNeeded);

    // not blindly applied to everything: a typical signal gets alternatives, not changes
    const auto typical = advise (Goal::warmth, Source::vocal, 0.6f, makeFeatures (Source::vocal), cur);
    CHECK (typical.changes.empty());
    CHECK_FALSE (typical.alternatives.empty());
}

TEST_CASE ("Punch compresses only peaky material and never already-squashed material", "[advisor]")
{
    const auto cur = defaults();
    auto peaky = makeFeatures (Source::drums);
    peaky.crestDb = 14.0f;
    peaky.onsetStrengthDb = 6.0f;
    const auto r = advise (Goal::punch, Source::drums, 0.6f, peaky, cur);
    REQUIRE (r.outcome == Outcome::suggested);
    CHECK (to (r, ParamIDs::compAttack) >= 20.0f); // slow attack lets the transient through
    CHECK (to (r, ParamIDs::compRatio) <= AdvisorLimits::maxRatio);
    CHECK (to (r, ParamIDs::compEnable) > 0.5f);

    auto squashed = peaky;
    squashed.crestDb = 6.0f;
    CHECK (advise (Goal::punch, Source::drums, 0.6f, squashed, cur).changes.empty());
}

TEST_CASE ("Presence lifts a lean 2-5 kHz and refuses to boost a narrow peak", "[advisor]")
{
    const auto cur = defaults();
    const auto lean = advise (Goal::presence, Source::vocal, 0.6f, makeFeatures (Source::vocal, { 0, 0, 0, -8, 0, 0 }), cur);
    REQUIRE (lean.outcome == Outcome::suggested);
    CHECK (to (lean, ParamIDs::eqGain[2]) > 0.0f);
    CHECK (to (lean, ParamIDs::eqFreq[2]) == Approx (3500.0f).margin (1.0f));

    auto peaked = makeFeatures (Source::vocal, { 0, 0, 0, -8, 0, 0 });
    peaked.bandDb[(size_t) bandNear (4000.0f)] += 12.0f;
    const auto r = advise (Goal::presence, Source::vocal, 0.6f, peaked, cur);
    CHECK (r.changes.empty());
    CHECK (r.rationale.find ("harshness") != std::string::npos);

    CHECK (advise (Goal::presence, Source::vocal, 0.6f, makeFeatures (Source::vocal, { 0, 0, 0, 8, 0, 0 }), cur).changes.empty());
}

TEST_CASE ("Brightness lifts a dull top end but not on clipped or already bright audio", "[advisor]")
{
    const auto cur = defaults();
    auto dull = makeFeatures (Source::guitar, { 0, 0, 0, 0, -9, -9 });
    const auto r = advise (Goal::brightness, Source::guitar, 0.6f, dull, cur);
    REQUIRE (r.outcome == Outcome::suggested);
    CHECK (to (r, ParamIDs::eqGain[3]) > 0.0f);

    dull.clippingEvents = 5;
    const auto clipped = advise (Goal::brightness, Source::guitar, 0.6f, dull, cur);
    CHECK (clipped.changes.empty());
    CHECK (clipped.rationale.find ("Clipping") != std::string::npos);

    CHECK (advise (Goal::brightness, Source::guitar, 0.6f, makeFeatures (Source::guitar, { 0, 0, 0, 0, 8, 8 }), cur).changes.empty());
}

TEST_CASE ("Body, density, smoothness and dynamic control follow their evidence", "[advisor]")
{
    const auto cur = defaults();
    CHECK (to (advise (Goal::body, Source::bass, 0.6f, makeFeatures (Source::bass, { -9, 0, 0, 0, 0, 0 }), cur), ParamIDs::eqGain[0]) > 0.0f);
    CHECK (advise (Goal::body, Source::bass, 0.6f, makeFeatures (Source::bass, { 8, 0, 0, 0, 0, 0 }), cur).changes.empty());

    auto dynamic = makeFeatures (Source::vocal);
    dynamic.dynamicRangeDb = 11.0f;
    dynamic.crestDb = 15.0f;
    for (auto g : { Goal::density, Goal::smoothness, Goal::dynamicControl })
    {
        const auto r = advise (g, Source::vocal, 0.6f, dynamic, cur);
        CAPTURE (goalName (g));
        REQUIRE (r.outcome == Outcome::suggested);
        const auto* c = findGroup (r, Module::compressor);
        REQUIRE (c != nullptr);
        CHECK (to (r, ParamIDs::compRatio) <= AdvisorLimits::maxRatio);
        // threshold is placed below the typical peak level so something is actually compressed
        CHECK (to (r, ParamIDs::compThreshold) < dynamic.rmsDb + 0.5f * dynamic.crestDb);
        CHECK (to (r, ParamIDs::compMakeup) <= 0.0f); // no free loudness
    }
    CHECK (advise (Goal::dynamicControl, Source::vocal, 0.6f, makeFeatures (Source::vocal), cur).changes.empty());
}

TEST_CASE ("Reduced harshness targets the measured resonance", "[advisor]")
{
    const auto cur = defaults();
    auto f = makeFeatures (Source::vocal);
    f.bandDb[(size_t) bandNear (4500.0f)] += 10.0f;
    const auto r = advise (Goal::reducedHarshness, Source::vocal, 0.6f, f, cur);
    REQUIRE (r.outcome == Outcome::suggested);
    CHECK (to (r, ParamIDs::eqFreq[2]) == Approx (Features::bandCentre (bandNear (4500.0f))).epsilon (0.02));
    CHECK (to (r, ParamIDs::eqGain[2]) < -1.0f);
    CHECK (to (r, ParamIDs::eqGain[2]) >= -AdvisorLimits::maxEqGainDb);
    CHECK (to (r, ParamIDs::eqQ[2]) > 1.5f); // narrow

    // a peak outside 2-8 kHz is not "harshness"
    auto other = makeFeatures (Source::vocal);
    other.bandDb[(size_t) bandNear (1000.0f)] += 12.0f;
    CHECK (advise (Goal::reducedHarshness, Source::vocal, 0.6f, other, cur).changes.empty());
}

TEST_CASE ("Advisor respects what the user has already set", "[advisor]")
{
    auto cur = defaults();
    auto f = makeFeatures (Source::vocal);
    f.bandDb[(size_t) bandNear (4500.0f)] += 10.0f;

    SECTION ("a band already in use keeps its frequency and the total stays within the safety limit")
    {
        cur[ParamIDs::eqGain[2]] = -5.0f;
        cur[ParamIDs::eqFreq[2]] = 3000.0f;
        const auto r = advise (Goal::reducedHarshness, Source::vocal, 1.0f, f, cur);
        REQUIRE (r.outcome == Outcome::suggested);
        CHECK (to (r, ParamIDs::eqFreq[2], 3000.0f) == Approx (3000.0f)); // untouched
        CHECK (to (r, ParamIDs::eqGain[2]) >= -AdvisorLimits::maxEqGainDb);
        CHECK (to (r, ParamIDs::eqGain[2]) <= -5.0f);
    }
    SECTION ("a compressor that is already on is left alone")
    {
        cur[ParamIDs::compEnable] = 1.0f;
        cur[ParamIDs::compRatio] = 8.0f;
        auto dynamic = makeFeatures (Source::vocal);
        dynamic.dynamicRangeDb = 12.0f;
        dynamic.crestDb = 15.0f;
        const auto r = advise (Goal::dynamicControl, Source::vocal, 0.8f, dynamic, cur);
        CHECK (r.changes.empty());
        CHECK (to (r, ParamIDs::compRatio, 8.0f) == Approx (8.0f));
    }
    SECTION ("a saturator that is already on is left alone")
    {
        cur[ParamIDs::satEnable] = 1.0f;
        const auto r = advise (Goal::warmth, Source::vocal, 0.8f, makeFeatures (Source::vocal, { 0, -8, 0, 0, 0, 0 }), cur);
        REQUIRE (r.outcome == Outcome::suggested);
        CHECK (findGroup (r, Module::saturation) == nullptr);
    }
}

TEST_CASE ("Advisor output is always inside the safety limits (property test)", "[advisor][property]")
{
    juce::Random rng (2024);
    const auto cur = defaults();
    int suggested = 0, none = 0;
    for (int trial = 0; trial < 3000; ++trial)
    {
        Features f;
        f.valid = rng.nextInt (20) != 0;
        f.activeSeconds = 1.0f + rng.nextFloat() * 10.0f;
        f.peakDb = -30.0f + rng.nextFloat() * 30.0f;
        f.rmsDb = f.peakDb - 3.0f - rng.nextFloat() * 25.0f;
        f.crestDb = f.peakDb - f.rmsDb;
        f.dynamicRangeDb = rng.nextFloat() * 25.0f;
        f.onsetStrengthDb = rng.nextFloat() * 12.0f;
        f.clippingEvents = rng.nextInt (8);
        for (auto& b : f.bandDb)
            b = -60.0f + rng.nextFloat() * 50.0f;

        const auto goal = allGoals[rng.nextInt (numGoals)];
        const auto source = allSources[rng.nextInt (numSources)];
        const auto intensity = rng.nextFloat();
        const auto r = advise (goal, source, intensity, f, cur);
        const auto again = advise (goal, source, intensity, f, cur);

        // deterministic
        REQUIRE (r.targetValues() == again.targetValues());
        REQUIRE (r.headline == again.headline);

        // consistent
        if (r.outcome == Outcome::suggested)
        {
            ++suggested;
            REQUIRE_FALSE (r.changes.empty());
            REQUIRE (r.evidence != Evidence::weak);
            REQUIRE_FALSE (r.manualSteps.empty());
        }
        else
        {
            ++none;
            REQUIRE (r.changes.empty());
            REQUIRE_FALSE (r.rationale.empty());
        }

        // bounded
        for (const auto& [id, v] : r.targetValues())
        {
            REQUIRE (std::isfinite (v));
            for (size_t i = 0; i < 4; ++i)
            {
                if (id == ParamIDs::eqGain[i]) REQUIRE (std::abs (v) <= AdvisorLimits::maxEqGainDb);
                if (id == ParamIDs::eqFreq[i]) REQUIRE ((v >= Params::eqFreqRange[i].min && v <= Params::eqFreqRange[i].max));
                if (id == ParamIDs::eqQ[i]) REQUIRE ((v >= Params::eqQRange.min && v <= Params::eqQRange.max));
            }
            if (id == ParamIDs::compRatio) REQUIRE (v <= AdvisorLimits::maxRatio);
            if (id == ParamIDs::compThreshold) REQUIRE ((v >= Params::thresholdRange.min && v <= Params::thresholdRange.max));
            if (id == ParamIDs::compMakeup) REQUIRE (v <= 0.0f);
            if (id == ParamIDs::satDrive) REQUIRE (v <= AdvisorLimits::maxSatDriveDb);
            if (id == ParamIDs::satMix) REQUIRE (v <= AdvisorLimits::maxSatMix);
        }

        // every id it touches is a real parameter
        PluginProcessor* none_ = nullptr;
        juce::ignoreUnused (none_);
    }
    WARN ("property test: " << suggested << " suggestions, " << none << " no-change outcomes");
    CHECK (suggested > 100);
    CHECK (none > 100); // the advisor says "no" a lot, as intended
}

TEST_CASE ("Every parameter id a recommendation touches exists in the plugin", "[advisor]")
{
    PluginProcessor p;
    juce::Random rng (5);
    for (int trial = 0; trial < 500; ++trial)
    {
        Features f = makeFeatures (allSources[rng.nextInt (numSources)], { (rng.nextFloat() - 0.5f) * 24, (rng.nextFloat() - 0.5f) * 24, 0, (rng.nextFloat() - 0.5f) * 24, (rng.nextFloat() - 0.5f) * 24, (rng.nextFloat() - 0.5f) * 24 });
        f.crestDb = 8.0f + rng.nextFloat() * 8.0f;
        f.dynamicRangeDb = 3.0f + rng.nextFloat() * 12.0f;
        f.onsetStrengthDb = rng.nextFloat() * 10.0f;
        const auto r = advise (allGoals[rng.nextInt (numGoals)], allSources[rng.nextInt (numSources)], rng.nextFloat(), f, p.getParamValues());
        for (const auto& [id, v] : r.targetValues())
            REQUIRE (p.apvts.getParameter (id) != nullptr);
    }
}

TEST_CASE ("End to end: real audio with a resonance -> analysis -> advice -> EQ response", "[advisor][e2e]")
{
    constexpr double fs = 48000.0;
    const int n = (int) fs * 6;

    // pink-ish noise (Paul Kellet's filter) plus a 4.2 kHz tone standing well above the noise
    juce::Random rng (99);
    juce::AudioBuffer<float> b (2, n);
    for (int ch = 0; ch < 2; ++ch)
    {
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (int i = 0; i < n; ++i)
        {
            const float white = rng.nextFloat() * 2.0f - 1.0f;
            b0 = 0.99886f * b0 + white * 0.0555179f;
            b1 = 0.99332f * b1 + white * 0.0750759f;
            b2 = 0.96900f * b2 + white * 0.1538520f;
            b3 = 0.86650f * b3 + white * 0.3104856f;
            b4 = 0.55000f * b4 + white * 0.5329522f;
            b5 = -0.7616f * b5 - white * 0.0168980f;
            const float pink = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f) * 0.11f;
            b6 = white * 0.115926f;
            b.setSample (ch, i, 0.25f * pink + 0.02f * (float) std::sin (2.0 * std::numbers::pi * 4200.0 * i / fs));
        }
    }

    const auto f = SignalAnalyzer::analyse (b.getReadPointer (0), b.getReadPointer (1), n, fs);
    REQUIRE (f.valid);
    const auto r = advise (Goal::reducedHarshness, Source::other, 0.6f, f, defaults());
    REQUIRE (r.outcome == Outcome::suggested);

    EqParams p;
    p.bands[2] = { to (r, ParamIDs::eqFreq[2]), to (r, ParamIDs::eqGain[2]), to (r, ParamIDs::eqQ[2]) };
    CHECK (std::abs (std::log2 (p.bands[2].freq / 4200.0f)) < 1.0f / 6.0f); // within a 1/6 octave of the real resonance
    CHECK (p.bands[2].gainDb < -1.0f);
    CHECK (ParametricEQ::responseDb (p, 4200.0, fs) < -0.8); // the EQ really attenuates the resonance
    CHECK (std::abs (ParametricEQ::responseDb (p, 500.0, fs)) < 0.3); // and leaves the rest alone
}
