#pragma once

#include "FrameConfig.h"
#include <juce_dsp/juce_dsp.h>

namespace vs
{
/** Short-time spectrum of one channel. Samples are pushed one at a time; once per hop the owner calls
    analyse() to get a (lightly time-smoothed) power spectrum normalised so a full-scale sine is 1.0. */
class SpectralAnalyzer
{
public:
    void prepare (const FrameConfig& config);
    void reset();

    void pushSample (float x) noexcept
    {
        ring[(size_t) writePos] = x;
        writePos = (writePos + 1) & mask;
    }

    /** Windows the last fftSize samples and updates the power spectrum. */
    void analyse() noexcept;

    const float* getPower() const noexcept { return smoothedPower.data(); }

private:
    FrameConfig cfg;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> ring, window, fftData, smoothedPower;
    int writePos = 0;
    int mask = 0;
};
} // namespace vs
