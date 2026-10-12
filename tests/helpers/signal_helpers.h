#pragma once

#include <PluginProcessor.h>
#include <cmath>
#include <vector>

namespace testing_helpers
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 480;

inline void setParam (PluginProcessor& plugin, const char* id, float plainValue)
{
    auto* p = plugin.apvts.getParameter (id);
    jassert (p != nullptr);
    p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
}

inline float getParam (PluginProcessor& plugin, const char* id)
{
    auto* p = plugin.apvts.getParameter (id);
    return p->convertFrom0to1 (p->getValue());
}

inline std::vector<float> whiteNoise (int numSamples, float stdDev, int seed = 1234)
{
    juce::Random rng (seed);
    std::vector<float> x ((size_t) numSamples);
    for (auto& s : x)
        s = stdDev * 1.7320508f * (rng.nextFloat() * 2.0f - 1.0f); // uniform with the requested rms
    return x;
}

inline void addSine (std::vector<float>& x, double freq, float amplitude, double sampleRate = kSampleRate)
{
    for (size_t i = 0; i < x.size(); ++i)
        x[i] += amplitude * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * (double) i / sampleRate);
}

/** Runs a stereo (L = R = input) signal through the plugin in host-sized blocks, returns the left channel. */
inline std::vector<float> run (PluginProcessor& plugin, const std::vector<float>& input, double sampleRate = kSampleRate)
{
    plugin.setPlayConfigDetails (2, 2, sampleRate, kBlockSize);
    plugin.prepareToPlay (sampleRate, kBlockSize);

    std::vector<float> out (input.size());
    juce::AudioBuffer<float> buffer (2, kBlockSize);
    juce::MidiBuffer midi;

    for (size_t pos = 0; pos < input.size(); pos += (size_t) kBlockSize)
    {
        const auto n = (int) std::min ((size_t) kBlockSize, input.size() - pos);
        buffer.setSize (2, n, false, false, true);
        for (int c = 0; c < 2; ++c)
            std::copy (input.begin() + (long) pos, input.begin() + (long) pos + n, buffer.getWritePointer (c));

        plugin.processBlock (buffer, midi);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + n, out.begin() + (long) pos);
    }
    return out;
}

/** Hann-windowed power of one frequency over [start, start + length). */
inline double tonePower (const std::vector<float>& x, size_t start, size_t length, double freq, double sampleRate = kSampleRate)
{
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < length; ++i)
    {
        const auto w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (double) i / (double) length);
        const auto phase = 2.0 * juce::MathConstants<double>::pi * freq * (double) i / sampleRate;
        re += w * (double) x[start + i] * std::cos (phase);
        im -= w * (double) x[start + i] * std::sin (phase);
    }
    return re * re + im * im;
}

/** Mean power over several frequencies spread across [lowHz, highHz]. */
inline double bandPower (const std::vector<float>& x, size_t start, size_t length, double lowHz, double highHz, int points = 12)
{
    double sum = 0.0;
    for (int i = 0; i < points; ++i)
        sum += tonePower (x, start, length, lowHz + (highHz - lowHz) * ((double) i + 0.37) / (double) points);
    return sum / points;
}

inline double toDb (double powerRatio) { return 10.0 * std::log10 (powerRatio + 1.0e-30); }

inline bool allFinite (const std::vector<float>& x)
{
    return std::all_of (x.begin(), x.end(), [] (float v) { return std::isfinite (v); });
}
} // namespace testing_helpers
