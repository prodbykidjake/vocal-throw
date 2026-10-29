#include <Harmony.h>
#include <PitchTracker.h>
#include <PluginProcessor.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
    constexpr double rate = 44100.0;
    constexpr int blockSize = 512;

    std::vector<float> tone (int numSamples, double frequency, float level, double sampleRate = rate)
    {
        std::vector<float> out (static_cast<size_t> (numSamples));

        // a few harmonics, so it is closer to a voice than a pure sine
        for (int i = 0; i < numSamples; ++i)
        {
            const auto phase = juce::MathConstants<double>::twoPi * frequency * i / sampleRate;
            out[static_cast<size_t> (i)] = level * static_cast<float> (0.7 * std::sin (phase) + 0.2 * std::sin (2.0 * phase) + 0.1 * std::sin (3.0 * phase));
        }

        return out;
    }

    float rmsOf (const std::vector<float>& signal, size_t start, size_t end)
    {
        double sum = 0.0;

        for (auto i = start; i < end; ++i)
            sum += static_cast<double> (signal[i]) * static_cast<double> (signal[i]);

        return static_cast<float> (std::sqrt (sum / static_cast<double> (juce::jmax<size_t> (1, end - start))));
    }

    void setParam (PluginProcessor& processor, const char* id, float value)
    {
        auto* parameter = processor.apvts.getParameter (id);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    void quiet (PluginProcessor& processor)
    {
        setParam (processor, ParamIDs::stack, 1.0f);
        setParam (processor, ParamIDs::octave, 0.0f);
        setParam (processor, ParamIDs::echo, 0.0f);
        setParam (processor, ParamIDs::space, 0.0f);
        setParam (processor, ParamIDs::dirt, 0.0f);
        setParam (processor, ParamIDs::tone, 1.0f);
        setParam (processor, ParamIDs::gain, 0.0f);
    }

    void prepare (PluginProcessor& processor)
    {
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (juce::AudioChannelSet::mono());
        layout.outputBuses.add (juce::AudioChannelSet::stereo());
        REQUIRE (processor.setBusesLayout (layout));
        processor.setRateAndBufferSizeDetails (rate, blockSize);
        processor.prepareToPlay (rate, blockSize);
    }

    // Runs a mono signal through and returns the left output. onBlock is called before each block with its start sample.
    std::vector<float> runLeft (PluginProcessor& processor, const std::vector<float>& input, const std::function<void (size_t)>& onBlock = {})
    {
        std::vector<float> out (input.size());
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        for (size_t start = 0; start < input.size(); start += blockSize)
        {
            if (onBlock)
                onBlock (start);

            const auto count = static_cast<int> (juce::jmin (static_cast<size_t> (blockSize), input.size() - start));
            buffer.setSize (2, count, false, false, true);
            buffer.clear();
            buffer.copyFrom (0, 0, input.data() + start, count);
            processor.processBlock (buffer, midi);
            std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + count, out.begin() + static_cast<std::ptrdiff_t> (start));
        }

        return out;
    }

    // Frequency from rising zero crossings.
    double zeroCrossingFrequency (const std::vector<float>& signal, size_t start, size_t end)
    {
        double first = -1.0, last = -1.0;
        int crossings = 0;

        for (auto i = start + 1; i < end; ++i)
        {
            const auto a = signal[i - 1];
            const auto b = signal[i];

            if (a < 0.0f && b >= 0.0f)
            {
                const auto t = static_cast<double> (i - 1) + static_cast<double> (-a / (b - a));

                if (first < 0.0)
                    first = t;
                else
                    ++crossings;

                last = t;
            }
        }

        return crossings > 0 ? crossings * rate / (last - first) : 0.0;
    }
}

TEST_CASE ("Harmony scale maths", "[harmony]")
{
    constexpr int c = 0;
    constexpr int a = 9;

    SECTION ("a 3rd up stays in the key")
    {
        CHECK (Harmony::semitonesFor (60.0f, c, false, 2) == 4);   // C to E
        CHECK (Harmony::semitonesFor (64.0f, c, false, 2) == 3);   // E to G
        CHECK (Harmony::semitonesFor (62.0f, c, false, 2) == 3);   // D to F
        CHECK (Harmony::semitonesFor (60.0f, a, true, 2) == 4);    // C to E in A minor
        CHECK (Harmony::semitonesFor (57.0f, a, true, 2) == 3);    // A to C in A minor
    }

    SECTION ("5ths and 3rds down")
    {
        CHECK (Harmony::semitonesFor (60.0f, c, false, 4) == 7);   // C to G
        CHECK (Harmony::semitonesFor (71.0f, c, false, 4) == 6);   // B to F
        CHECK (Harmony::semitonesFor (60.0f, c, false, -2) == -3); // C down to A
        CHECK (Harmony::semitonesFor (64.0f, c, false, -2) == -4); // E down to C
    }

    SECTION ("a slightly flat or off-key note is treated as the nearest scale note")
    {
        CHECK (Harmony::semitonesFor (63.8f, c, false, 2) == 3);   // nearly E
        CHECK (Harmony::semitonesFor (61.0f, c, false, 2) == 4);   // C# counts as C
    }

    SECTION ("note names")
    {
        CHECK (Harmony::noteName (60) == "C4");
        CHECK (Harmony::noteName (69) == "A4");
        CHECK (Harmony::noteName (61) == "C#4");
    }
}

TEST_CASE ("Pitch tracker", "[harmony]")
{
    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        for (const auto frequency : { 98.0, 220.0, 440.0, 784.0 })
        {
            PitchTracker tracker;
            tracker.prepare (sampleRate);
            const auto signal = tone (static_cast<int> (sampleRate * 0.3), frequency, 0.4f, sampleRate);
            tracker.process (signal.data(), static_cast<int> (signal.size()));

            INFO (sampleRate << " Hz rate, " << frequency << " Hz tone");
            REQUIRE (tracker.isVoiced());
            CHECK (tracker.getFrequency() == Catch::Approx (frequency).epsilon (0.01));
        }
    }

    SECTION ("silence is not pitched, and the last pitch is held")
    {
        PitchTracker tracker;
        tracker.prepare (rate);
        const auto signal = tone (static_cast<int> (rate * 0.2), 300.0, 0.4f);
        tracker.process (signal.data(), static_cast<int> (signal.size()));

        std::vector<float> silence (static_cast<size_t> (rate * 0.2), 0.0f);
        tracker.process (silence.data(), static_cast<int> (silence.size()));

        CHECK_FALSE (tracker.isVoiced());
        CHECK (tracker.getFrequency() == Catch::Approx (300.0).epsilon (0.01));
    }
}

TEST_CASE ("Harmony mode", "[harmony]")
{
    PluginProcessor processor;
    quiet (processor);
    setParam (processor, ParamIDs::harmony, 1.0f);
    setParam (processor, ParamIDs::key, 0.0f);     // C
    setParam (processor, ParamIDs::scale, 0.0f);   // major

    const auto c4 = 261.63;
    const auto input = tone (static_cast<int> (rate * 1.5), c4, 0.3f);

    auto harmonyPart = [&]
    {
        prepare (processor);
        const auto out = runLeft (processor, input);
        std::vector<float> added (out.size());

        // the dry vocal passes untouched, so what's left is the harmony
        for (size_t i = 0; i < out.size(); ++i)
            added[i] = out[i] - input[i];

        return added;
    };

    SECTION ("a 3rd up from C in C major is E")
    {
        setParam (processor, ParamIDs::interval, 0.0f);
        const auto added = harmonyPart();
        CHECK (zeroCrossingFrequency (added, static_cast<size_t> (rate * 0.5), added.size()) == Catch::Approx (329.63).epsilon (0.01));
    }

    SECTION ("a 5th up from C is G")
    {
        setParam (processor, ParamIDs::interval, 1.0f);
        const auto added = harmonyPart();
        CHECK (zeroCrossingFrequency (added, static_cast<size_t> (rate * 0.5), added.size()) == Catch::Approx (392.0).epsilon (0.01));
    }

    SECTION ("a 3rd down from C is A")
    {
        setParam (processor, ParamIDs::interval, 3.0f);
        const auto added = harmonyPart();
        CHECK (zeroCrossingFrequency (added, static_cast<size_t> (rate * 0.5), added.size()) == Catch::Approx (220.0).epsilon (0.01));
    }

    SECTION ("the UI is told what is sung and played")
    {
        setParam (processor, ParamIDs::interval, 2.0f);
        harmonyPart();
        const auto state = processor.getHarmonyState();
        CHECK (state.sungNote == Catch::Approx (60.0).margin (0.2));
        CHECK (state.count == 2);
        CHECK (state.semitones[0] == 4);
        CHECK (state.semitones[1] == 7);
    }
}

TEST_CASE ("Duck and Throw", "[space]")
{
    PluginProcessor processor;
    quiet (processor);

    // half a second of vocal, then a second of silence
    auto input = tone (static_cast<int> (rate * 1.5), 300.0, 0.3f);
    std::fill (input.begin() + static_cast<std::ptrdiff_t> (rate * 0.5), input.end(), 0.0f);

    const auto vocalStart = static_cast<size_t> (rate * 0.3);
    const auto vocalEnd = static_cast<size_t> (rate * 0.5);
    const auto gapStart = static_cast<size_t> (rate * 0.6);
    const auto gapEnd = static_cast<size_t> (rate * 1.0);

    auto effectOnly = [&] (const std::vector<float>& out)
    {
        std::vector<float> added (out.size());

        for (size_t i = 0; i < out.size(); ++i)
            added[i] = out[i] - input[i];

        return added;
    };

    SECTION ("Duck pushes the echo down under the vocal, and lets it back in the gap")
    {
        setParam (processor, ParamIDs::echo, 0.6f);
        prepare (processor);
        const auto open = effectOnly (runLeft (processor, input));

        setParam (processor, ParamIDs::duck, 1.0f);
        prepare (processor);
        const auto ducked = effectOnly (runLeft (processor, input));

        CHECK (rmsOf (ducked, vocalStart, vocalEnd) < 0.2f * rmsOf (open, vocalStart, vocalEnd));
        CHECK (rmsOf (ducked, gapStart, gapEnd) > 0.3f * rmsOf (open, gapStart, gapEnd));
    }

    SECTION ("with Echo and Reverb at zero, Throw still sends the vocal into a tail")
    {
        prepare (processor);
        const auto dry = effectOnly (runLeft (processor, input));
        CHECK (rmsOf (dry, gapStart, gapEnd) == 0.0f);

        prepare (processor);
        const auto thrown = effectOnly (runLeft (processor, input, [&] (size_t start)
                                                 {
                                                     setParam (processor, ParamIDs::throwOn, start >= vocalStart && start < vocalEnd ? 1.0f : 0.0f);
                                                 }));

        CHECK (rmsOf (thrown, gapStart, gapEnd) > 0.01f);
    }

    SECTION ("Throw ignores Duck")
    {
        setParam (processor, ParamIDs::duck, 1.0f);
        setParam (processor, ParamIDs::throwOn, 1.0f);
        prepare (processor);
        const auto thrown = effectOnly (runLeft (processor, input));
        CHECK (rmsOf (thrown, vocalStart, vocalEnd) > 0.03f);
    }

    SECTION ("the output meter sees the signal")
    {
        prepare (processor);
        runLeft (processor, input);
        processor.takeOutputPeaks();

        std::vector<float> loud = tone (blockSize * 4, 300.0, 0.3f);
        runLeft (processor, loud);
        const auto [left, right] = processor.takeOutputPeaks();
        CHECK (left > 0.2f);
        CHECK (right > 0.2f);

        const auto [againL, againR] = processor.takeOutputPeaks();
        CHECK (againL == 0.0f);
        CHECK (againR == 0.0f);
    }
}
