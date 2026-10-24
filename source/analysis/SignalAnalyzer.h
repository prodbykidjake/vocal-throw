#pragma once

#include "CaptureRing.h"
#include "Features.h"
#include <vector>

// Message-thread analysis. analyse() is a pure function of the audio (easy to test);
// analyseRecent() snapshots the audio thread's CaptureRing and analyses it.
class SignalAnalyzer
{
public:
    // Pure analysis of a stereo (or mono: pass the same pointer twice) block of audio.
    static Features analyse (const float* left, const float* right, int numSamples, double sampleRate);

    // Analyses the most recent `seconds` of audible input held in the ring (limited by the ring size).
    static Features analyseRecent (const CaptureRing& ring, double sampleRate, float seconds);
};
