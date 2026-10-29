#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

class OnePoleLowpass
{
public:
    void setCutoff (float hz, double sampleRate) noexcept
    {
        const auto limited = juce::jmin (hz, static_cast<float> (sampleRate) * 0.45f);
        coefficient = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * limited / static_cast<float> (sampleRate));
    }

    float process (float x) noexcept
    {
        state += coefficient * (x - state);
        return state;
    }

    // passes x through unfiltered but keeps the state in step, so switching back on doesn't click
    float track (float x) noexcept
    {
        state = x;
        return x;
    }

    void reset() noexcept { state = 0.0f; }

private:
    float coefficient = 1.0f;
    float state = 0.0f;
};

// Tempo-synced echo whose repeats bounce between left and right.
class PingPongEcho
{
public:
    static constexpr float feedback = 0.45f;
    static constexpr float maxDelaySeconds = 2.0f;

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        const auto size = static_cast<size_t> (sampleRate * (static_cast<double> (maxDelaySeconds) + 0.1));
        bufferL.assign (size, 0.0f);
        bufferR.assign (size, 0.0f);
        writeIndex = 0;
        delay.reset (sampleRate, 0.05);
        delay.setCurrentAndTargetValue (static_cast<float> (sampleRate) * 0.25f);
        dampL.setCutoff (4500.0f, sampleRate);
        dampR.setCutoff (4500.0f, sampleRate);
        dampL.reset();
        dampR.reset();
    }

    void setDelaySeconds (float seconds)
    {
        delay.setTargetValue (juce::jlimit (0.001f, maxDelaySeconds, seconds) * static_cast<float> (sampleRate));
    }

    // Adds the echo into l/r. mix is how loud the repeats are.
    void process (float& l, float& r, float mix) noexcept
    {
        const auto size = static_cast<int> (bufferL.size());
        const auto d = delay.getNextValue();

        auto readPos = static_cast<float> (writeIndex) - d;

        if (readPos < 0.0f)
            readPos += static_cast<float> (size);

        const auto i0 = static_cast<int> (readPos) % size;
        const auto i1 = (i0 + 1) % size;
        const auto frac = readPos - std::floor (readPos);

        const auto echoL = bufferL[static_cast<size_t> (i0)] + frac * (bufferL[static_cast<size_t> (i1)] - bufferL[static_cast<size_t> (i0)]);
        const auto echoR = bufferR[static_cast<size_t> (i0)] + frac * (bufferR[static_cast<size_t> (i1)] - bufferR[static_cast<size_t> (i0)]);

        // the input enters on the left only, so repeats alternate L, R, L, R...
        bufferL[static_cast<size_t> (writeIndex)] = (l + r) * 0.5f + feedback * dampR.process (echoR);
        bufferR[static_cast<size_t> (writeIndex)] = feedback * dampL.process (echoL);
        writeIndex = (writeIndex + 1) % size;

        l += echoL * mix;
        r += echoR * mix;
    }

private:
    std::vector<float> bufferL, bufferR;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> delay;
    OnePoleLowpass dampL, dampR;
    double sampleRate = 44100.0;
    int writeIndex = 0;
};

// Gentle clipper that leaves everything below 0.8 untouched.
inline float softClip (float x) noexcept
{
    constexpr float knee = 0.8f;
    const auto a = std::abs (x);

    if (a <= knee)
        return x;

    const auto clipped = knee + (1.0f - knee) * std::tanh ((a - knee) / (1.0f - knee));
    return x < 0.0f ? -clipped : clipped;
}
