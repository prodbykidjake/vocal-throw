#include "SpectralAnalyzer.h"

namespace vs
{
// Averaging consecutive frames steadies the noisy bin-to-bin fluctuation of broadband content, so only
// peaks that persist across frames stand out against the envelope.
static constexpr float kPowerSmoothing = 0.5f;

void SpectralAnalyzer::prepare (const FrameConfig& config)
{
    cfg = config;
    fft = std::make_unique<juce::dsp::FFT> (cfg.fftOrder);
    mask = cfg.fftSize - 1;

    window.resize ((size_t) cfg.fftSize);
    for (int i = 0; i < cfg.fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) cfg.fftSize);

    fftData.assign ((size_t) cfg.fftSize * 2, 0.0f);
    smoothedPower.assign ((size_t) cfg.numBins, 0.0f);
    ring.assign ((size_t) cfg.fftSize, 0.0f);
    writePos = 0;
}

void SpectralAnalyzer::reset()
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    std::fill (smoothedPower.begin(), smoothedPower.end(), 0.0f);
    writePos = 0;
}

void SpectralAnalyzer::analyse() noexcept
{
    // writePos points at the oldest sample
    for (int i = 0; i < cfg.fftSize; ++i)
        fftData[(size_t) i] = ring[(size_t) ((writePos + i) & mask)] * window[(size_t) i];

    std::fill (fftData.begin() + cfg.fftSize, fftData.end(), 0.0f);
    fft->performRealOnlyForwardTransform (fftData.data(), true);

    // A full-scale sine under a Hann window peaks at fftSize / 4
    const float norm = 4.0f / (float) cfg.fftSize;
    const float norm2 = norm * norm;

    for (int k = 0; k < cfg.numBins; ++k)
    {
        const float re = fftData[(size_t) (2 * k)];
        const float im = fftData[(size_t) (2 * k + 1)];
        const float p = (re * re + im * im) * norm2;
        smoothedPower[(size_t) k] += kPowerSmoothing * (p - smoothedPower[(size_t) k]);
    }
}
} // namespace vs
