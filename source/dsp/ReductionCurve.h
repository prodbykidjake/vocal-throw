#pragma once

#include "FrameConfig.h"
#include <vector>

namespace vs
{
struct ReductionParams
{
    bool hard = false;
    float depth01 = 0.4f;
    float detail01 = 0.5f;
    float attack01 = 0.5f;
    float release01 = 0.5f;
    float maxCutDb = 12.0f;

    // How far a bin has to stand out of the envelope to count as a resonance: 0 = anything, 1 = only the
    // tallest peaks. The knee in dB grows as 0.5 + 7.5 * selectivity^2 (46 % is about the old fixed 2 dB).
    float selectivity01 = 0.46f;

    // Per-channel multiplier on Depth (1 = unchanged, 0 = none). Used for the stereo balance.
    float channelScale = 1.0f;

    // Tilts, -1..+1. "Low" acts below ~500 Hz and "high" above ~2 kHz, like very gentle shelves.
    // Positive detail tilt = narrower, positive attack/release tilt = slower.
    float detailLow = 0.0f, detailHigh = 0.0f;
    float attackLow = 0.0f, attackHigh = 0.0f;
    float releaseLow = 0.0f, releaseHigh = 0.0f;

    bool operator== (const ReductionParams&) const = default;
};

/** Turns a power spectrum into a per-bin gain reduction (in positive dB).

    A resonance is a bin that sticks out of a smoothed version of its own spectrum. Detail sets how
    narrow that smoothing is. Soft mode only looks at that relative excess (so it ignores level), Hard
    mode also scales by the absolute level of the bin. Attack/release run per bin, faster up top. */
class ReductionCurve
{
public:
    void prepare (const FrameConfig& config);
    void reset();
    void setParams (const ReductionParams& p);

    /** Analysis hop in samples (the frame rate of process()). Rebuilds the attack/release tables, no allocation. */
    void setHopSize (int hop);

    /** @param power numBins linear powers, full-scale sine = 1.0
        @param depthWeight optional numBins multipliers on detection sensitivity (the depth curve) */
    void process (const float* power, const float* depthWeight = nullptr) noexcept;

    const float* getReductionDb() const noexcept { return reduction.data(); }

private:
    void rebuildEnvelopeTables();
    void rebuildBallisticsTables();

    FrameConfig cfg;
    ReductionParams params;
    float kneeDb = 2.09f;

    std::vector<int> loIdx, hiIdx;
    std::vector<float> attackCoef, releaseCoef, freqWeight, lowShelf, highShelf;
    std::vector<float> specDb, clipped, target, smoothed, reduction;
    std::vector<double> prefix;
};
} // namespace vs
