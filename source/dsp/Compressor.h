#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

struct CompressorParams
{
    float thresholdDb = -18.0f;
    float ratio = 2.0f;
    float attackMs = 10.0f;
    float releaseMs = 120.0f;
    float kneeDb = 6.0f;
    float makeupDb = 0.0f;
    float mix = 1.0f; // 0..1 parallel blend
};

// Feed-forward compressor: stereo-linked peak detector (instant attack, 14 ms hold, then a fast 2 ms fall) ->
// soft-knee gain computer -> attack/release smoothing of the gain reduction in the dB domain.
// The hold matters: a detector that follows the raw rectified waveform makes the gain wobble at twice the note
// frequency, which shows up as distortion on low notes (measured: -42 dB THD at 60 Hz with 10/100 ms, -19 dB with
// 0.1/5 ms). Holding across the waveform's dips keeps the gain steady on sustained notes without slowing attacks.
// Attack/release are one-pole time constants (the time to cover ~63% of a step).
// Zero latency: there is no look-ahead.
class Compressor
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        holdSamples = std::max (1, (int) std::lround (0.014 * sampleRate));
        detectorFall = (float) std::exp (-1.0 / (0.002 * sampleRate));
        makeup.reset (sampleRate, 0.02);
        mix.reset (sampleRate, 0.02);
        setParams (params, true);
        reset();
    }

    void reset()
    {
        smoothedReductionDb = 0.0f;
        detector = 0.0f;
        holdCounter = 0;
        grMeter.store (0.0f, std::memory_order_relaxed);
    }

    void setParams (const CompressorParams& p, bool immediately = false)
    {
        params = p;
        params.ratio = std::max (1.0f, p.ratio);
        params.kneeDb = std::max (0.0f, p.kneeDb);
        attackCoeff = timeToCoeff (p.attackMs);
        releaseCoeff = timeToCoeff (p.releaseMs);
        if (immediately)
        {
            makeup.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (p.makeupDb));
            mix.setCurrentAndTargetValue (p.mix);
        }
        else
        {
            makeup.setTargetValue (juce::Decibels::decibelsToGain (p.makeupDb));
            mix.setTargetValue (p.mix);
        }
    }

    // Static curve: gain reduction in dB (>= 0) for a given input level in dB.
    static float reductionDb (float levelDb, float thresholdDb, float ratio, float kneeDb)
    {
        const auto over = levelDb - thresholdDb;
        const auto slope = 1.0f / ratio - 1.0f;
        if (kneeDb > 0.0f && 2.0f * over > -kneeDb && 2.0f * over < kneeDb)
        {
            const auto t = over + kneeDb * 0.5f;
            return -slope * t * t / (2.0f * kneeDb);
        }
        return over > 0.0f ? -slope * over : 0.0f;
    }

    void process (juce::AudioBuffer<float>& buffer, int numChannels, int start, int numSamples)
    {
        float maxReduction = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            float level = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                level = std::max (level, std::abs (buffer.getReadPointer (ch, start)[i]));

            // peak detector: instant attack, hold across waveform dips, then fall quickly
            if (level >= detector)
            {
                detector = level;
                holdCounter = holdSamples;
            }
            else if (level >= 0.89f * detector) // a peak within ~1 dB of the held one keeps the hold alive (steady tones)
                holdCounter = holdSamples;
            else if (holdCounter > 0)
                --holdCounter;
            else
                detector = level + (detector - level) * detectorFall;

            const auto levelDb = juce::Decibels::gainToDecibels (detector, -120.0f);
            const auto target = reductionDb (levelDb, params.thresholdDb, params.ratio, params.kneeDb);
            const auto coeff = target > smoothedReductionDb ? attackCoeff : releaseCoeff;
            smoothedReductionDb = target + coeff * (smoothedReductionDb - target);
            maxReduction = std::max (maxReduction, smoothedReductionDb);

            const auto wetGain = std::pow (10.0f, -smoothedReductionDb * 0.05f) * makeup.getNextValue();
            const auto m = mix.getNextValue();
            const auto g = (1.0f - m) + m * wetGain;
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch, start)[i] *= g;
        }
        grMeter.store (maxReduction, std::memory_order_relaxed);
    }

    // Largest gain reduction (dB, positive) over the most recent block. Safe to read from any thread.
    float getGainReductionDb() const { return grMeter.load (std::memory_order_relaxed); }

private:
    float timeToCoeff (float ms) const
    {
        const auto samples = std::max (1.0e-3, (double) ms * 0.001 * sampleRate);
        return (float) std::exp (-1.0 / samples);
    }

    double sampleRate = 44100.0;
    CompressorParams params;
    float attackCoeff = 0.0f, releaseCoeff = 0.0f;
    float smoothedReductionDb = 0.0f;
    float detector = 0.0f, detectorFall = 0.0f;
    int holdCounter = 0, holdSamples = 1;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> makeup { 1.0f }, mix { 1.0f };
    std::atomic<float> grMeter { 0.0f };
};
