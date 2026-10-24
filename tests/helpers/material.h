#pragma once

#include "dsp_test_utils.h"
#include <dsp/Biquad.h>
#include <numbers>

// Realistic-ish synthetic test material (no recordings are checked in): a sung phrase with moving formants, vibrato,
// phrase dynamics and breath noise, and a drum loop. Used by the audio lab and the loudness-neutrality tests.
namespace test
{
namespace material
{
inline constexpr double fs = 48000.0;
// Synthetic sung phrase: glottal-ish pulse train through moving formants, vibrato, syllable envelope, breath noise.
inline juce::AudioBuffer<float> voice (int seconds, unsigned seed, float rmsDb)
{
    juce::Random rng ((juce::int64) seed);
    const int n = (int) fs * seconds;
    juce::AudioBuffer<float> b (2, n);
    struct Vowel { double f1, f2, f3; };
    const Vowel vowels[] = { { 700, 1100, 2600 }, { 450, 1900, 2600 }, { 300, 2300, 3000 }, { 500, 900, 2500 }, { 320, 800, 2400 } };
    BiquadState st[3];
    double phase = 0.0, vib = 0.0;
    float hp = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs;
        const double syl = std::fmod (t * 2.2, 1.0);                 // ~2.2 syllables/s
        const int idx = (int) (t * 2.2) % 5, nxt = (idx + 1) % 5;
        const double mix = std::clamp ((syl - 0.7) / 0.3, 0.0, 1.0);
        const auto v1 = vowels[idx], v2 = vowels[nxt];
        const double f0 = (150.0 + 40.0 * std::sin (t * 0.9) + 25.0 * (idx % 3)) * (1.0 + 0.012 * std::sin (2.0 * std::numbers::pi * 5.5 * t));
        phase += f0 / fs;
        if (phase >= 1.0) phase -= 1.0;
        const double glottal = (phase < 0.6 ? phase / 0.6 : (1.0 - phase) / 0.4) - 0.5; // asymmetric triangle: rich in harmonics
        const double env = (syl < 0.08 ? syl / 0.08 : 1.0) * (0.45 + 0.55 * std::pow (std::sin (std::numbers::pi * std::min (1.0, syl / 0.92)), 0.6))
                           * std::pow (10.0, (std::sin (t * 0.35) * 6.0) / 20.0);    // +-6 dB phrase dynamics
        const double w = 2.0 * std::numbers::pi;
        float y = 0.0f;
        const double f1 = v1.f1 + (v2.f1 - v1.f1) * mix, f2 = v1.f2 + (v2.f2 - v1.f2) * mix, f3 = v1.f3 + (v2.f3 - v1.f3) * mix;
        const BiquadCoeffs c1 = BiquadCoeffs::peak (fs, f1, 18.0, 4.0), c2 = BiquadCoeffs::peak (fs, f2, 14.0, 5.0), c3 = BiquadCoeffs::peak (fs, f3, 10.0, 6.0);
        float s = (float) glottal;
        s = st[0].process (c1, s);
        s = st[1].process (c2, s);
        s = st[2].process (c3, s);
        const float breath = (rng.nextFloat() * 2.0f - 1.0f);
        hp = 0.95f * hp + 0.05f * breath;
        y = (float) (env * s * 0.25) + 0.012f * (float) env * (breath - hp);
        juce::ignoreUnused (w, vib);
        b.setSample (0, i, y);
        b.setSample (1, i, y);
    }
    const auto cur = test::rmsDb (b, 0, 0, n);
    b.applyGain (juce::Decibels::decibelsToGain ((float) (rmsDb - cur)));
    return b;
}

// Drum loop: kick, snare (noise + tone), hats, with room-like decay.
inline juce::AudioBuffer<float> drums (int seconds, unsigned seed, float rmsDb)
{
    juce::Random rng ((juce::int64) seed);
    const int n = (int) fs * seconds;
    juce::AudioBuffer<float> b (2, n);
    b.clear();
    const double bpm = 100.0, beat = 60.0 / bpm;
    for (double t = 0.0; t < seconds; t += beat / 2.0) // 8th notes
    {
        const int step = (int) std::round (t / (beat / 2.0)) % 8;
        const int start = (int) (t * fs);
        auto add = [&] (int len, auto&& gen) {
            for (int k = 0; k < len && start + k < n; ++k)
            {
                const float s = gen ((double) k / fs);
                b.addSample (0, start + k, s);
                b.addSample (1, start + k, s * 0.95f);
            }
        };
        if (step == 0 || step == 4 || step == 5) // kick
            add ((int) (0.35 * fs), [] (double x) { const double ph = 50.0 * x + 90.0 * (1.0 - std::exp (-40.0 * x)) / 40.0; /* integral of 50 + 90 e^-40x */ return (float) (std::sin (2.0 * std::numbers::pi * ph) * std::exp (-x * 9.0) * 0.9); });
        if (step == 2 || step == 6) // snare
            add ((int) (0.25 * fs), [&] (double x) { return (float) ((rng.nextFloat() * 2.0f - 1.0f) * std::exp (-x * 22.0) * 0.6 + std::sin (2.0 * std::numbers::pi * 190.0 * x) * std::exp (-x * 30.0) * 0.5); });
        add ((int) (0.06 * fs), [&] (double x) { return (float) ((rng.nextFloat() * 2.0f - 1.0f) * std::exp (-x * 90.0) * 0.18); }); // hat
    }
    const auto cur = test::rmsDb (b, 0, 0, n);
    b.applyGain (juce::Decibels::decibelsToGain ((float) (rmsDb - cur)));
    return b;
}

}
}
