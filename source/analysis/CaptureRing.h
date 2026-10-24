#pragma once

#include <atomic>
#include <cstdint>
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>

// Continuously overwritten ring holding the most recent *audible* input, written by the audio thread and
// snapshotted by the message thread. Blocks quieter than -75 dBFS are not recorded, so "the last N seconds"
// means the last N seconds of actual signal, even if playback stopped a while ago or the editor was closed.
// Single writer, any reader: the writer never waits; the reader copies a window much smaller than the ring
// (so the writer cannot lap it) and verifies afterwards.
class CaptureRing
{
public:
    static constexpr int capacity = 1 << 19; // 524,288 samples per channel (~10.9 s at 48 kHz)
    static constexpr float silenceThreshold = 0.000178f; // -75 dBFS

    CaptureRing() : left (std::make_unique<float[]> (capacity)), right (std::make_unique<float[]> (capacity)) {}

    // Audio thread. No allocation, no locks.
    void push (const juce::AudioBuffer<float>& buffer, int numChannels)
    {
        const int n = buffer.getNumSamples();
        float peak = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            peak = std::max (peak, buffer.getMagnitude (ch, 0, n));
        if (peak < silenceThreshold || n > capacity / 4)
            return;

        const auto w = written.load (std::memory_order_relaxed);
        const auto* l = buffer.getReadPointer (0);
        const auto* r = buffer.getReadPointer (numChannels > 1 ? 1 : 0);
        for (int i = 0; i < n; ++i)
        {
            const auto idx = (std::size_t) ((w + (std::uint64_t) i) & (capacity - 1));
            left[idx] = l[i];
            right[idx] = r[i];
        }
        written.store (w + (std::uint64_t) n, std::memory_order_release);
    }

    // Message thread. Copies up to maxSamples of the most recent audio (never more than 3/4 of the ring) and
    // returns how many were copied. Fine to call at any time, including while the audio thread is running.
    int copyRecent (float* outLeft, float* outRight, int maxSamples) const
    {
        const auto limit = (std::uint64_t) (capacity * 3 / 4);
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const auto w = written.load (std::memory_order_acquire);
            const auto n = std::min ({ (std::uint64_t) std::max (0, maxSamples), w, limit });
            const auto start = w - n;
            for (std::uint64_t i = 0; i < n; ++i)
            {
                const auto idx = (std::size_t) ((start + i) & (capacity - 1));
                outLeft[i] = left[idx];
                outRight[i] = right[idx];
            }
            if (written.load (std::memory_order_acquire) - start <= (std::uint64_t) capacity)
                return (int) n; // the writer did not lap what we read
        }
        return 0;
    }

    std::uint64_t totalWritten() const { return written.load (std::memory_order_acquire); }

private:
    std::unique_ptr<float[]> left, right;
    std::atomic<std::uint64_t> written { 0 };
};
