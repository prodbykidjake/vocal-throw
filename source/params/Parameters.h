#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Single source of truth for parameter IDs and layout.
// IDs are part of the saved-session contract: never rename or reuse one once released.
namespace ParamIDs
{
inline constexpr auto inputGain = "inputGain";
inline constexpr auto outputGain = "outputGain";
inline constexpr auto bypass = "bypass";
}

namespace Params
{
inline constexpr int version = 1;
inline constexpr float gainMinDb = -24.0f;
inline constexpr float gainMaxDb = 24.0f;

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using juce::AudioParameterBool;
    using juce::AudioParameterFloat;
    using juce::NormalisableRange;
    using juce::ParameterID;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto gainAttributes = juce::AudioParameterFloatAttributes().withLabel ("dB");

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::inputGain, version },
                                                       "Input Gain",
                                                       NormalisableRange<float> (gainMinDb, gainMaxDb, 0.01f),
                                                       0.0f,
                                                       gainAttributes));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::outputGain, version },
                                                       "Output Gain",
                                                       NormalisableRange<float> (gainMinDb, gainMaxDb, 0.01f),
                                                       0.0f,
                                                       gainAttributes));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::bypass, version },
                                                      "Bypass",
                                                      false));
    return layout;
}
}
