#include "DepthCurve.h"
#include <algorithm>
#include <cmath>
#include <juce_core/juce_core.h>

namespace vs
{
bool BandSettings::sameAs (const BandSettings& o) const noexcept
{
    return enabled == o.enabled && shape == o.shape && juce::exactlyEqual (freqHz, o.freqHz)
        && juce::exactlyEqual (gainDb, o.gainDb) && juce::exactlyEqual (q, o.q) && juce::exactlyEqual (balance, o.balance);
}

void DepthCurve::prepare (const FrameConfig& config)
{
    cfg = config;
    for (auto& w : weights)
        w.assign ((size_t) cfg.numBins, 1.0f);
    haveBands = false;
}

void DepthCurve::setBands (const BandArray& newBands) noexcept
{
    if (weights[0].empty())
        return;

    bool changed = ! haveBands;
    for (size_t i = 0; i < bands.size() && ! changed; ++i)
        changed = ! bands[i].sameAs (newBands[i]);

    if (! changed)
        return;

    bands = newBands;
    haveBands = true;
    recompute();
}

void DepthCurve::recompute() noexcept
{
    for (int k = 0; k < cfg.numBins; ++k)
        for (int c = 0; c < 2; ++c)
            weights[(size_t) c][(size_t) k] = weightAt (bands, (float) k * cfg.binHz(), c);
}

float DepthCurve::bandMagnitude (const BandSettings& band, float hz) noexcept
{
    constexpr double pi = 3.14159265358979323846;
    const auto fc = (double) juce::jlimit (10.0f, 24000.0f, band.freqHz);
    const auto x = std::max ((double) hz, 1.0) / fc;
    const auto gain = (double) band.gainDb;
    const auto q = (double) juce::jlimit (0.3f, 10.0f, band.q);
    const auto order = (double) juce::jlimit (0.5f, 8.0f, band.q);
    const auto fromDb = [] (double db) { return std::pow (10.0, db / 20.0); };
    const auto d = x - 1.0 / x;

    double m = 1.0;
    switch (band.shape)
    {
        case BandShape::lowCut:    m = 1.0 / std::sqrt (1.0 + std::pow (1.0 / x, 2.0 * order)); break;
        case BandShape::highCut:   m = 1.0 / std::sqrt (1.0 + std::pow (x, 2.0 * order)); break;
        case BandShape::lowShelf:  m = fromDb (gain / (1.0 + std::pow (x, 2.0 * q))); break;
        case BandShape::highShelf: m = fromDb (gain / (1.0 + std::pow (1.0 / x, 2.0 * q))); break;
        case BandShape::bell:      m = fromDb (gain / (1.0 + q * q * d * d)); break;
        case BandShape::tilt:      m = fromDb (gain * (2.0 / pi) * std::atan (std::log2 (x) * q)); break;
        case BandShape::bandpass:  m = fromDb (gain) / std::sqrt (1.0 + q * q * d * d); break;
        case BandShape::bandReject: m = std::sqrt ((q * q * d * d) / (1.0 + q * q * d * d)); break;
    }
    return (float) m;
}

float DepthCurve::weightAt (const BandArray& bands, float hz, int channel) noexcept
{
    auto parallel = 0.0f;
    auto anyBandpass = false;
    auto product = 1.0f;

    for (const auto& b : bands)
    {
        if (! b.enabled)
            continue;

        const auto balance = juce::jlimit (-1.0f, 1.0f, b.balance);
        const auto factor = channel == 0 ? 1.0f - std::max (0.0f, balance) : 1.0f - std::max (0.0f, -balance);
        const auto m = bandMagnitude (b, hz);
        if (b.shape == BandShape::bandpass)
        {
            anyBandpass = true;
            parallel = std::max (parallel, m * factor);
        }
        else
        {
            product *= juce::exactlyEqual (factor, 1.0f) ? m : std::pow (m, factor);
        }
    }

    return juce::jlimit (0.0f, maxWeight, (anyBandpass ? parallel : 1.0f) * product);
}
} // namespace vs
