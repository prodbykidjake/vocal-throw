#pragma once

#include <array>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>
#include <numbers>

struct SaturatorParams
{
    float driveDb = 3.0f;
    float mix = 1.0f;
};

// Symmetric tanh soft clipper with first-order antiderivative anti-aliasing (ADAA): instead of
// tanh(u[n]) it outputs (F(u[n]) - F(u[n-1])) / (u[n] - u[n-1]) with F = ln(cosh), which is the
// exact average of tanh over the step between samples. That cuts aliasing substantially at no
// latency cost (it behaves like a half-sample delay plus a mild high-frequency roll-off).
// The wet path is gain-compensated so that a sine at the reference level (-12 dBFS peak) keeps
// the same RMS at every drive setting, which stops the level jumping when drive is automated.
// Measured aliasing is reported in tests/DspDynamics.cpp.
class Saturator
{
public:
    static constexpr float referencePeak = 0.25f;

    void prepare (double sampleRate)
    {
        drive.reset (sampleRate, 0.02);
        mix.reset (sampleRate, 0.02);
        lastDriveDb = -1000.0f;
        setParams (pending, true);
    }

    void setParams (const SaturatorParams& p, bool immediately = false)
    {
        pending = p;
        if (! juce::exactlyEqual (p.driveDb, lastDriveDb)) // compensation is only recomputed when drive changes
        {
            lastDriveDb = p.driveDb;
            const auto k = juce::Decibels::decibelsToGain (p.driveDb);
            const auto c = compensation (k);
            if (immediately)
            {
                drive.setCurrentAndTargetValue (k);
                comp.setCurrentAndTargetValue (c);
            }
            else
            {
                drive.setTargetValue (k);
                comp.setTargetValue (c);
            }
        }
        if (immediately)
            mix.setCurrentAndTargetValue (p.mix);
        else
            mix.setTargetValue (p.mix);
    }

    // Wet gain that preserves the RMS of a reference sine through tanh(k * x).
    static float compensation (float k)
    {
        constexpr int n = 128;
        double ms = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const auto y = std::tanh ((double) k * referencePeak * std::sin (2.0 * std::numbers::pi * i / n));
            ms += y * y;
        }
        ms /= n;
        const auto refRms = referencePeak / std::numbers::sqrt2;
        return (float) (refRms / std::sqrt (std::max (ms, 1.0e-12)));
    }

    void process (juce::AudioBuffer<float>& buffer, int numChannels, int start, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const auto k = drive.getNextValue();
            const auto c = comp.getNextValue();
            const auto m = mix.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto& x = buffer.getWritePointer (ch, start)[i];
                const auto u = k * x;
                auto& prev = state[(size_t) ch];
                const auto wet = c * adaaTanh (u, prev.u, prev.f);
                prev = { u, logCosh (u) };
                x += m * (wet - x);
            }
        }
    }

private:
    // ln(cosh(x)) without overflow for large |x|.
    static float logCosh (float x)
    {
        const auto a = std::abs (x);
        return a + std::log1p (std::exp (-2.0f * a)) - 0.693147181f;
    }

    static float adaaTanh (float u, float uPrev, float fPrev)
    {
        const auto du = u - uPrev;
        if (std::abs (du) < 1.0e-4f)
            return std::tanh (0.5f * (u + uPrev));
        return (logCosh (u) - fPrev) / du;
    }

    struct ChannelState
    {
        float u = 0.0f, f = 0.0f;
    };
    std::array<ChannelState, 2> state;

    float lastDriveDb = -1000.0f;
    SaturatorParams pending;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> drive { 1.0f }, comp { 1.0f }, mix { 1.0f };
};
