#include "SampleData.h"

namespace
{
    constexpr float silenceThreshold = 0.002f; // about -54 dB
    constexpr float targetPeak = 0.9f;
}

std::shared_ptr<SampleData> SampleData::fromBuffer (const juce::AudioBuffer<float>& buffer, double sampleRate, const juce::String& name)
{
    const auto numSamples = buffer.getNumSamples();
    const auto numChannels = buffer.getNumChannels();

    if (numSamples <= 0 || numChannels <= 0 || sampleRate <= 0.0)
        return nullptr;

    const auto maxLength = static_cast<int> (maxSeconds * sampleRate);
    const auto length = juce::jmin (numSamples, maxLength);

    std::vector<float> mono (static_cast<size_t> (length), 0.0f);
    const auto channelGain = 1.0f / static_cast<float> (numChannels);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const auto* src = buffer.getReadPointer (ch);

        for (int i = 0; i < length; ++i)
            mono[static_cast<size_t> (i)] += src[i] * channelGain;
    }

    int first = 0;
    int last = length - 1;

    while (first < length && std::abs (mono[static_cast<size_t> (first)]) < silenceThreshold)
        ++first;

    if (first >= length)
        return nullptr;

    while (last > first && std::abs (mono[static_cast<size_t> (last)]) < silenceThreshold)
        --last;

    // keep a couple of milliseconds of lead-in so the attack isn't clipped
    first = juce::jmax (0, first - static_cast<int> (0.002 * sampleRate));

    auto result = std::make_shared<SampleData>();
    result->samples.assign (mono.begin() + first, mono.begin() + last + 1);
    result->sampleRate = sampleRate;
    result->name = name;

    float peak = 0.0f;

    for (auto s : result->samples)
        peak = juce::jmax (peak, std::abs (s));

    if (peak > 0.0f)
    {
        const auto gain = targetPeak / peak;

        for (auto& s : result->samples)
            s *= gain;
    }

    return result;
}

std::shared_ptr<SampleData> SampleData::fromFile (const juce::File& file, juce::AudioFormatManager& formats, juce::String& errorMessage)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        errorMessage = "Couldn't read " + file.getFileName();
        return nullptr;
    }

    const auto length = static_cast<int> (juce::jmin (reader->lengthInSamples, static_cast<juce::int64> (maxSeconds * reader->sampleRate)));
    juce::AudioBuffer<float> buffer (static_cast<int> (reader->numChannels), length);

    if (! reader->read (&buffer, 0, length, 0, true, true))
    {
        errorMessage = "Couldn't read " + file.getFileName();
        return nullptr;
    }

    auto result = fromBuffer (buffer, reader->sampleRate, file.getFileNameWithoutExtension());

    if (result == nullptr)
        errorMessage = file.getFileName() + " is empty or silent";

    return result;
}

juce::String SampleData::toBase64() const
{
    std::vector<int16_t> pcm (samples.size());

    for (size_t i = 0; i < samples.size(); ++i)
        pcm[i] = static_cast<int16_t> (std::lround (juce::jlimit (-1.0f, 1.0f, samples[i]) * 32767.0f));

    return juce::Base64::toBase64 (pcm.data(), pcm.size() * sizeof (int16_t));
}

std::shared_ptr<SampleData> SampleData::fromBase64 (const juce::String& data, double sampleRate, const juce::String& name)
{
    juce::MemoryOutputStream decoded;

    if (! juce::Base64::convertFromBase64 (decoded, data) || sampleRate <= 0.0)
        return nullptr;

    const auto numSamples = decoded.getDataSize() / sizeof (int16_t);

    if (numSamples == 0 || numSamples > static_cast<size_t> (maxSeconds * sampleRate) + 1)
        return nullptr;

    std::vector<int16_t> pcm (numSamples);
    std::memcpy (pcm.data(), decoded.getData(), numSamples * sizeof (int16_t));

    auto result = std::make_shared<SampleData>();
    result->samples.resize (numSamples);

    for (size_t i = 0; i < numSamples; ++i)
        result->samples[i] = static_cast<float> (pcm[i]) / 32767.0f;

    result->sampleRate = sampleRate;
    result->name = name;
    return result;
}
