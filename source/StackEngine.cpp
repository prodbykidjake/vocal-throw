#include "StackEngine.h"

namespace
{
    constexpr float maxDetuneCents = 25.0f;
    constexpr float maxDelayMs = 30.0f;
    constexpr float sideLayerGain = 0.85f;
    constexpr float octaveLayerGain = 0.8f;
    constexpr float attackSeconds = 0.002f;
    constexpr float endFadeSamples = 64.0f;
}

void StackEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    allNotesOff();
}

void StackEngine::setSample (const SampleData* newSample)
{
    allNotesOff();
    sample = newSample;
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
    const auto amount = spread * static_cast<float> (rank) / static_cast<float> (maxRank);

    shape.pan = side * amount;
    shape.cents = side * amount * maxDetuneCents;
    shape.delayMs = amount * maxDelayMs;
    shape.gain = sideLayerGain;
    return shape;
}

StackEngine::Layer* StackEngine::claimLayer()
{
    Layer* oldest = &layers[0];

    for (auto& layer : layers)
    {
        if (! layer.active)
            return &layer;

        if (layer.age < oldest->age)
            oldest = &layer;
    }

    return oldest;
}

void StackEngine::startLayer (int note, double semitoneOffset, const LayerShape& shape, float level, const StackParams& params)
{
    auto* layer = claimLayer();
    const auto semitones = static_cast<double> (note - rootNote) + semitoneOffset + static_cast<double> (shape.cents) / 100.0;
    const auto angle = (juce::jlimit (-1.0f, 1.0f, shape.pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;

    layer->active = true;
    layer->releasing = false;
    layer->oneShot = params.oneShot;
    layer->note = note;
    layer->delaySamples = static_cast<int> (shape.delayMs * 0.001 * sampleRate);
    layer->age = ++ageCounter;
    layer->position = 0.0;
    layer->ratio = std::pow (2.0, semitones / 12.0) * sample->sampleRate / sampleRate;
    layer->gainL = std::cos (angle) * level;
    layer->gainR = std::sin (angle) * level;
    layer->attack = 0.0f;
    layer->release = 1.0f;
    layer->releaseStep = static_cast<float> (1.0 / juce::jmax (1.0, params.releaseMs * 0.001 * sampleRate));
}

void StackEngine::noteOn (int note, float velocity, const StackParams& params)
{
    if (sample == nullptr || sample->size() == 0)
        return;

    const auto count = juce::jlimit (1, maxStack, params.stack);
    const auto hasOctave = params.octave > 0.01f;

    // keep the stack's overall level steady no matter how many copies are in it
    float sumSquares = 0.0f;

    for (int i = 0; i < count; ++i)
    {
        const auto g = shapeFor (i, count, params.spread).gain;
        sumSquares += g * g;
    }

    const auto octaveGain = octaveLayerGain * params.octave;
    sumSquares += hasOctave ? octaveGain * octaveGain : 0.0f;

    const auto stackBoost = 1.0f + 0.1f * static_cast<float> (count - 1);
    const auto velocityGain = 0.25f + 0.75f * juce::jlimit (0.0f, 1.0f, velocity);
    const auto level = velocityGain * stackBoost / std::sqrt (sumSquares);

    for (int i = 0; i < count; ++i)
    {
        const auto shape = shapeFor (i, count, params.spread);
        startLayer (note, 0.0, shape, level * shape.gain, params);
    }

    if (hasOctave)
        startLayer (note, 12.0, LayerShape {}, level * octaveGain, params);
}

void StackEngine::noteOff (int note)
{
    for (auto& layer : layers)
        if (layer.active && layer.note == note && ! layer.oneShot)
            layer.releasing = true;
}

void StackEngine::allNotesOff()
{
    for (auto& layer : layers)
        layer.active = false;
}

int StackEngine::getActiveLayerCount() const noexcept
{
    return static_cast<int> (std::count_if (layers.begin(), layers.end(), [] (const Layer& l) { return l.active; }));
}

float StackEngine::readInterpolated (const std::vector<float>& data, double position) noexcept
{
    const auto size = static_cast<int> (data.size());
    const auto i = static_cast<int> (position);
    const auto f = static_cast<float> (position - static_cast<double> (i));

    auto at = [&] (int k) { return (k < 0 || k >= size) ? 0.0f : data[static_cast<size_t> (k)]; };

    // 4-point Hermite
    const auto y0 = at (i - 1);
    const auto y1 = at (i);
    const auto y2 = at (i + 1);
    const auto y3 = at (i + 2);
    const auto c1 = 0.5f * (y2 - y0);
    const auto c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const auto c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

void StackEngine::render (float* left, float* right, int numSamples)
{
    if (sample == nullptr)
        return;

    const auto attackStep = static_cast<float> (1.0 / (attackSeconds * sampleRate));
    const auto length = static_cast<double> (sample->size());

    for (auto& layer : layers)
    {
        if (! layer.active)
            continue;

        for (int i = 0; i < numSamples; ++i)
        {
            if (layer.delaySamples > 0)
            {
                --layer.delaySamples;
                continue;
            }

            if (layer.position >= length)
            {
                layer.active = false;
                break;
            }

            if (layer.releasing)
            {
                layer.release -= layer.releaseStep;

                if (layer.release <= 0.0f)
                {
                    layer.active = false;
                    break;
                }
            }

            layer.attack = juce::jmin (1.0f, layer.attack + attackStep);

            const auto remaining = static_cast<float> (length - layer.position);
            const auto endFade = juce::jmin (1.0f, remaining / endFadeSamples);
            const auto value = readInterpolated (sample->samples, layer.position) * layer.attack * layer.release * endFade;

            left[i] += value * layer.gainL;
            right[i] += value * layer.gainR;
            layer.position += layer.ratio;
        }
    }
}
