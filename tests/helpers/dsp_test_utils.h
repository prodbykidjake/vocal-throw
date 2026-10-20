#pragma once

#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>
#include <numbers>

namespace test
{
inline juce::AudioBuffer<float> sine (int channels, int samples, double fs, double freq, float amp, double phase = 0.0)
{
    juce::AudioBuffer<float> b (channels, samples);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < samples; ++i)
            b.setSample (ch, i, amp * (float) std::sin (2.0 * std::numbers::pi * freq * i / fs + phase));
    return b;
}

inline juce::AudioBuffer<float> noise (int channels, int samples, unsigned seed, float amp = 0.5f)
{
    juce::Random rng ((juce::int64) seed + 1);
    juce::AudioBuffer<float> b (channels, samples);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < samples; ++i)
            b.setSample (ch, i, amp * (rng.nextFloat() * 2.0f - 1.0f));
    return b;
}

inline juce::AudioBuffer<float> dc (int channels, int samples, float value)
{
    juce::AudioBuffer<float> b (channels, samples);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < samples; ++i)
            b.setSample (ch, i, value);
    return b;
}

inline double rmsDb (const juce::AudioBuffer<float>& b, int ch, int start, int n)
{
    double sq = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double v = b.getSample (ch, start + i);
        sq += v * v;
    }
    return 10.0 * std::log10 (std::max (sq / n, 1.0e-20));
}
}
