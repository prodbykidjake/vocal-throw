#pragma once

#include "CaptureFifo.h"
#include "Features.h"
#include <vector>

// Message-thread analysis. analyse() is a pure function of the audio (easy to test);
// the instance methods keep a rolling history fed from the audio thread's CaptureFifo.
class SignalAnalyzer
{
public:
    static constexpr float historySeconds = 12.0f;

    // Pure analysis of a stereo (or mono: pass the same pointer twice) block of audio.
    static Features analyse (const float* left, const float* right, int numSamples, double sampleRate);

    // Pulls everything queued by the audio thread into the history.
    void drain (CaptureFifo& fifo, double sampleRate);

    // Analyses the most recent `seconds` of history.
    Features analyseRecent (float seconds) const;

    void clear() { count = 0; }
    int available() const { return count; }

private:
    double fs = 0.0;
    std::vector<float> left, right; // ring buffers
    int writePos = 0, count = 0;
};
