#pragma once

#include "../dsp/Compressor.h"
#include "../dsp/GainStage.h"
#include "../dsp/ParametricEQ.h"
#include "../dsp/Saturator.h"

struct ChainParams
{
    float inputGainDb = 0.0f, outputGainDb = 0.0f;
    bool eqEnabled = true, compEnabled = true, satEnabled = true;
    EqParams eq;
    CompressorParams comp;
    SaturatorParams sat;
};

// Input Gain -> EQ -> Compressor -> Saturation -> Output Gain.
// Each module can be enabled/disabled click-free (10 ms crossfade); a settled-off module costs nothing.
// The chain has no look-ahead and no oversampling, so it adds zero latency.
class Chain
{
public:
    void prepare (double newSampleRate, int maxBlockSize)
    {
        sampleRate = newSampleRate;
        scratch.setSize (2, std::max (1, maxBlockSize), false, false, true);
        inputGain.prepare (sampleRate);
        outputGain.prepare (sampleRate);
        eq.prepare (sampleRate);
        comp.prepare (sampleRate);
        sat.prepare (sampleRate);
        for (auto* m : { &eqMix, &compMix, &satMix })
            m->reset (sampleRate, 0.01);
        eqMix.setCurrentAndTargetValue (params.eqEnabled ? 1.0f : 0.0f);
        compMix.setCurrentAndTargetValue (params.compEnabled ? 1.0f : 0.0f);
        satMix.setCurrentAndTargetValue (params.satEnabled ? 1.0f : 0.0f);
    }

    void setParams (const ChainParams& p)
    {
        params = p;
        inputGain.setGainDb (p.inputGainDb);
        outputGain.setGainDb (p.outputGainDb);
        eq.setParams (p.eq);
        comp.setParams (p.comp);
        sat.setParams (p.sat);
    }

    // Jump straight to the given parameters without smoothing (used right after prepare()).
    void snapToParams (const ChainParams& p)
    {
        params = p;
        inputGain.setGainDb (p.inputGainDb);
        outputGain.setGainDb (p.outputGainDb);
        inputGain.prepare (sampleRate);
        outputGain.prepare (sampleRate);
        eq.setParams (p.eq, true);
        comp.setParams (p.comp, true);
        sat.setParams (p.sat, true);
        eqMix.setCurrentAndTargetValue (p.eqEnabled ? 1.0f : 0.0f);
        compMix.setCurrentAndTargetValue (p.compEnabled ? 1.0f : 0.0f);
        satMix.setCurrentAndTargetValue (p.satEnabled ? 1.0f : 0.0f);
    }

    void reset()
    {
        eq.reset();
        comp.reset();
    }

    // The chain is exposed in three stages so the processor can measure loudness between them
    // (auto level match compares "after input gain" with "after the modules"). numSamples must not exceed
    // the maxBlockSize given to prepare().
    void processInput (juce::AudioBuffer<float>& buffer, int numChannels, int start, int numSamples)
    {
        inputGain.process (buffer, numChannels, start, numSamples);
    }

    void processModules (juce::AudioBuffer<float>& buffer, int numChannels, int start, int numSamples)
    {
        blended (eqMix, params.eqEnabled, buffer, numChannels, start, numSamples,
                 [this] (auto& b, int c, int s, int n) { eq.process (b, c, s, n); }, [this] { eq.reset(); });
        blended (compMix, params.compEnabled, buffer, numChannels, start, numSamples,
                 [this] (auto& b, int c, int s, int n) { comp.process (b, c, s, n); }, [this] { comp.reset(); });
        blended (satMix, params.satEnabled, buffer, numChannels, start, numSamples,
                 [this] (auto& b, int c, int s, int n) { sat.process (b, c, s, n); }, [] {});
    }

    void processOutput (juce::AudioBuffer<float>& buffer, int numChannels, int start, int numSamples)
    {
        outputGain.process (buffer, numChannels, start, numSamples);
    }

    void process (juce::AudioBuffer<float>& buffer, int numChannels, int start, int numSamples)
    {
        processInput (buffer, numChannels, start, numSamples);
        processModules (buffer, numChannels, start, numSamples);
        processOutput (buffer, numChannels, start, numSamples);
    }

    float getGainReductionDb() const { return params.compEnabled ? comp.getGainReductionDb() : 0.0f; }

private:
    using Mix = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    template <typename Process, typename OnOff>
    void blended (Mix& mix, bool enabled, juce::AudioBuffer<float>& buffer, int numChannels, int start, int n,
                  Process&& process, OnOff&& onSettledOff)
    {
        mix.setTargetValue (enabled ? 1.0f : 0.0f);
        const bool moving = mix.isSmoothing();
        if (! moving && juce::exactlyEqual (mix.getTargetValue(), 0.0f))
        {
            onSettledOff(); // keep dormant module state clean for when it's re-enabled
            return;
        }
        if (! moving)
        {
            process (buffer, numChannels, start, n);
            return;
        }

        for (int ch = 0; ch < numChannels; ++ch)
            scratch.copyFrom (ch, 0, buffer, ch, start, n);
        process (buffer, numChannels, start, n);
        for (int i = 0; i < n; ++i)
        {
            const auto w = mix.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* out = buffer.getWritePointer (ch, start);
                const auto dry = scratch.getReadPointer (ch)[i];
                out[i] = dry + w * (out[i] - dry);
            }
        }
    }

    double sampleRate = 44100.0;
    ChainParams params;
    GainStage inputGain, outputGain;
    ParametricEQ eq;
    Compressor comp;
    Saturator sat;
    Mix eqMix { 1.0f }, compMix { 1.0f }, satMix { 1.0f };
    juce::AudioBuffer<float> scratch;
};
