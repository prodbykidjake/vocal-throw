#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace ParamIDs
{
inline constexpr auto mode = "mode";
inline constexpr auto depth = "depth";
inline constexpr auto detail = "detail";
inline constexpr auto attack = "attack";
inline constexpr auto release = "release";
inline constexpr auto maxCut = "maxCut";
inline constexpr auto wetTrim = "wetTrim";
inline constexpr auto mix = "mix";
inline constexpr auto outGain = "outGain";
inline constexpr auto stereoMode = "stereoMode";
inline constexpr auto link = "link";
inline constexpr auto delta = "delta";
inline constexpr auto bypass = "bypass";
} // namespace ParamIDs

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
