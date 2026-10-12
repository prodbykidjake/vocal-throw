#include "Parameters.h"

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

    return layout;
}
