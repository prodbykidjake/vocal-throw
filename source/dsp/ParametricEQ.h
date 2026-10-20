#pragma once

#include "Biquad.h"
#include <array>
#include <juce_audio_basics/juce_audio_basics.h>

struct EqParams
{
    struct Band
    {
        float freq, gainDb, q;
    };

    bool hpEnabled = false;
    float hpFreq = 80.0f;
    // 0: low shelf, 1: bell, 2: bell, 3: high shelf
    std::array<Band, 4> bands { { { 100.0f, 0.0f, 1.0f }, { 300.0f, 0.0f, 1.0f }, { 3000.0f, 0.0f, 1.0f }, { 8000.0f, 0.0f, 1.0f } } };
};

// High-pass (12 dB/oct Butterworth) followed by four fixed-type bands.
// Parameters are smoothed and coefficients re-derived every 32 samples only while something moves.
class ParametricEQ
{
public:
    static constexpr int maxChannels = 2;
    static constexpr int subBlock = 32;
    static constexpr double hpQ = 0.70710678;

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        for (int i = 0; i < 4; ++i)
        {
            freq[(size_t) i].reset (sampleRate, 0.03);
            gain[(size_t) i].reset (sampleRate, 0.03);
            q[(size_t) i].reset (sampleRate, 0.03);
        }
        hpFreqSmoothed.reset (sampleRate, 0.03);
        setParams (params, true);
        reset();
    }

    void reset()
    {
        for (auto& ch : state)
        {
            ch.hp.reset();
            for (auto& s : ch.bands)
                s.reset();
        }
    }

    void setParams (const EqParams& p, bool immediately = false)
    {
        params = p;
        for (size_t i = 0; i < 4; ++i)
        {
            if (immediately)
            {
                freq[i].setCurrentAndTargetValue (p.bands[i].freq);
                gain[i].setCurrentAndTargetValue (p.bands[i].gainDb);
                q[i].setCurrentAndTargetValue (p.bands[i].q);
            }
            else
            {
                freq[i].setTargetValue (p.bands[i].freq);
                gain[i].setTargetValue (p.bands[i].gainDb);
                q[i].setTargetValue (p.bands[i].q);
            }
        }
        if (immediately)
            hpFreqSmoothed.setCurrentAndTargetValue (p.hpFreq);
        else
            hpFreqSmoothed.setTargetValue (p.hpFreq);
        updateCoeffs();
    }

    void process (juce::AudioBuffer<float>& buffer, int numChannels, int start, int numSamples)
    {
        numChannels = std::min (numChannels, maxChannels);
        for (int pos = 0; pos < numSamples; pos += subBlock)
        {
            const int n = std::min (subBlock, numSamples - pos);
            if (anySmoothing())
            {
                for (size_t i = 0; i < 4; ++i)
                {
                    freq[i].skip (n);
                    gain[i].skip (n);
                    q[i].skip (n);
                }
                hpFreqSmoothed.skip (n);
                updateCoeffs();
            }

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* data = buffer.getWritePointer (ch, start + pos);
                auto& st = state[(size_t) ch];
                for (int i = 0; i < n; ++i)
                {
                    float x = data[i];
                    if (params.hpEnabled)
                        x = st.hp.process (hpCoeffs, x);
                    for (size_t b = 0; b < 4; ++b)
                        if (bandActive[b])
                            x = st.bands[b].process (coeffs[b], x);
                    data[i] = x;
                }
            }
        }
    }

    // Magnitude response in dB for the given parameters (used by tests and the UI curve).
    static double responseDb (const EqParams& p, double f, double fs)
    {
        double db = 0.0;
        if (p.hpEnabled)
            db += BiquadCoeffs::highPass (fs, p.hpFreq, hpQ).magnitudeDb (f, fs);
        for (size_t b = 0; b < 4; ++b)
            if (std::abs (p.bands[b].gainDb) >= minActiveGainDb)
                db += makeBand (b, fs, p.bands[b].freq, p.bands[b].gainDb, p.bands[b].q).magnitudeDb (f, fs);
        return db;
    }

private:
    static constexpr float minActiveGainDb = 0.005f;

    static BiquadCoeffs makeBand (size_t band, double fs, double f, double g, double qv)
    {
        switch (band)
        {
            case 0: return BiquadCoeffs::lowShelf (fs, f, g, qv);
            case 3: return BiquadCoeffs::highShelf (fs, f, g, qv);
            default: return BiquadCoeffs::peak (fs, f, g, qv);
        }
    }

    bool anySmoothing() const
    {
        if (hpFreqSmoothed.isSmoothing())
            return true;
        for (size_t i = 0; i < 4; ++i)
            if (freq[i].isSmoothing() || gain[i].isSmoothing() || q[i].isSmoothing())
                return true;
        return false;
    }

    void updateCoeffs()
    {
        hpCoeffs = BiquadCoeffs::highPass (sampleRate, hpFreqSmoothed.getCurrentValue(), hpQ);
        for (size_t b = 0; b < 4; ++b)
        {
            const auto g = gain[b].getCurrentValue();
            // A band whose gain is (and is heading to) ~0 dB is skipped entirely.
            bandActive[b] = std::abs (g) >= minActiveGainDb || std::abs (gain[b].getTargetValue()) >= minActiveGainDb;
            if (bandActive[b])
                coeffs[b] = makeBand (b, sampleRate, freq[b].getCurrentValue(), g, q[b].getCurrentValue());
        }
    }

    struct ChannelState
    {
        BiquadState hp;
        std::array<BiquadState, 4> bands;
    };

    double sampleRate = 44100.0;
    EqParams params;
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative>, 4> freq, q;
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>, 4> gain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> hpFreqSmoothed;
    std::array<BiquadCoeffs, 4> coeffs;
    std::array<bool, 4> bandActive {};
    BiquadCoeffs hpCoeffs;
    std::array<ChannelState, maxChannels> state;
};
