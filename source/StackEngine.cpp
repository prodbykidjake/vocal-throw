#include "StackEngine.h"

namespace
{
    constexpr float maxDetuneCents = 25.0f;
    constexpr float maxDriftMs = 30.0f;
    constexpr float minSpread = 0.12f;      // even at Spread 0 the copies differ a little, or they'd comb-filter
    constexpr float copyWindowMs = 40.0f;   // room the read head has to move in; long is smoothest for small detunes
    constexpr float octaveWindowMs = 30.0f; // shorter keeps the octave copy tight
    constexpr float minTapMs = 2.0f;
    constexpr float searchMs = 15.0f;       // covers one cycle of a voice down to about 67 Hz
    constexpr float octaveLevel = 0.7f;
    constexpr float maxBufferSeconds = 0.12f;
}

void StackEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    int size = 1;

    while (size < static_cast<int> (maxBufferSeconds * sampleRate))
        size *= 2;

    buffer.assign (static_cast<size_t> (size), 0.0f);
    mask = size - 1;

    // raised-cosine crossfade from 0 to 1
    for (size_t i = 0; i < fadeCurve.size(); ++i)
    {
        const auto x = static_cast<double> (i) / static_cast<double> (fadeCurve.size() - 1);
        fadeCurve[i] = static_cast<float> (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * x));
    }

    // gains and delays glide over about 20 ms when the knobs move
    smoothing = static_cast<float> (1.0 - std::exp (-1.0 / (0.02 * sampleRate)));

    reset();
}

void StackEngine::reset()
{
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    writeIndex = 0;

    for (auto& voice : copies)
        voice = {};

    octaveVoice = {};
}

LayerShape StackEngine::shapeFor (int index, int count, float spread)
{
    LayerShape shape;

    if (index <= 0 || count <= 1)
        return shape;

    // copies alternate left/right and move further out the higher their rank
    const auto side = (index % 2 == 1) ? -1.0f : 1.0f;
    const auto rank = (index + 1) / 2;
    const auto maxRank = juce::jmax (1, count / 2);
    const auto effectiveSpread = minSpread + (1.0f - minSpread) * juce::jlimit (0.0f, 1.0f, spread);
    const auto amount = effectiveSpread * static_cast<float> (rank) / static_cast<float> (maxRank);

    shape.pan = side * amount;
    shape.cents = side * amount * maxDetuneCents;
    shape.delayMs = amount * maxDriftMs;
    shape.gain = copyGain (count);
    return shape;
}

float StackEngine::copyGain (int count)
{
    return count <= 1 ? 0.0f : 0.75f * std::pow (static_cast<float> (count - 1), -0.35f);
}

void StackEngine::setVoice (Voice& voice, double ratio, float windowMs, float delayMs, float pan, float gain)
{
    const auto angle = (juce::jlimit (-1.0f, 1.0f, pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    const auto toSamples = [this] (float ms) { return static_cast<float> (ms * 0.001 * sampleRate); };

    voice.ratio = ratio;
    voice.minDelay = toSamples (minTapMs);
    voice.maxDelay = toSamples (minTapMs + windowMs);
    voice.searchRange = toSamples (searchMs);
    // the octave copy needs quick joins; small detunes can take long, smooth ones
    voice.fadeLength = toSamples (ratio > 1.5 ? 8.0f : 15.0f);
    voice.targetOffset = toSamples (delayMs);
    voice.targetGainL = std::cos (angle) * gain;
    voice.targetGainR = std::sin (angle) * gain;

    if (! voice.started)
    {
        voice.delayA = 0.5 * static_cast<double> (voice.minDelay + voice.maxDelay);
        voice.offset = voice.targetOffset;
        voice.started = true;
    }
}

void StackEngine::setParams (const StackParams& params)
{
    const auto count = juce::jlimit (1, maxStack, params.stack);

    for (int i = 0; i < maxCopies; ++i)
    {
        auto& voice = copies[static_cast<size_t> (i)];

        if (i + 1 < count)
        {
            const auto shape = shapeFor (i + 1, count, params.spread);
            setVoice (voice, std::pow (2.0, static_cast<double> (shape.cents) / 1200.0), copyWindowMs, shape.delayMs, shape.pan, shape.gain);
        }
        else
        {
            // unused copies fade out rather than cutting off
            voice.targetGainL = 0.0f;
            voice.targetGainR = 0.0f;
        }
    }

    setVoice (octaveVoice, 2.0, octaveWindowMs, 0.0f, 0.0f, octaveLevel * juce::jlimit (0.0f, 1.0f, params.octave));
}

float StackEngine::readDelayed (double delaySamples) const noexcept
{
    const auto position = static_cast<double> (writeIndex) - juce::jmax (1.0, delaySamples);
    const auto whole = static_cast<int> (std::floor (position));
    const auto frac = static_cast<float> (position - static_cast<double> (whole));
    const auto a = buffer[static_cast<size_t> (whole & mask)];
    const auto b = buffer[static_cast<size_t> ((whole + 1) & mask)];
    return a + frac * (b - a);
}

double StackEngine::findJump (const Voice& voice) const noexcept
{
    const auto drift = voice.ratio - 1.0;
    const auto offset = static_cast<double> (voice.offset);

    // pitching up, the delay shrinks, so jump back to a long delay; pitching down, jump forward to a short one
    const auto low = drift > 0.0 ? static_cast<double> (voice.maxDelay - voice.searchRange) : static_cast<double> (voice.minDelay);
    const auto high = drift > 0.0 ? static_cast<double> (voice.maxDelay) : static_cast<double> (voice.minDelay + voice.searchRange);

    // compare what the playing tap is about to play with what each candidate would play over the crossfade
    const auto steps = juce::jmax (1, static_cast<int> (voice.fadeLength / 4.0f));

    auto score = [&] (double candidate)
    {
        double total = 0.0;

        for (int k = 0; k < steps; ++k)
        {
            const auto travel = drift * static_cast<double> (k * 4);
            total += static_cast<double> (readDelayed (offset + voice.delayA - travel) * readDelayed (offset + candidate - travel));
        }

        return total;
    };

    // coarse pass every 4 samples, then the best spot refined sample by sample
    auto best = high;
    auto bestScore = -1.0e30;

    for (auto candidate = low; candidate <= high; candidate += 4.0)
    {
        if (const auto value = score (candidate); value > bestScore)
        {
            bestScore = value;
            best = candidate;
        }
    }

    const auto coarse = best;

    for (auto candidate = juce::jmax (low, coarse - 3.0); candidate <= juce::jmin (high, coarse + 3.0); candidate += 1.0)
    {
        if (const auto value = score (candidate); value > bestScore)
        {
            bestScore = value;
            best = candidate;
        }
    }

    return best;
}

float StackEngine::processVoice (Voice& voice) noexcept
{
    voice.offset += smoothing * (voice.targetOffset - voice.offset);
    voice.gainL += smoothing * (voice.targetGainL - voice.gainL);
    voice.gainR += smoothing * (voice.targetGainR - voice.gainR);

    if (voice.gainL + voice.gainR < 1.0e-5f && voice.targetGainL + voice.targetGainR < 1.0e-6f)
        return 0.0f;

    const auto drift = voice.ratio - 1.0;
    voice.delayA = juce::jlimit (1.0, static_cast<double> (voice.maxDelay + voice.searchRange), voice.delayA - drift);

    if (voice.fading)
    {
        voice.delayB -= drift;
    }
    else
    {
        // start the next crossfade early enough that it finishes before the playing tap runs out of room
        const auto endOfFade = voice.delayA - drift * static_cast<double> (voice.fadeLength);

        if (endOfFade <= static_cast<double> (voice.minDelay) || endOfFade >= static_cast<double> (voice.maxDelay))
        {
            voice.delayB = findJump (voice);
            voice.fade = 0.0f;
            voice.fading = true;
        }
    }

    const auto offset = static_cast<double> (voice.offset);
    auto value = readDelayed (offset + voice.delayA);

    if (voice.fading)
    {
        const auto w = fadeCurve[static_cast<size_t> (voice.fade * static_cast<float> (fadeCurve.size() - 1))];
        value += w * (readDelayed (offset + voice.delayB) - value);
        voice.fade += 1.0f / voice.fadeLength;

        if (voice.fade >= 1.0f)
        {
            voice.delayA = voice.delayB;
            voice.fading = false;
        }
    }

    return value;
}

void StackEngine::process (const float* monoInput, float* left, float* right, int numSamples)
{
    if (buffer.empty())
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        buffer[static_cast<size_t> (writeIndex)] = monoInput[i];

        for (auto& voice : copies)
        {
            const auto value = processVoice (voice);
            left[i] += value * voice.gainL;
            right[i] += value * voice.gainR;
        }

        const auto octave = processVoice (octaveVoice);
        left[i] += octave * octaveVoice.gainL;
        right[i] += octave * octaveVoice.gainR;

        writeIndex = (writeIndex + 1) & mask;
    }
}
