#include "Parameters.h"
#include "dsp/DepthCurve.h"

juce::String ParamIDs::bandParamID (int band, BandParam which)
{
    static constexpr const char* suffixes[] = { "active", "enabled", "shape", "freq", "gain", "q" };
    return "band" + juce::String (band) + "_" + suffixes[(int) which];
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    constexpr int version = 1;

    auto percent = [] (const char* id, const char* name, float defaultValue) {
        return std::make_unique<AudioParameterFloat> (ParameterID { id, version }, name,
            NormalisableRange<float> (0.0f, 100.0f, 0.1f), defaultValue,
            AudioParameterFloatAttributes().withLabel ("%"));
    };

    auto decibels = [] (const char* id, const char* name, float minDb, float maxDb, float defaultValue) {
        return std::make_unique<AudioParameterFloat> (ParameterID { id, version }, name,
            NormalisableRange<float> (minDb, maxDb, 0.1f), defaultValue,
            AudioParameterFloatAttributes().withLabel ("dB"));
    };

    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::mode, version }, "Mode", StringArray { "Soft", "Hard" }, 0));
    layout.add (percent (ParamIDs::depth, "Depth", 40.0f));
    layout.add (percent (ParamIDs::detail, "Detail", 50.0f));
    layout.add (percent (ParamIDs::attack, "Attack", 50.0f));
    layout.add (percent (ParamIDs::release, "Release", 50.0f));
    layout.add (decibels (ParamIDs::maxCut, "Max Cut", 1.0f, 40.0f, 12.0f));
    layout.add (decibels (ParamIDs::wetTrim, "Wet Trim", -12.0f, 12.0f, 0.0f));
    layout.add (percent (ParamIDs::mix, "Mix", 100.0f));
    layout.add (decibels (ParamIDs::outGain, "Out Gain", -18.0f, 18.0f, 0.0f));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::stereoMode, version }, "Stereo Mode", StringArray { "L/R", "M/S" }, 0));
    layout.add (percent (ParamIDs::link, "Link", 100.0f));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::delta, version }, "Delta", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::bypass, version }, "Bypass", false));

    auto tilt = [] (const char* id, const char* name) {
        return std::make_unique<AudioParameterFloat> (ParameterID { id, version }, name,
            NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f,
            AudioParameterFloatAttributes().withLabel ("%"));
    };

    layout.add (tilt (ParamIDs::detailLow, "Detail Tilt Low"));
    layout.add (tilt (ParamIDs::detailHigh, "Detail Tilt High"));
    layout.add (tilt (ParamIDs::attackLow, "Attack Tilt Low"));
    layout.add (tilt (ParamIDs::attackHigh, "Attack Tilt High"));
    layout.add (tilt (ParamIDs::releaseLow, "Release Tilt Low"));
    layout.add (tilt (ParamIDs::releaseHigh, "Release Tilt High"));

    // Depth curve bands. 1-3 start active (low cut, neutral bell, high cut), the rest are free slots.
    struct BandDefault
    {
        bool active;
        vs::BandShape shape;
        float freq, q;
    };
    constexpr BandDefault bandDefaults[ParamIDs::numBands] = {
        { true, vs::BandShape::lowCut, 80.0f, 2.0f },
        { true, vs::BandShape::bell, 3000.0f, 1.0f },
        { true, vs::BandShape::highCut, 16000.0f, 2.0f },
        { false, vs::BandShape::bell, 200.0f, 1.0f },
        { false, vs::BandShape::bell, 500.0f, 1.0f },
        { false, vs::BandShape::bell, 1200.0f, 1.0f },
        { false, vs::BandShape::bell, 6000.0f, 1.0f },
        { false, vs::BandShape::bell, 10000.0f, 1.0f },
    };

    for (int i = 0; i < ParamIDs::numBands; ++i)
    {
        using ParamIDs::BandParam;
        const auto n = i + 1;
        const auto& d = bandDefaults[i];
        const auto id = [n] (BandParam p) { return ParamIDs::bandParamID (n, p); };
        const auto name = [n] (const char* what) { return "Band " + String (n) + " " + what; };

        NormalisableRange<float> freqRange (20.0f, 20000.0f, 0.01f);
        freqRange.setSkewForCentre (1000.0f);
        NormalisableRange<float> qRange (0.3f, 10.0f, 0.01f);
        qRange.setSkewForCentre (1.5f);

        layout.add (std::make_unique<AudioParameterBool> (ParameterID { id (BandParam::active), version }, name ("Active"), d.active));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { id (BandParam::enabled), version }, name ("Enabled"), true));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { id (BandParam::shape), version }, name ("Shape"),
            StringArray { "Low Cut", "Low Shelf", "High Shelf", "High Cut", "Bell", "Bandpass", "Band Reject", "Tilt" }, (int) d.shape));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id (BandParam::freq), version }, name ("Frequency"), freqRange, d.freq,
            AudioParameterFloatAttributes().withLabel ("Hz")));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id (BandParam::gain), version }, name ("Depth"),
            NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id (BandParam::q), version }, name ("Q"), qRange, d.q));
    }

    return layout;
}
