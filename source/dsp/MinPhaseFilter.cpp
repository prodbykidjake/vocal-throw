#include "MinPhaseFilter.h"

namespace vs
{
void MinPhaseFilter::prepare (const FrameConfig& config)
{
    cfg = config;
    K = cfg.kernelLength;
    fft = std::make_unique<juce::dsp::FFT> (cfg.fftOrder);
    bufA.assign ((size_t) cfg.fftSize, {});
    bufB.assign ((size_t) cfg.fftSize, {});
    kernelCur.assign ((size_t) K, 0.0f);
    kernelPrev.assign ((size_t) K, 0.0f);
    history.assign ((size_t) K * 2, 0.0f);
    fadeStep = 1.0f / (float) cfg.hopSize;

    // The kernel tail fades out over its last quarter so truncation doesn't click
    taper.assign ((size_t) K, 1.0f);
    const int fadeLen = K / 4;
    for (int i = 0; i < fadeLen; ++i)
        taper[(size_t) (K - fadeLen + i)] = 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) i / (float) fadeLen);

    reset();
}

void MinPhaseFilter::reset()
{
    // identity filter: stored reversed, so the last tap is the one that multiplies the newest sample
    std::fill (kernelCur.begin(), kernelCur.end(), 0.0f);
    kernelCur[(size_t) K - 1] = 1.0f;
    kernelPrev = kernelCur;
    std::fill (history.begin(), history.end(), 0.0f);
    writePos = 0;
    fadePos = cfg.hopSize;
}

void MinPhaseFilter::setCurve (const float* reductionDb) noexcept
{
    const int n = cfg.fftSize;
    const int half = n / 2;
    constexpr float dbToLn = 0.11512925f; // ln(10) / 20

    // log magnitude, mirrored to a full symmetric spectrum
    for (int k = 0; k <= half; ++k)
    {
        const float lnMag = -reductionDb[k] * dbToLn;
        bufA[(size_t) k] = { lnMag, 0.0f };
        if (k > 0 && k < half)
            bufA[(size_t) (n - k)] = { lnMag, 0.0f };
    }

    // real cepstrum
    fft->perform (bufA.data(), bufB.data(), true);

    // fold the cepstrum onto positive quefrencies (this is what makes the response minimum phase)
    bufA[0] = { bufB[0].real(), 0.0f };
    for (int i = 1; i < half; ++i)
        bufA[(size_t) i] = { 2.0f * bufB[(size_t) i].real(), 0.0f };
    bufA[(size_t) half] = { bufB[(size_t) half].real(), 0.0f };
    for (int i = half + 1; i < n; ++i)
        bufA[(size_t) i] = { 0.0f, 0.0f };

    // back to the frequency domain gives log(H) with the minimum-phase phase; exponentiate it
    fft->perform (bufA.data(), bufB.data(), false);
    for (int k = 0; k < n; ++k)
    {
        const float mag = std::exp (bufB[(size_t) k].real());
        const float ph = bufB[(size_t) k].imag();
        bufA[(size_t) k] = { mag * std::cos (ph), mag * std::sin (ph) };
    }

    fft->perform (bufA.data(), bufB.data(), true);

    std::swap (kernelPrev, kernelCur);
    for (int j = 0; j < K; ++j)
        kernelCur[(size_t) (K - 1 - j)] = bufB[(size_t) j].real() * taper[(size_t) j];

    fadePos = 0;
}

void MinPhaseFilter::getCurrentResponse (std::vector<float>& magnitudes) const
{
    const int n = cfg.fftSize;
    std::vector<juce::dsp::Complex<float>> in ((size_t) n), out ((size_t) n);
    for (int j = 0; j < K; ++j)
        in[(size_t) j] = { kernelCur[(size_t) (K - 1 - j)], 0.0f };

    fft->perform (in.data(), out.data(), false);

    magnitudes.resize ((size_t) cfg.numBins);
    for (int k = 0; k < cfg.numBins; ++k)
        magnitudes[(size_t) k] = std::abs (out[(size_t) k]);
}
} // namespace vs
