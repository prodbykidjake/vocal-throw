#include "SignalAnalyzer.h"
#include <algorithm>
#include <juce_dsp/juce_dsp.h>
#include <numbers>
#include <numeric>

namespace
{
constexpr float silenceGateDb = -60.0f;
constexpr int fftOrder = 13;
constexpr int fftSize = 1 << fftOrder;

float toDb (double power) { return (float) (10.0 * std::log10 (std::max (power, 1.0e-12))); }

float percentile (std::vector<float> v, float p)
{
    if (v.empty())
        return 0.0f;
    std::sort (v.begin(), v.end());
    return v[(size_t) std::clamp ((int) std::lround (p * (float) (v.size() - 1)), 0, (int) v.size() - 1)];
}
}

Features SignalAnalyzer::analyse (const float* left, const float* right, int n, double sampleRate)
{
    Features f;
    f.analysedSeconds = (float) (n / sampleRate);
    if (n < (int) sampleRate / 2 || left == nullptr || right == nullptr)
        return f;

    // --- levels, clipping, stereo -----------------------------------------------------------
    const int frame100 = std::max (1, (int) (0.1 * sampleRate));
    std::vector<float> frameDb;      // all 100 ms frames
    std::vector<bool> frameActive;
    double activePower = 0.0, ll = 0, rr = 0, lr = 0, mm = 0, ss = 0;
    int activeFrames = 0, run = 0;
    float peak = 0.0f;

    for (int start = 0; start + frame100 <= n; start += frame100)
    {
        double power = 0.0;
        for (int i = start; i < start + frame100; ++i)
            power += 0.5 * ((double) left[i] * left[i] + (double) right[i] * right[i]);
        power /= frame100;
        const auto db = toDb (power);
        const bool active = db > silenceGateDb;
        frameDb.push_back (db);
        frameActive.push_back (active);
        if (active)
        {
            ++activeFrames;
            activePower += power;
            for (int i = start; i < start + frame100; ++i)
            {
                ll += (double) left[i] * left[i];
                rr += (double) right[i] * right[i];
                lr += (double) left[i] * right[i];
                const double m = 0.5 * (left[i] + right[i]), s = 0.5 * (left[i] - right[i]);
                mm += m * m;
                ss += s * s;
            }
        }
    }

    for (int i = 0; i < n; ++i)
    {
        const auto a = std::max (std::abs (left[i]), std::abs (right[i]));
        peak = std::max (peak, a);
        run = a >= 0.999f ? run + 1 : 0;
        if (run == 3)
            ++f.clippingEvents;
    }

    f.activeSeconds = (float) (activeFrames * frame100 / sampleRate);
    if (f.activeSeconds < 1.0f)
        return f; // not enough signal to say anything useful

    f.peakDb = (float) (20.0 * std::log10 (std::max (peak, 1.0e-6f)));
    f.rmsDb = toDb (activePower / activeFrames);
    f.crestDb = f.peakDb - f.rmsDb;
    {
        std::vector<float> active;
        for (size_t i = 0; i < frameDb.size(); ++i)
            if (frameActive[i])
                active.push_back (frameDb[i]);
        f.dynamicRangeDb = percentile (active, 0.95f) - percentile (active, 0.10f);
    }
    {
        const auto denom = std::sqrt (ll * rr);
        f.stereoCorrelation = denom > 1.0e-12 ? (float) std::clamp (lr / denom, -1.0, 1.0) : 1.0f;
        const auto m = std::sqrt (mm), s = std::sqrt (ss);
        f.widthIndex = m + s > 1.0e-9 ? (float) std::min (1.0, 2.0 * s / (m + s)) : 0.0f;
    }

    // --- onset strength: steepest level rises over 10 ms frames --------------------------------
    {
        const int frame10 = std::max (1, (int) (0.01 * sampleRate));
        std::vector<float> rises;
        float prev = -120.0f;
        for (int start = 0; start + frame10 <= n; start += frame10)
        {
            double power = 0.0;
            for (int i = start; i < start + frame10; ++i)
                power += 0.5 * ((double) left[i] * left[i] + (double) right[i] * right[i]);
            const auto db = toDb (power / frame10);
            if (db > silenceGateDb + 10.0f && prev > silenceGateDb + 10.0f && db > prev)
                rises.push_back (db - prev);
            prev = db;
        }
        if (rises.size() >= 10)
        {
            std::sort (rises.begin(), rises.end(), std::greater<float>());
            const auto top = std::max<size_t> (1, rises.size() / 10);
            f.onsetStrengthDb = std::accumulate (rises.begin(), rises.begin() + (long) top, 0.0f) / (float) top;
        }
    }

    // --- averaged power spectrum on the mono sum -----------------------------------------------
    std::vector<float> window ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * (float) std::cos (2.0 * std::numbers::pi * i / fftSize);

    juce::dsp::FFT fft (fftOrder);
    std::vector<float> fftData ((size_t) fftSize * 2);
    std::vector<double> power ((size_t) fftSize / 2 + 1, 0.0);
    int frames = 0;
    for (int start = 0; start + fftSize <= n; start += fftSize / 2)
    {
        double energy = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            const auto x = 0.5f * (left[start + i] + right[start + i]);
            energy += (double) x * x;
            fftData[(size_t) i] = x * window[(size_t) i];
        }
        if (toDb (energy / fftSize) <= silenceGateDb)
            continue; // skip silent frames so gaps do not drag the average spectrum down
        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (fftData.data());
        for (size_t k = 0; k <= (size_t) fftSize / 2; ++k)
            power[k] += (double) fftData[k] * fftData[k];
        ++frames;
    }
    if (frames == 0)
        return f;

    const double binHz = sampleRate / fftSize;
    for (int b = 0; b < Features::numBands; ++b)
    {
        const double centre = Features::bandCentre (b);
        const double lo = centre / std::exp2 (1.0 / 12.0), hi = centre * std::exp2 (1.0 / 12.0);
        double sum = 0.0;
        int count = 0;
        for (auto k = (size_t) std::ceil (lo / binHz); (double) k * binHz < hi && k <= (size_t) fftSize / 2; ++k)
        {
            sum += power[k];
            ++count;
        }
        // Band energy (sum over the bins it covers), so pink noise reads flat and white noise rises 3 dB/octave.
        double p;
        if (count > 0)
            p = sum / count * (hi - lo) / binHz; // mean density x width: avoids quantisation where a band spans only a few bins
        else // band narrower than a bin: interpolate the spectrum and scale by the band's width in bins
        {
            const auto pos = centre / binHz;
            const auto k0 = (size_t) std::floor (pos);
            const auto frac = pos - (double) k0;
            const auto density = power[k0] * (1.0 - frac) + power[std::min (k0 + 1, (size_t) fftSize / 2)] * frac;
            p = density * (hi - lo) / binHz;
        }
        f.bandDb[(size_t) b] = toDb (p / frames);
    }

    f.valid = true;
    return f;
}

Features SignalAnalyzer::analyseRecent (const CaptureRing& ring, double sampleRate, float seconds)
{
    const int wanted = (int) ((double) seconds * sampleRate);
    std::vector<float> l ((size_t) std::max (0, wanted)), r (l.size());
    const int n = ring.copyRecent (l.data(), r.data(), wanted);
    return analyse (l.data(), r.data(), n, sampleRate);
}
