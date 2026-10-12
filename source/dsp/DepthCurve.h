#pragma once

#include "FrameConfig.h"
#include <array>
#include <vector>

namespace vs
{
/** Shapes of a depth-curve band. The order matches the "shape" choice parameter. */
enum class BandShape
{
    lowCut = 0,
    lowShelf,
    highShelf,
    highCut,
    bell,
    bandpass,
    bandReject,
    tilt
};

inline constexpr int numBandShapes = 8;

/** True if the band's gain is meaningful for its shape (the cuts and band reject have none). */
constexpr bool shapeUsesGain (BandShape s) noexcept
{
    return s != BandShape::lowCut && s != BandShape::highCut && s != BandShape::bandReject;
}

struct BandSettings
{
    bool enabled = false; // active (the node exists) and not bypassed
    BandShape shape = BandShape::bell;
    float freqHz = 1000.0f;
    float gainDb = 0.0f; // sensitivity change; boosting means more suppression there
    float q = 1.0f;      // bell/bandpass/reject: Q. shelves/tilt: steepness. cuts: filter order

    bool sameAs (const BandSettings& o) const noexcept;
};

using BandArray = std::array<BandSettings, 8>;

/** The "depth curve": a per-bin multiplier on how sensitive detection is, built from up to 8 bands.
    It is an inverse/sidechain EQ on the detector. It never touches the audio directly.

    Bands multiply (their dB add). Enabled bandpass bands run in parallel instead: the strongest of
    them sets the base weight, so several bandpasses isolate several zones. With none, the base is 1. */
class DepthCurve
{
public:
    static constexpr float maxWeight = 32.0f;

    void prepare (const FrameConfig& config);

    /** Recomputes the per-bin weights, but only if the settings changed. */
    void setBands (const BandArray& newBands) noexcept;

    /** numBins linear multipliers (1 = neutral) */
    const float* getWeights() const noexcept { return weights.data(); }

    /** Linear multiplier of a single band at a frequency. */
    static float bandMagnitude (const BandSettings& band, float hz) noexcept;

    /** Linear multiplier of the whole curve at a frequency. */
    static float weightAt (const BandArray& bands, float hz) noexcept;

private:
    void recompute() noexcept;

    FrameConfig cfg;
    BandArray bands {};
    bool haveBands = false;
    std::vector<float> weights;
};
} // namespace vs
