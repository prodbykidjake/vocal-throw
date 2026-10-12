#include "ResonanceSuppressor.h"

namespace vs
{
static constexpr double kSmoothingSeconds = 0.02;
static constexpr float kDisplayMinHz = 20.0f;
static constexpr float kDisplayMaxHz = 20000.0f;

float ResonanceSuppressor::displayPointFrequency (int index) noexcept
{
    const auto t = (float) index / (float) (numDisplayPoints - 1);
    return kDisplayMinHz * std::pow (kDisplayMaxHz / kDisplayMinHz, t);
}

void ResonanceSuppressor::prepare (double sampleRate, int numChannels)
{
    cfg = FrameConfig::forSampleRate (sampleRate);
    activeChannels = juce::jlimit (1, maxChannels, numChannels);

    for (int c = 0; c < maxChannels; ++c)
    {
        analyzers[(size_t) c].prepare (cfg);
        curves[(size_t) c].prepare (cfg);
        filters[(size_t) c].prepare (cfg);
    }

    linkedPower.assign ((size_t) cfg.numBins, 0.0f);

    const auto delaySize = juce::nextPowerOfTwo (cfg.latency + 2);
    delayMask = delaySize - 1;
    for (auto& d : delayBuf)
        d.assign ((size_t) delaySize, 0.0f);

    // Each display point covers the bins between the geometric midpoints to its neighbours
    for (int i = 0; i < numDisplayPoints; ++i)
    {
        const auto lo = i == 0 ? displayPointFrequency (0) : std::sqrt (displayPointFrequency (i - 1) * displayPointFrequency (i));
        const auto hi = i == numDisplayPoints - 1 ? displayPointFrequency (i) : std::sqrt (displayPointFrequency (i) * displayPointFrequency (i + 1));
        const auto loBin = juce::jlimit (0, cfg.numBins - 1, (int) std::floor (lo / cfg.binHz()));
        const auto hiBin = juce::jlimit (loBin, cfg.numBins - 1, (int) std::ceil (hi / cfg.binHz()));
        displayLo[(size_t) i] = loBin;
        displayHi[(size_t) i] = hiBin;
    }

    mixSmoothed.reset (sampleRate, kSmoothingSeconds);
    trimSmoothed.reset (sampleRate, kSmoothingSeconds);
    outGainSmoothed.reset (sampleRate, kSmoothingSeconds);
    bypassSmoothed.reset (sampleRate, kSmoothingSeconds);

    prepared = true;
    setParams (params);
    mixSmoothed.setCurrentAndTargetValue (mixSmoothed.getTargetValue());
    trimSmoothed.setCurrentAndTargetValue (trimSmoothed.getTargetValue());
    outGainSmoothed.setCurrentAndTargetValue (outGainSmoothed.getTargetValue());
    bypassSmoothed.setCurrentAndTargetValue (bypassSmoothed.getTargetValue());
    reset();
}

void ResonanceSuppressor::reset()
{
    for (int c = 0; c < maxChannels; ++c)
    {
        analyzers[(size_t) c].reset();
        curves[(size_t) c].reset();
        filters[(size_t) c].reset();
        std::fill (delayBuf[(size_t) c].begin(), delayBuf[(size_t) c].end(), 0.0f);
    }

    delayWrite = 0;
    hopCounter = 0;

    for (auto& d : display)
        d.store (0.0f, std::memory_order_relaxed);
}

void ResonanceSuppressor::setParams (const SuppressorParams& p) noexcept
{
    params = p;
    if (! prepared)
        return;

    ReductionParams rp;
    rp.hard = p.hard;
    rp.depth01 = juce::jlimit (0.0f, 1.0f, p.depth * 0.01f);
    rp.detail01 = juce::jlimit (0.0f, 1.0f, p.detail * 0.01f);
    rp.attack01 = juce::jlimit (0.0f, 1.0f, p.attack * 0.01f);
    rp.release01 = juce::jlimit (0.0f, 1.0f, p.release * 0.01f);
    rp.maxCutDb = p.maxCutDb;
    for (auto& c : curves)
        c.setParams (rp);

    mixSmoothed.setTargetValue (juce::jlimit (0.0f, 1.0f, p.mix * 0.01f));
    trimSmoothed.setTargetValue (juce::Decibels::decibelsToGain (p.wetTrimDb));
    outGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (p.outGainDb));
    bypassSmoothed.setTargetValue (p.bypass ? 1.0f : 0.0f);
}

void ResonanceSuppressor::process (juce::AudioBuffer<float>& buffer) noexcept
{
    if (! prepared)
        return;

    const int numCh = juce::jmin (buffer.getNumChannels(), activeChannels);
    const int numSamples = buffer.getNumSamples();
    const bool midSide = params.midSide && numCh == 2;
    const bool delta = params.delta;

    float* data[maxChannels] = {};
    for (int c = 0; c < numCh; ++c)
        data[c] = buffer.getWritePointer (c);

    for (int i = 0; i < numSamples; ++i)
    {
        float in[maxChannels] = {};
        for (int c = 0; c < numCh; ++c)
            in[c] = data[c][i];

        if (midSide)
        {
            const auto m = 0.5f * (in[0] + in[1]);
            const auto s = 0.5f * (in[0] - in[1]);
            in[0] = m;
            in[1] = s;
        }

        float dry[maxChannels] = {};
        float wet[maxChannels] = {};
        for (int c = 0; c < numCh; ++c)
        {
            analyzers[(size_t) c].pushSample (in[c]);

            auto& d = delayBuf[(size_t) c];
            d[(size_t) delayWrite] = in[c];
            dry[c] = d[(size_t) ((delayWrite - cfg.latency) & delayMask)];
            wet[c] = filters[(size_t) c].process (dry[c]);
        }
        delayWrite = (delayWrite + 1) & delayMask;

        if (++hopCounter >= cfg.hopSize)
        {
            hopCounter = 0;
            runAnalysisHop();
        }

        const auto mix = mixSmoothed.getNextValue();
        const auto trim = trimSmoothed.getNextValue();
        const auto gain = outGainSmoothed.getNextValue();
        const auto byp = bypassSmoothed.getNextValue();

        float proc[maxChannels] = {};
        for (int c = 0; c < numCh; ++c)
        {
            const auto w = wet[c] * trim;
            proc[c] = (delta ? mix * (dry[c] - w) : dry[c] + mix * (w - dry[c])) * gain;
        }

        if (midSide)
        {
            const auto l = proc[0] + proc[1];
            const auto r = proc[0] - proc[1];
            proc[0] = l;
            proc[1] = r;
            const auto dl = dry[0] + dry[1];
            const auto dr = dry[0] - dry[1];
            dry[0] = dl;
            dry[1] = dr;
        }

        for (int c = 0; c < numCh; ++c)
            data[c][i] = byp <= 0.0f ? proc[c] : (byp >= 1.0f ? dry[c] : proc[c] + byp * (dry[c] - proc[c]));
    }
}

void ResonanceSuppressor::runAnalysisHop() noexcept
{
    const int numCh = activeChannels;
    for (int c = 0; c < numCh; ++c)
        analyzers[(size_t) c].analyse();

    const auto link = numCh == 2 ? juce::jlimit (0.0f, 1.0f, params.link * 0.01f) : 0.0f;

    for (int c = 0; c < numCh; ++c)
    {
        const float* power = analyzers[(size_t) c].getPower();

        if (link > 0.0f)
        {
            const float* p0 = analyzers[0].getPower();
            const float* p1 = analyzers[1].getPower();
            for (int k = 0; k < cfg.numBins; ++k)
                linkedPower[(size_t) k] = (1.0f - link) * power[k] + link * 0.5f * (p0[k] + p1[k]);
            power = linkedPower.data();
        }

        curves[(size_t) c].process (power);
        filters[(size_t) c].setCurve (curves[(size_t) c].getReductionDb());
    }

    for (int i = 0; i < numDisplayPoints; ++i)
    {
        float peak = 0.0f;
        for (int c = 0; c < numCh; ++c)
        {
            const float* r = curves[(size_t) c].getReductionDb();
            for (int k = displayLo[(size_t) i]; k <= displayHi[(size_t) i]; ++k)
                peak = std::max (peak, r[k]);
        }
        display[(size_t) i].store (peak, std::memory_order_relaxed);
    }
}

void ResonanceSuppressor::getReductionDisplay (float* dest) const noexcept
{
    for (int i = 0; i < numDisplayPoints; ++i)
        dest[i] = display[(size_t) i].load (std::memory_order_relaxed);
}
} // namespace vs
