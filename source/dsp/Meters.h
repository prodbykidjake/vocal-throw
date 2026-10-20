#pragma once

#include "Biquad.h"
#include <array>
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>

namespace meter_detail
{
// Lock-free "max since last read" accumulator: audio thread calls accumulate(), UI calls take().
struct PeakAccumulator
{
    void accumulate (float v)
    {
        auto cur = value.load (std::memory_order_relaxed);
        while (v > cur && ! value.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
    }
    float take() { return value.exchange (0.0f, std::memory_order_relaxed); }
    std::atomic<float> value { 0.0f };
};
}

// Sample peak, true peak (4x oversampled, 12-tap windowed-sinc interpolation), RMS (300 ms
// one-pole), stereo correlation and a width index. All outputs are lock-free for the UI thread.
class LevelMeter
{
public:
    static constexpr int channels = 2;

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        for (int p = 0; p < 3; ++p)
        {
            const double d = 0.25 * (p + 1);
            for (int j = 0; j < taps; ++j)
            {
                const double x = (taps / 2 - 1 + d) - j;
                const double sinc = std::abs (x) < 1.0e-9 ? 1.0 : std::sin (std::numbers::pi * x) / (std::numbers::pi * x);
                const double w = 0.5 * (1.0 + std::cos (std::numbers::pi * x / (taps / 2)));
                kernel[(size_t) p][(size_t) j] = (float) (sinc * w);
            }
        }
        reset();
    }

    void reset()
    {
        for (auto& h : history)
            h.fill (0.0f);
        ms = { 0.0f, 0.0f };
        sumLL = sumRR = sumLR = 0.0f;
        sumMM = sumSS = 0.0f;
        for (auto& a : { &peakAcc[0], &peakAcc[1], &truePeakAcc[0], &truePeakAcc[1] })
            a->value.store (0.0f);
        rmsOut[0] = rmsOut[1] = 0.0f;
        correlation.store (1.0f);
        widthIndex.store (0.0f);
    }

    void process (const juce::AudioBuffer<float>& buffer, int numChannels)
    {
        const int n = buffer.getNumSamples();
        if (n == 0)
            return;
        numChannels = std::min (numChannels, channels);
        const auto alpha = 1.0f - std::exp (-(float) n / (float) (0.3 * sampleRate));

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* x = buffer.getReadPointer (ch);
            auto& h = history[(size_t) ch];
            float peak = 0.0f, tp = 0.0f;
            double sq = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const auto v = x[i];
                std::move (h.begin() + 1, h.end(), h.begin());
                h[taps - 1] = v;
                peak = std::max (peak, std::abs (v));
                tp = std::max (tp, std::abs (v));
                for (size_t p = 0; p < 3; ++p)
                {
                    float acc = 0.0f;
                    for (size_t j = 0; j < (size_t) taps; ++j)
                        acc += h[j] * kernel[p][j];
                    tp = std::max (tp, std::abs (acc));
                }
                sq += (double) v * v;
            }
            peakAcc[(size_t) ch].accumulate (peak);
            truePeakAcc[(size_t) ch].accumulate (tp);
            ms[(size_t) ch] += ((float) (sq / n) - ms[(size_t) ch]) * alpha;
            rmsOut[(size_t) ch].store (std::sqrt (ms[(size_t) ch]), std::memory_order_relaxed);
        }

        if (numChannels == 2)
        {
            const auto* l = buffer.getReadPointer (0);
            const auto* r = buffer.getReadPointer (1);
            double ll = 0, rr = 0, lr = 0, mm = 0, ss = 0;
            for (int i = 0; i < n; ++i)
            {
                ll += (double) l[i] * l[i];
                rr += (double) r[i] * r[i];
                lr += (double) l[i] * r[i];
                const double m = 0.5 * (l[i] + r[i]), s = 0.5 * (l[i] - r[i]);
                mm += m * m;
                ss += s * s;
            }
            sumLL += ((float) (ll / n) - sumLL) * alpha;
            sumRR += ((float) (rr / n) - sumRR) * alpha;
            sumLR += ((float) (lr / n) - sumLR) * alpha;
            sumMM += ((float) (mm / n) - sumMM) * alpha;
            sumSS += ((float) (ss / n) - sumSS) * alpha;
            const auto denom = std::sqrt (sumLL * sumRR);
            correlation.store (denom > 1.0e-12f ? std::clamp (sumLR / denom, -1.0f, 1.0f) : 1.0f, std::memory_order_relaxed);
            const auto m = std::sqrt (sumMM), s = std::sqrt (sumSS);
            widthIndex.store (m + s > 1.0e-9f ? std::min (1.0f, 2.0f * s / (m + s)) : 0.0f, std::memory_order_relaxed);
        }
    }

    // Per-channel values since the previous call (linear gain).
    float takeSamplePeak (int ch) { return peakAcc[(size_t) ch].take(); }
    float takeTruePeak (int ch) { return truePeakAcc[(size_t) ch].take(); }
    float getRms (int ch) const { return rmsOut[(size_t) ch].load (std::memory_order_relaxed); }
    float getCorrelation() const { return correlation.load (std::memory_order_relaxed); }
    float getWidthIndex() const { return widthIndex.load (std::memory_order_relaxed); } // 0 mono .. 1 wide

private:
    static constexpr int taps = 12;
    double sampleRate = 44100.0;
    std::array<std::array<float, taps>, channels> history {};
    std::array<std::array<float, taps>, 3> kernel {};
    std::array<float, channels> ms {};
    float sumLL = 0, sumRR = 0, sumLR = 0, sumMM = 0, sumSS = 0;
    std::array<meter_detail::PeakAccumulator, channels> peakAcc, truePeakAcc;
    std::array<std::atomic<float>, channels> rmsOut {};
    std::atomic<float> correlation { 1.0f }, widthIndex { 0.0f };
};

// ITU-R BS.1770 K-weighted loudness with a one-pole (1.5 s) power average, which approximates
// the 3 s short-term window. Used for fair A/B level matching, not for compliance reporting.
class LoudnessMeter
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        // Stage 1: high shelf, stage 2: RLB high-pass (coefficients per pyloudnorm / BS.1770).
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (std::numbers::pi * f0 / fs);
            const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            shelf = { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                      2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (std::numbers::pi * f0 / fs);
            const double a0 = 1.0 + K / Q + K * K;
            highpass = { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
        }
        reset();
    }

    void reset()
    {
        for (auto& s : st)
            s = {};
        power = 0.0f;
        lufs.store (-120.0f);
    }

    void process (const juce::AudioBuffer<float>& buffer, int numChannels)
    {
        const int n = buffer.getNumSamples();
        if (n == 0)
            return;
        numChannels = std::min (numChannels, 2);
        double sum = 0.0;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* x = buffer.getReadPointer (ch);
            auto& s = st[(size_t) ch];
            double sq = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const auto y = s.hp.process (highpass, s.shelf.process (shelf, x[i]));
                sq += (double) y * y;
            }
            sum += sq / n;
        }
        const auto alpha = 1.0f - std::exp (-(float) n / (float) (1.5 * fs));
        power += ((float) sum - power) * alpha;
        lufs.store (power > 1.0e-12f ? -0.691f + 10.0f * std::log10 (power) : -120.0f, std::memory_order_relaxed);
    }

    float getLufs() const { return lufs.load (std::memory_order_relaxed); }

private:
    struct Stage
    {
        BiquadState shelf, hp;
    };
    double fs = 48000.0;
    BiquadCoeffs shelf, highpass;
    std::array<Stage, 2> st;
    float power = 0.0f;
    std::atomic<float> lufs { -120.0f };
};
