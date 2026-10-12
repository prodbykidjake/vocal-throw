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
inline constexpr auto sidechain = "sidechain";
inline constexpr auto keyListen = "keyListen";
inline constexpr auto detailLow = "detailLow";
inline constexpr auto detailHigh = "detailHigh";
inline constexpr auto attackLow = "attackLow";
inline constexpr auto attackHigh = "attackHigh";
inline constexpr auto releaseLow = "releaseLow";
inline constexpr auto releaseHigh = "releaseHigh";

inline constexpr int numBands = 8;
enum class BandParam
{
    active, // the node exists
    enabled, // not bypassed
    shape,
    freq,
    gain,
    q
};

/** @param band 1..numBands */
juce::String bandParamID (int band, BandParam which);
} // namespace ParamIDs

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
