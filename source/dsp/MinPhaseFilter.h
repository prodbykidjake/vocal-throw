#pragma once

#include "FrameConfig.h"
#include <juce_dsp/juce_dsp.h>

namespace vs
{
/** Applies a per-hop magnitude curve to a signal through a minimum-phase FIR.

    setCurve() takes the new reduction (positive dB per bin), rebuilds the kernel with the cepstral
    method and starts a one-hop crossfade from the previous kernel, so the filter follows the signal
    without zipper noise. */
class MinPhaseFilter
{
public:
    void prepare (const FrameConfig& config);
    void reset();

    void setCurve (const float* reductionDb) noexcept;

    float process (float x) noexcept
    {
        history[(size_t) writePos] = x;
        history[(size_t) (writePos + K)] = x;
        writePos = writePos + 1 == K ? 0 : writePos + 1;

        // after the increment writePos is the index of the oldest sample
        const float* h = history.data() + writePos;
        const float current = dot (h, kernelCur.data(), K);

        if (fadePos >= cfg.fadeLength)
            return current;

        const float previous = dot (h, kernelPrev.data(), K);
        const float a = (float) ++fadePos * fadeStep;
        return previous + a * (current - previous);
    }

    /** Magnitude response of the current kernel at the analysis bins, for testing. */
    void getCurrentResponse (std::vector<float>& magnitudes) const;

private:
    static float dot (const float* a, const float* b, int n) noexcept
    {
        float s = 0.0f;
        for (int i = 0; i < n; ++i)
            s += a[i] * b[i];
        return s;
    }

    FrameConfig cfg;
    int K = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<juce::dsp::Complex<float>> bufA, bufB;
    std::vector<float> kernelCur, kernelPrev, taper, history;
    int writePos = 0;
    int fadePos = 0;
    float fadeStep = 1.0f;
};
} // namespace vs
