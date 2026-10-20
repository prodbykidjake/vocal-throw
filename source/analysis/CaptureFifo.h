#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// Single-producer (audio thread) / single-consumer (message thread) stereo sample queue.
// The audio thread only copies samples in; if the consumer falls behind, samples are dropped
// rather than blocking.
class CaptureFifo
{
public:
    static constexpr int capacity = 1 << 16;

    void push (const juce::AudioBuffer<float>& buffer, int numChannels)
    {
        const int n = buffer.getNumSamples();
        const auto scope = fifo.write (n);
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto* src = buffer.getReadPointer (std::min (ch, numChannels - 1));
            std::copy_n (src, scope.blockSize1, data[(size_t) ch].begin() + scope.startIndex1);
            std::copy_n (src + scope.blockSize1, scope.blockSize2, data[(size_t) ch].begin() + scope.startIndex2);
        }
    }

    // Copies out up to maxSamples samples; returns how many were read.
    int pop (float* left, float* right, int maxSamples)
    {
        const auto scope = fifo.read (maxSamples);
        std::copy_n (data[0].begin() + scope.startIndex1, scope.blockSize1, left);
        std::copy_n (data[0].begin() + scope.startIndex2, scope.blockSize2, left + scope.blockSize1);
        std::copy_n (data[1].begin() + scope.startIndex1, scope.blockSize1, right);
        std::copy_n (data[1].begin() + scope.startIndex2, scope.blockSize2, right + scope.blockSize1);
        return scope.blockSize1 + scope.blockSize2;
    }

    int numReady() const { return fifo.getNumReady(); }

private:
    juce::AbstractFifo fifo { capacity };
    std::array<std::array<float, capacity>, 2> data {};
};
