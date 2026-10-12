#include "ReductionCurve.h"
#include <algorithm>
#include <cmath>
#include <juce_core/juce_core.h>

namespace vs
{
// Tuning constants -----------------------------------------------------------------------------------
static constexpr float kWidestEnvelopeOct = 1.5f;  // envelope smoothing width at Detail = 0
static constexpr float kNarrowestEnvelopeOct = 0.125f; // ... and at Detail = 1
static constexpr float kPeakClipDb = 3.0f;         // peaks are clipped to this above the first envelope pass
static constexpr float kKneeDb = 2.0f;             // excess below this isn't a resonance
static constexpr float kSoftDepthScale = 2.0f;     // reduction dB per dB of excess at Depth = 1
static constexpr float kHardDepthScale = 3.0f;
static constexpr float kFloorDb = -130.0f;         // nothing is detected below this absolute level
static constexpr float kFullGateDb = -110.0f;
static constexpr float kHardLevelRefDb = -80.0f;   // Hard mode: level factor is 0 here, 1 at +30 dB above
static constexpr float kHardLevelRangeDb = 30.0f;
static constexpr float kLowCutStartHz = 80.0f;     // nothing is cut below here, full effect above kLowCutEndHz
static constexpr float kLowCutEndHz = 200.0f;
// ----------------------------------------------------------------------------------------------------

static float smoothstep (float edge0, float edge1, float x) noexcept
{
    const auto t = juce::jlimit (0.0f, 1.0f, (x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

void ReductionCurve::prepare (const FrameConfig& config)
{
    cfg = config;
    const auto nb = (size_t) cfg.numBins;

    loIdx.assign (nb, 0);
    hiIdx.assign (nb, 0);
    attackCoef.assign (nb, 1.0f);
    releaseCoef.assign (nb, 1.0f);
    freqWeight.assign (nb, 1.0f);
    specDb.assign (nb, 0.0f);
    clipped.assign (nb, 0.0f);
    target.assign (nb, 0.0f);
    smoothed.assign (nb, 0.0f);
    reduction.assign (nb, 0.0f);
    prefix.assign (nb + 1, 0.0);

    for (size_t k = 0; k < nb; ++k)
        freqWeight[k] = smoothstep (kLowCutStartHz, kLowCutEndHz, (float) k * cfg.binHz());

    rebuildEnvelopeTables();
    rebuildBallisticsTables();
}

void ReductionCurve::reset()
{
    std::fill (reduction.begin(), reduction.end(), 0.0f);
}

void ReductionCurve::setParams (const ReductionParams& p)
{
    const auto detailChanged = ! juce::exactlyEqual (p.detail01, params.detail01);
    const auto timeChanged = ! juce::exactlyEqual (p.attack01, params.attack01) || ! juce::exactlyEqual (p.release01, params.release01);
    params = p;

    if (detailChanged)
        rebuildEnvelopeTables();
    if (timeChanged)
        rebuildBallisticsTables();
}

void ReductionCurve::rebuildEnvelopeTables()
{
    const auto widthOct = kWidestEnvelopeOct * std::pow (kNarrowestEnvelopeOct / kWidestEnvelopeOct, params.detail01);
    const auto ratio = std::pow (2.0f, widthOct * 0.5f);
    const int last = cfg.numBins - 1;

    for (int k = 0; k <= last; ++k)
    {
        auto lo = (int) std::floor ((float) k / ratio);
        auto hi = (int) std::ceil ((float) k * ratio);
        lo = std::min (lo, k - 4);
        hi = std::max (hi, k + 4);
        loIdx[(size_t) k] = std::max (lo, 0);
        hiIdx[(size_t) k] = std::min (hi, last);
    }
}

void ReductionCurve::rebuildBallisticsTables()
{
    // Time constants at 1 kHz: attack 3..150 ms, release 20..800 ms
    const auto attackMs = 3.0f * std::pow (50.0f, params.attack01);
    const auto releaseMs = 20.0f * std::pow (40.0f, params.release01);
    const auto hopSeconds = (float) cfg.hopSize / (float) cfg.sampleRate;

    for (int k = 0; k < cfg.numBins; ++k)
    {
        const auto f = std::max ((float) k * cfg.binHz(), 20.0f);
        const auto freqScale = juce::jlimit (0.35f, 2.5f, std::pow (1000.0f / f, 0.4f));
        const auto a = attackMs * 0.001f * freqScale;
        const auto r = releaseMs * 0.001f * freqScale;
        attackCoef[(size_t) k] = 1.0f - std::exp (-hopSeconds / a);
        releaseCoef[(size_t) k] = 1.0f - std::exp (-hopSeconds / r);
    }
}

void ReductionCurve::process (const float* power) noexcept
{
    const int nb = cfg.numBins;

    for (int k = 0; k < nb; ++k)
        specDb[(size_t) k] = 10.0f * std::log10 (power[k] + 1.0e-13f);

    // Two envelope passes: the second ignores the peaks the first one found, so a resonance doesn't pull
    // up its own reference
    prefix[0] = 0.0;
    for (int k = 0; k < nb; ++k)
        prefix[(size_t) k + 1] = prefix[(size_t) k] + (double) specDb[(size_t) k];

    for (int k = 0; k < nb; ++k)
    {
        const auto lo = loIdx[(size_t) k];
        const auto hi = hiIdx[(size_t) k];
        const auto env = (float) ((prefix[(size_t) hi + 1] - prefix[(size_t) lo]) / (double) (hi - lo + 1));
        clipped[(size_t) k] = std::min (specDb[(size_t) k], env + kPeakClipDb);
    }

    for (int k = 0; k < nb; ++k)
        prefix[(size_t) k + 1] = prefix[(size_t) k] + (double) clipped[(size_t) k];

    const auto depthScale = params.depth01 * (params.hard ? kHardDepthScale : kSoftDepthScale);

    for (int k = 0; k < nb; ++k)
    {
        const auto lo = loIdx[(size_t) k];
        const auto hi = hiIdx[(size_t) k];
        const auto env = (float) ((prefix[(size_t) hi + 1] - prefix[(size_t) lo]) / (double) (hi - lo + 1));
        const auto level = specDb[(size_t) k];
        const auto excess = level - env - kKneeDb;

        auto t = 0.0f;
        if (excess > 0.0f)
        {
            const auto gate = smoothstep (kFloorDb, kFullGateDb, level);
            const auto levelFactor = params.hard ? juce::jlimit (0.0f, 2.0f, (level - kHardLevelRefDb) / kHardLevelRangeDb) : 1.0f;
            t = std::min (excess * depthScale * levelFactor * gate * freqWeight[(size_t) k], params.maxCutDb);
        }
        target[(size_t) k] = t;
    }

    // A little smoothing across frequency keeps the resulting filter well behaved
    for (int k = 0; k < nb; ++k)
    {
        const auto prev = target[(size_t) std::max (k - 1, 0)];
        const auto next = target[(size_t) std::min (k + 1, nb - 1)];
        smoothed[(size_t) k] = 0.25f * prev + 0.5f * target[(size_t) k] + 0.25f * next;
    }

    for (int k = 0; k < nb; ++k)
    {
        auto& r = reduction[(size_t) k];
        const auto goal = smoothed[(size_t) k];
        r += (goal - r) * (goal > r ? attackCoef[(size_t) k] : releaseCoef[(size_t) k]);
        r = std::min (r, params.maxCutDb);
    }
}
} // namespace vs
