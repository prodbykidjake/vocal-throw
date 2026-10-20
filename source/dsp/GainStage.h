#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// Smoothed gain in dB. Allocation-free after prepare(); safe to call from the audio thread.
// Smoothing is shared across channels so every channel gets the same ramp.
class GainStage
{
public:
    void prepare (double sampleRate, double rampSeconds = 0.02)
    {
        gain.reset (sampleRate, rampSeconds);
        gain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (targetDb));
    }

    void setGainDb (float db)
    {
        targetDb = db;
        gain.setTargetValue (juce::Decibels::decibelsToGain (db));
    }

    void reset() { gain.setCurrentAndTargetValue (gain.getTargetValue()); }

    // Applies the gain in place to the first numChannels channels of the buffer.
    void process (juce::AudioBuffer<float>& buffer, int numChannels, int startSample, int numSamples)
    {
        if (! gain.isSmoothing())
        {
            const auto g = gain.getTargetValue();
            if (! juce::exactlyEqual (g, 1.0f))
                for (int ch = 0; ch < numChannels; ++ch)
                    juce::FloatVectorOperations::multiply (buffer.getWritePointer (ch, startSample), g, numSamples);
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const auto g = gain.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch, startSample)[i] *= g;
        }
    }

private:
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gain { 1.0f };
    float targetDb = 0.0f;
};
