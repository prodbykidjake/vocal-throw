#pragma once

#include <cmath>

namespace vs
{
/** Analysis/synthesis sizes derived from the sample rate. The analysis window is defined in time
    (about 43 ms) so the behaviour stays roughly constant across sample rates. */
struct FrameConfig
{
    double sampleRate = 48000.0;
    int fftOrder = 11;
    int fftSize = 2048;
    int hopSize = 512;
    int numBins = 1025;
    int kernelLength = 512;  // taps of the minimum-phase FIR
    int fadeLength = 128;    // samples over which a new kernel is crossfaded in
    int latency = 1024;      // lookahead in samples (half an analysis window)

    float binHz() const noexcept { return (float) (sampleRate / (double) fftSize); }

    static FrameConfig forSampleRate (double sr) noexcept
    {
        FrameConfig c;
        c.sampleRate = sr;
        c.fftOrder = sr <= 50000.0 ? 11 : (sr <= 100000.0 ? 12 : 13);
        c.fftSize = 1 << c.fftOrder;
        c.hopSize = c.fftSize / 4;
        c.numBins = c.fftSize / 2 + 1;
        c.kernelLength = c.fftSize / 4;
        c.fadeLength = c.hopSize / 4;
        c.latency = c.fftSize / 2;
        return c;
    }
};
} // namespace vs
