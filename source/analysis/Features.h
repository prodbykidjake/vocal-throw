#pragma once

#include <array>
#include <cmath>

// Everything the advisor is allowed to know about the signal. These are measurements only;
// interpretation lives in the advisor.
struct Features
{
    static constexpr int numBands = 60; // 1/6-octave bands, centres 20 Hz * 2^(i/6): 20 Hz .. ~18 kHz

    static constexpr float bandCentre (int i) { return 20.0f * std::exp2 ((float) i / 6.0f); }

    bool valid = false; // false when there was not enough signal to say anything
    float analysedSeconds = 0.0f;
    float activeSeconds = 0.0f; // time above the silence gate (-60 dBFS per 100 ms frame)

    float peakDb = -120.0f;           // sample peak over the analysed audio
    float rmsDb = -120.0f;            // RMS over active frames
    float crestDb = 0.0f;             // peak - RMS
    float dynamicRangeDb = 0.0f;      // P95 - P10 of 100 ms frame levels (active frames)
    float onsetStrengthDb = 0.0f;     // mean of the steepest 10% of 10 ms level rises (dB per 10 ms)
    int clippingEvents = 0;           // runs of >= 3 consecutive samples at/near full scale
    float stereoCorrelation = 1.0f;   // -1..1
    float widthIndex = 0.0f;          // 0 mono .. 1 wide

    // Average band energy over active frames, per 1/6-octave band, in dB (arbitrary but consistent reference).
    // Pink noise reads flat across bands; white noise rises 3 dB per octave.
    std::array<float, numBands> bandDb {};

    // Power-mean level (dB) of the bands whose centre lies in [loHz, hiHz).
    float groupDb (float loHz, float hiHz) const
    {
        double sum = 0.0;
        int count = 0;
        for (int i = 0; i < numBands; ++i)
        {
            const auto f = bandCentre (i);
            if (f >= loHz && f < hiHz)
            {
                sum += std::pow (10.0, bandDb[(size_t) i] / 10.0);
                ++count;
            }
        }
        return count > 0 ? (float) (10.0 * std::log10 (sum / count)) : -120.0f;
    }
};
