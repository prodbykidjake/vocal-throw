#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

// A mono vocal clip, ready to be played back by StackEngine.
// Immutable once built, so the audio thread can read it while the UI holds a reference.
struct SampleData
{
    static constexpr double maxSeconds = 10.0;

    std::vector<float> samples;
    double sampleRate = 44100.0;
    juce::String name;

    int size() const noexcept { return static_cast<int> (samples.size()); }

    // Mixes to mono, trims silence at both ends, normalises the peak, and caps the length.
    // Returns nullptr if the clip is empty or silent.
    static std::shared_ptr<SampleData> fromBuffer (const juce::AudioBuffer<float>& buffer, double sampleRate, const juce::String& name);

    // Returns nullptr and fills errorMessage if the file can't be read.
    static std::shared_ptr<SampleData> fromFile (const juce::File& file, juce::AudioFormatManager& formats, juce::String& errorMessage);

    // 16-bit PCM as base64, so the clip is saved inside the DAW project.
    juce::String toBase64() const;
    static std::shared_ptr<SampleData> fromBase64 (const juce::String& data, double sampleRate, const juce::String& name);
};
