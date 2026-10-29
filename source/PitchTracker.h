#pragma once

#include "Effects.h"
#include <array>

// Follows the pitch of a sung vocal, cheaply enough for the audio thread.
// The input is low-passed and decimated to about 11 kHz, and a YIN detector runs on the last 256 decimated
// samples every 64 (roughly every 6 ms). It covers about 70 Hz to 1 kHz, which is any singing or rapping voice.
class PitchTracker
{
public:
    static constexpr int window = 256;
    static constexpr int hop = 64;
    static constexpr int ringSize = 512;
    static constexpr float threshold = 0.15f;   // YIN's dip threshold: lower is stricter about what counts as pitched
    static constexpr float minLevel = 0.004f;   // about -48 dB: quieter than this is treated as silence

    void prepare (double sampleRate)
    {
        decimation = juce::jmax (1, juce::roundToInt (sampleRate / 11025.0));
        decimatedRate = static_cast<float> (sampleRate) / static_cast<float> (decimation);
        minLag = juce::jmax (2, static_cast<int> (decimatedRate / 1000.0f));
        maxLag = juce::jmin (ringSize - window - 2, static_cast<int> (decimatedRate / 70.0f));
        lowpassA.setCutoff (1800.0f, sampleRate);
        lowpassB.setCutoff (1800.0f, sampleRate);
        reset();
    }

    void reset()
    {
        ring.fill (0.0f);
        writeIndex = 0;
        sum = 0.0f;
        sumCount = 0;
        sinceAnalysis = 0;
        frequency = 0.0f;
        voiced = false;
        lowpassA.reset();
        lowpassB.reset();
    }

    void process (const float* input, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            sum += lowpassB.process (lowpassA.process (input[i]));

            if (++sumCount < decimation)
                continue;

            ring[static_cast<size_t> (writeIndex)] = sum / static_cast<float> (decimation);
            writeIndex = (writeIndex + 1) & (ringSize - 1);
            sum = 0.0f;
            sumCount = 0;

            if (++sinceAnalysis >= hop)
            {
                sinceAnalysis = 0;
                analyse();
            }
        }
    }

    // The latest pitched frequency in Hz. It holds through breaths and consonants; 0 until something pitched is heard.
    float getFrequency() const noexcept { return frequency; }

    // Whether the most recent analysis heard a pitched sound.
    bool isVoiced() const noexcept { return voiced; }

private:
    void analyse() noexcept
    {
        // the newest window + maxLag samples, oldest first
        const auto length = window + maxLag;
        const auto start = writeIndex - length;
        double energy = 0.0;

        for (int i = 0; i < length; ++i)
        {
            frame[static_cast<size_t> (i)] = ring[static_cast<size_t> ((start + i) & (ringSize - 1))];

            if (i >= maxLag)
                energy += static_cast<double> (frame[static_cast<size_t> (i)]) * static_cast<double> (frame[static_cast<size_t> (i)]);
        }

        if (std::sqrt (energy / window) < minLevel)
        {
            voiced = false;
            return;
        }

        // difference function, then YIN's cumulative mean normalisation
        double running = 0.0;
        normalised[0] = 1.0f;

        for (int lag = 1; lag <= maxLag; ++lag)
        {
            double difference = 0.0;

            for (int j = 0; j < window; ++j)
            {
                const auto delta = frame[static_cast<size_t> (maxLag + j)] - frame[static_cast<size_t> (maxLag + j - lag)];
                difference += static_cast<double> (delta) * static_cast<double> (delta);
            }

            running += difference;
            normalised[static_cast<size_t> (lag)] = running > 0.0 ? static_cast<float> (difference * lag / running) : 1.0f;
        }

        // the first dip under the threshold, followed down to its lowest point
        int best = -1;

        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            if (normalised[static_cast<size_t> (lag)] < threshold)
            {
                while (lag + 1 <= maxLag && normalised[static_cast<size_t> (lag + 1)] < normalised[static_cast<size_t> (lag)])
                    ++lag;

                best = lag;
                break;
            }
        }

        if (best < 0)
        {
            voiced = false;
            return;
        }

        // parabolic interpolation between neighbouring lags for sub-sample accuracy
        auto refined = static_cast<float> (best);

        if (best > minLag && best < maxLag)
        {
            const auto a = normalised[static_cast<size_t> (best - 1)];
            const auto b = normalised[static_cast<size_t> (best)];
            const auto c = normalised[static_cast<size_t> (best + 1)];
            const auto curve = a - 2.0f * b + c;

            if (curve > 1.0e-9f)
                refined += 0.5f * (a - c) / curve;
        }

        frequency = decimatedRate / refined;
        voiced = true;
    }

    std::array<float, ringSize> ring {};
    std::array<float, ringSize> frame {};
    std::array<float, ringSize> normalised {};
    OnePoleLowpass lowpassA, lowpassB;
    int decimation = 4;
    float decimatedRate = 11025.0f;
    int minLag = 11, maxLag = 157;
    int writeIndex = 0;
    float sum = 0.0f;
    int sumCount = 0;
    int sinceAnalysis = 0;
    float frequency = 0.0f;
    bool voiced = false;
};
