#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Single source of truth for parameter IDs and layout.
// IDs are part of the saved-session contract: never rename or reuse one once released.
namespace ParamIDs
{
// global
inline constexpr auto inputGain = "inputGain";
inline constexpr auto outputGain = "outputGain";
inline constexpr auto bypass = "bypass";
inline constexpr auto loudnessMatch = "loudnessMatch";
inline constexpr auto autoGain = "autoGain";

// equalizer: high-pass + 4 bands (1 low shelf, 2 and 3 bells, 4 high shelf)
inline constexpr auto eqEnable = "eqEnable";
inline constexpr auto eqHpEnable = "eqHpEnable";
inline constexpr auto eqHpFreq = "eqHpFreq";
inline constexpr const char* eqFreq[4] = { "eq1Freq", "eq2Freq", "eq3Freq", "eq4Freq" };
inline constexpr const char* eqGain[4] = { "eq1Gain", "eq2Gain", "eq3Gain", "eq4Gain" };
inline constexpr const char* eqQ[4] = { "eq1Q", "eq2Q", "eq3Q", "eq4Q" };

// compressor
inline constexpr auto compEnable = "compEnable";
inline constexpr auto compThreshold = "compThreshold";
inline constexpr auto compRatio = "compRatio";
inline constexpr auto compAttack = "compAttack";
inline constexpr auto compRelease = "compRelease";
inline constexpr auto compKnee = "compKnee";
inline constexpr auto compMakeup = "compMakeup";
inline constexpr auto compMix = "compMix";

// saturation
inline constexpr auto satEnable = "satEnable";
inline constexpr auto satDrive = "satDrive";
inline constexpr auto satMix = "satMix";
}

namespace Params
{
inline constexpr int version = 1;
inline constexpr float gainMinDb = -24.0f;
inline constexpr float gainMaxDb = 24.0f;

struct Range
{
    float min, max, def;
};

// Defaults: EQ flat and enabled, compressor and saturator present but switched off, so a fresh
// instance is transparent until the user (or a recommendation) turns something on.
inline constexpr Range eqFreqRange[4] = { { 30.0f, 400.0f, 100.0f }, { 60.0f, 2000.0f, 300.0f }, { 400.0f, 12000.0f, 3000.0f }, { 2000.0f, 20000.0f, 8000.0f } };
inline constexpr Range eqGainRange = { -12.0f, 12.0f, 0.0f };
inline constexpr Range eqQRange = { 0.3f, 8.0f, 1.0f };
inline constexpr Range hpFreqRange = { 20.0f, 400.0f, 80.0f };

inline constexpr Range thresholdRange = { -60.0f, 0.0f, -18.0f };
inline constexpr Range ratioRange = { 1.0f, 20.0f, 2.0f };
inline constexpr Range attackRange = { 0.1f, 200.0f, 10.0f };
inline constexpr Range releaseRange = { 5.0f, 2000.0f, 120.0f };
inline constexpr Range kneeRange = { 0.0f, 24.0f, 6.0f };
inline constexpr Range makeupRange = { 0.0f, 24.0f, 0.0f };
inline constexpr Range mixRange = { 0.0f, 100.0f, 100.0f };
inline constexpr Range driveRange = { 0.0f, 24.0f, 3.0f };

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using juce::AudioParameterBool;
    using juce::AudioParameterFloat;
    using juce::NormalisableRange;
    using juce::ParameterID;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // skewCentre <= 0 means linear; otherwise the slider midpoint maps to that value.
    auto addFloat = [&] (const char* id, const juce::String& name, const Range& r, const char* unit, float skewCentre = 0.0f) {
        // Continuous (no interval): snapping a symmetric range to 0.01 steps made 0 dB read as -5e-7 dB under fast-math.
        NormalisableRange<float> nr (r.min, r.max);
        if (skewCentre > 0.0f)
            nr.setSkewForCentre (skewCentre);
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, version }, name, nr, r.def,
                                                           juce::AudioParameterFloatAttributes().withLabel (unit)));
    };
    auto addBool = [&] (const char* id, const char* name, bool def) {
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { id, version }, name, def));
    };

    const Range gainRange { gainMinDb, gainMaxDb, 0.0f };
    addFloat (ParamIDs::inputGain, "Input Gain", gainRange, "dB");
    addFloat (ParamIDs::outputGain, "Output Gain", gainRange, "dB");
    addBool (ParamIDs::bypass, "Bypass", false);
    addBool (ParamIDs::loudnessMatch, "Match Loudness On Bypass", false);
    addBool (ParamIDs::autoGain, "Auto Level Match", true);

    addBool (ParamIDs::eqEnable, "EQ Enable", true);
    addBool (ParamIDs::eqHpEnable, "EQ High-Pass Enable", false);
    addFloat (ParamIDs::eqHpFreq, "EQ High-Pass Freq", hpFreqRange, "Hz", 100.0f);
    const char* bandNames[4] = { "Low Shelf", "Bell 1", "Bell 2", "High Shelf" };
    for (int i = 0; i < 4; ++i)
    {
        const auto n = juce::String (i + 1);
        addFloat (ParamIDs::eqFreq[i], "EQ " + n + " " + bandNames[i] + " Freq", eqFreqRange[i], "Hz",
                  std::sqrt (eqFreqRange[i].min * eqFreqRange[i].max));
        addFloat (ParamIDs::eqGain[i], "EQ " + n + " Gain", eqGainRange, "dB");
        addFloat (ParamIDs::eqQ[i], "EQ " + n + " Q", eqQRange, "", 1.0f);
    }

    addBool (ParamIDs::compEnable, "Compressor Enable", false);
    addFloat (ParamIDs::compThreshold, "Comp Threshold", thresholdRange, "dB");
    addFloat (ParamIDs::compRatio, "Comp Ratio", ratioRange, ":1", 4.0f);
    addFloat (ParamIDs::compAttack, "Comp Attack", attackRange, "ms", 10.0f);
    addFloat (ParamIDs::compRelease, "Comp Release", releaseRange, "ms", 150.0f);
    addFloat (ParamIDs::compKnee, "Comp Knee", kneeRange, "dB");
    addFloat (ParamIDs::compMakeup, "Comp Makeup", makeupRange, "dB");
    addFloat (ParamIDs::compMix, "Comp Mix", mixRange, "%");

    addBool (ParamIDs::satEnable, "Saturation Enable", false);
    addFloat (ParamIDs::satDrive, "Saturation Drive", driveRange, "dB");
    addFloat (ParamIDs::satMix, "Saturation Mix", mixRange, "%");

    return layout;
}
}
