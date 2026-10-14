#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/** Factory presets. They are exposed to the host as programs and to the editor's preset arrows. */
namespace presets
{
int count() noexcept;
juce::String name (int index);

/** Loads a preset through the parameters.

    Every sound-shaping parameter is set: anything a preset doesn't mention goes back to its default, so
    presets never inherit leftovers from the previous one. Mix, out gain, bypass, delta and the
    sidechain switches are left alone. Quality is only changed when the quality lock is off. */
void apply (juce::AudioProcessorValueTreeState& state, int index);
} // namespace presets
