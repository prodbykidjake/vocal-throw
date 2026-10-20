#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

// RBJ cookbook biquad coefficients, computed in double precision. Frequencies are clamped to
// a safe range so extreme parameter values can never produce an unstable filter.
struct BiquadCoeffs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; // a0 normalised to 1

    static double clampFreq (double fs, double f) { return std::clamp (f, 10.0, 0.45 * fs); }
    static double clampQ (double q) { return std::clamp (q, 0.1, 20.0); }

    static BiquadCoeffs make (double b0, double b1, double b2, double a0, double a1, double a2)
    {
        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    static BiquadCoeffs peak (double fs, double f, double gainDb, double q)
    {
        const auto A = std::pow (10.0, gainDb / 40.0);
        const auto w0 = 2.0 * std::numbers::pi * clampFreq (fs, f) / fs;
        const auto alpha = std::sin (w0) / (2.0 * clampQ (q));
        const auto c = std::cos (w0);
        return make (1.0 + alpha * A, -2.0 * c, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * c, 1.0 - alpha / A);
    }

    static BiquadCoeffs lowShelf (double fs, double f, double gainDb, double q)
    {
        const auto A = std::pow (10.0, gainDb / 40.0);
        const auto w0 = 2.0 * std::numbers::pi * clampFreq (fs, f) / fs;
        const auto alpha = std::sin (w0) / (2.0 * clampQ (q));
        const auto c = std::cos (w0);
        const auto s = 2.0 * std::sqrt (A) * alpha;
        return make (A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
                     (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
    }

    static BiquadCoeffs highShelf (double fs, double f, double gainDb, double q)
    {
        const auto A = std::pow (10.0, gainDb / 40.0);
        const auto w0 = 2.0 * std::numbers::pi * clampFreq (fs, f) / fs;
        const auto alpha = std::sin (w0) / (2.0 * clampQ (q));
        const auto c = std::cos (w0);
        const auto s = 2.0 * std::sqrt (A) * alpha;
        return make (A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
                     (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
    }

    static BiquadCoeffs highPass (double fs, double f, double q)
    {
        const auto w0 = 2.0 * std::numbers::pi * clampFreq (fs, f) / fs;
        const auto alpha = std::sin (w0) / (2.0 * clampQ (q));
        const auto c = std::cos (w0);
        return make ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
    }

    double magnitudeDb (double f, double fs) const
    {
        const auto w = 2.0 * std::numbers::pi * f / fs;
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
        const auto h = (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
        return 20.0 * std::log10 (std::max (std::abs (h), 1.0e-12));
    }

    // Both poles inside the unit circle.
    bool isStable() const { return std::abs (a2) < 1.0 && std::abs (a1) < 1.0 + a2; }
};

// Transposed direct form II, double precision state.
struct BiquadState
{
    double z1 = 0.0, z2 = 0.0;

    void reset() { z1 = z2 = 0.0; }

    float process (const BiquadCoeffs& c, float in)
    {
        const double x = in;
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return (float) y;
    }
};
