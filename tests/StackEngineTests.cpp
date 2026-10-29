#include <PluginProcessor.h>
#include <StackEngine.h>
#include <catch2/catch_approx.hpp>
#include <juce_dsp/juce_dsp.h>
#include <catch2/catch_test_macros.hpp>
#include <set>

namespace
{
    constexpr double testRate = 44100.0;

    std::vector<float> sine (int numSamples, double frequency, float level = 0.5f)
    {
        std::vector<float> out (static_cast<size_t> (numSamples));

        for (int i = 0; i < numSamples; ++i)
            out[static_cast<size_t> (i)] = level * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * frequency * i / testRate));

        return out;
    }

    // Frequency from rising zero crossings, between the first and last one in the range.
    double measureFrequency (const std::vector<float>& signal, size_t start, size_t end)
    {
        double first = -1.0, last = -1.0;
        int crossings = 0;

        for (size_t i = start + 1; i < end; ++i)
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

        return crossings > 0 ? crossings * testRate / (last - first) : 0.0;
    }

    // The strongest frequency in the signal, from an FFT with the peak bin interpolated.
    double dominantFrequency (const std::vector<float>& signal, size_t start)
    {
        constexpr int order = 15;
        constexpr int size = 1 << order;
        REQUIRE (signal.size() >= start + static_cast<size_t> (size));

        juce::dsp::FFT fft (order);
        std::vector<float> data (static_cast<size_t> (size) * 2, 0.0f);

        for (int i = 0; i < size; ++i)
        {
            const auto hann = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (size - 1));
            data[static_cast<size_t> (i)] = signal[start + static_cast<size_t> (i)] * static_cast<float> (hann);
        }

        fft.performFrequencyOnlyForwardTransform (data.data());

        size_t peak = 1;

        for (size_t i = 2; i < static_cast<size_t> (size / 2); ++i)
            if (data[i] > data[peak])
                peak = i;

        const auto a = static_cast<double> (data[peak - 1]);
        const auto b = static_cast<double> (data[peak]);
        const auto c = static_cast<double> (data[peak + 1]);
        const auto offset = 0.5 * (a - c) / (a - 2.0 * b + c);
        return (static_cast<double> (peak) + offset) * testRate / size;
    }

    // Share of the signal's energy between lowHz and highHz.
    double energyFraction (const std::vector<float>& signal, size_t start, double lowHz, double highHz)
    {
        constexpr int order = 15;
        constexpr int size = 1 << order;
        REQUIRE (signal.size() >= start + static_cast<size_t> (size));

        juce::dsp::FFT fft (order);
        std::vector<float> data (static_cast<size_t> (size) * 2, 0.0f);

        for (int i = 0; i < size; ++i)
        {
            const auto hann = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (size - 1));
            data[static_cast<size_t> (i)] = signal[start + static_cast<size_t> (i)] * static_cast<float> (hann);
        }

        fft.performFrequencyOnlyForwardTransform (data.data());

        double inside = 0.0, total = 0.0;

        for (int i = 1; i < size / 2; ++i)
        {
            const auto power = static_cast<double> (data[static_cast<size_t> (i)]) * static_cast<double> (data[static_cast<size_t> (i)]);
            const auto hz = i * testRate / size;
            total += power;

            if (hz >= lowHz && hz <= highHz)
                inside += power;
        }

        return total > 0.0 ? inside / total : 0.0;
    }

    std::vector<float> noise (int numSamples, float level)
    {
        juce::Random random (42);
        std::vector<float> out (static_cast<size_t> (numSamples));

        for (auto& s : out)
            s = level * (random.nextFloat() * 2.0f - 1.0f);

        return out;
    }

    float rms (const float* data, int numSamples)
    {
        double sum = 0.0;

        for (int i = 0; i < numSamples; ++i)
            sum += static_cast<double> (data[i]) * static_cast<double> (data[i]);

        return static_cast<float> (std::sqrt (sum / juce::jmax (1, numSamples)));
    }

    void setParam (PluginProcessor& processor, const char* id, float value)
    {
        auto* parameter = processor.apvts.getParameter (id);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    // every effect at zero and a single voice: the plugin should do nothing at all
    void makeTransparent (PluginProcessor& processor)
    {
        setParam (processor, ParamIDs::stack, 1.0f);
        setParam (processor, ParamIDs::octave, 0.0f);
        setParam (processor, ParamIDs::echo, 0.0f);
        setParam (processor, ParamIDs::space, 0.0f);
        setParam (processor, ParamIDs::dirt, 0.0f);
        setParam (processor, ParamIDs::tone, 1.0f);
        setParam (processor, ParamIDs::gain, 0.0f);
    }

    void prepare (PluginProcessor& processor, juce::AudioChannelSet in, juce::AudioChannelSet out, int blockSize = 512)
    {
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (in);
        layout.outputBuses.add (out);
        REQUIRE (processor.setBusesLayout (layout));
        processor.setRateAndBufferSizeDetails (testRate, blockSize);
        processor.prepareToPlay (testRate, blockSize);
    }

    // Runs a mono signal through the processor block by block and returns each output channel.
    std::vector<std::vector<float>> run (PluginProcessor& processor, const std::vector<float>& input, int blockSize = 512)
    {
        const auto numOut = processor.getTotalNumOutputChannels();
        const auto numChannels = juce::jmax (numOut, processor.getTotalNumInputChannels());
        std::vector<std::vector<float>> out (static_cast<size_t> (numOut), std::vector<float> (input.size()));
        juce::AudioBuffer<float> buffer (numChannels, blockSize);
        juce::MidiBuffer midi;

        for (size_t start = 0; start < input.size(); start += static_cast<size_t> (blockSize))
        {
            const auto count = static_cast<int> (juce::jmin (static_cast<size_t> (blockSize), input.size() - start));
            buffer.setSize (numChannels, count, false, false, true);
            buffer.clear();

            for (int ch = 0; ch < processor.getTotalNumInputChannels(); ++ch)
                buffer.copyFrom (ch, 0, input.data() + start, count);

            processor.processBlock (buffer, midi);

            for (int ch = 0; ch < numOut; ++ch)
                std::copy (buffer.getReadPointer (ch), buffer.getReadPointer (ch) + count, out[static_cast<size_t> (ch)].begin() + static_cast<std::ptrdiff_t> (start));
        }

        return out;
    }
}

//==============================================================================
TEST_CASE ("Stack layout", "[engine]")
{
    SECTION ("the first voice is the untouched original")
    {
        const auto shape = StackEngine::shapeFor (0, 4, 1.0f);
        CHECK (shape.cents == Catch::Approx (0.0f));
        CHECK (shape.pan == Catch::Approx (0.0f));
        CHECK (shape.delayMs == Catch::Approx (0.0f));
    }

    SECTION ("copies alternate sides and move out with rank")
    {
        const auto a = StackEngine::shapeFor (1, 5, 1.0f);
        const auto b = StackEngine::shapeFor (2, 5, 1.0f);
        const auto c = StackEngine::shapeFor (4, 5, 1.0f);
        CHECK (a.pan < 0.0f);
        CHECK (b.pan > 0.0f);
        CHECK (a.pan == Catch::Approx (-b.pan));
        CHECK (std::abs (c.pan) > std::abs (a.pan));
        CHECK (std::abs (c.pan) <= 1.0f);
    }

    SECTION ("even with no spread the copies differ a little, so they don't comb-filter")
    {
        const auto shape = StackEngine::shapeFor (1, 3, 0.0f);
        CHECK (std::abs (shape.cents) > 1.0f);
        CHECK (shape.delayMs > 1.0f);
    }

    SECTION ("a single voice has no copies")
    {
        CHECK (StackEngine::copyGain (1) == Catch::Approx (0.0f));
        CHECK (StackEngine::copyGain (2) > 0.0f);
        CHECK (StackEngine::copyGain (6) < StackEngine::copyGain (2));
    }
}

TEST_CASE ("Stack engine", "[engine]")
{
    StackEngine engine;
    engine.prepare (testRate);
    StackParams params;

    const auto numSamples = static_cast<int> (testRate);
    const auto input = sine (numSamples, 440.0);
    std::vector<float> left (input.size(), 0.0f), right (input.size(), 0.0f);

    auto render = [&]
    {
        engine.setParams (params);
        std::fill (left.begin(), left.end(), 0.0f);
        std::fill (right.begin(), right.end(), 0.0f);
        engine.process (input.data(), left.data(), right.data(), numSamples);
    };

    SECTION ("one voice and no octave adds nothing")
    {
        params.stack = 1;
        params.octave = 0.0f;
        render();
        CHECK (rms (left.data(), numSamples) == Catch::Approx (0.0f).margin (1.0e-6));
        CHECK (rms (right.data(), numSamples) == Catch::Approx (0.0f).margin (1.0e-6));
    }

    SECTION ("a copy is detuned by the right amount")
    {
        // two voices at full spread: the copy is 25 cents flat and hard left
        params.stack = 2;
        params.spread = 1.0f;
        render();

        const auto expected = 440.0 * std::pow (2.0, -25.0 / 1200.0);
        CHECK (measureFrequency (left, input.size() / 2, input.size()) == Catch::Approx (expected).epsilon (0.005));
        CHECK (rms (right.data() + numSamples / 2, numSamples / 2) < 0.001f);
    }

    SECTION ("High Hype adds a copy an octave up")
    {
        params.stack = 1;
        params.octave = 1.0f;
        render();

        // zero crossings are unreliable here (the crossfading taps partly cancel on a pure tone), so use the spectrum
        CHECK (dominantFrequency (left, input.size() - 32768) == Catch::Approx (880.0).epsilon (0.02));

        // and the octave is clean: most of its energy sits at 880 Hz rather than in warbly side tones
        CHECK (energyFraction (left, input.size() - 32768, 860.0, 900.0) > 0.8);
        CHECK (rms (left.data() + numSamples / 2, numSamples / 2) > 0.1f);
    }

    SECTION ("silence in, silence out")
    {
        params.stack = StackEngine::maxStack;
        params.octave = 1.0f;
        engine.setParams (params);

        std::vector<float> quiet (input.size(), 0.0f);
        engine.process (quiet.data(), left.data(), right.data(), numSamples);
        CHECK (rms (left.data(), numSamples) == Catch::Approx (0.0f).margin (1.0e-6));
    }

    SECTION ("everything at maximum stays finite and in range")
    {
        params.stack = StackEngine::maxStack;
        params.spread = 1.0f;
        params.octave = 1.0f;
        render();

        float peak = 0.0f;

        for (size_t i = 0; i < input.size(); ++i)
        {
            REQUIRE (std::isfinite (left[i]));
            REQUIRE (std::isfinite (right[i]));
            peak = juce::jmax (peak, std::abs (left[i]), std::abs (right[i]));
        }

        CHECK (peak > 0.1f);
        CHECK (peak < 3.0f);
    }
}

//==============================================================================
TEST_CASE ("Processor as an insert", "[processor]")
{
    PluginProcessor processor;
    const auto input = sine (static_cast<int> (testRate * 0.5), 300.0, 0.4f);

    SECTION ("it is an audio effect, not an instrument")
    {
        CHECK_FALSE (processor.acceptsMidi());
        CHECK_FALSE (processor.isMidiEffect());
    }

    SECTION ("supports mono and stereo tracks")
    {
        auto supports = [&] (juce::AudioChannelSet in, juce::AudioChannelSet out)
        {
            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add (in);
            layout.outputBuses.add (out);
            return processor.checkBusesLayoutSupported (layout);
        };

        CHECK (supports (juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo()));
        CHECK (supports (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
        CHECK (supports (juce::AudioChannelSet::mono(), juce::AudioChannelSet::mono()));
        CHECK_FALSE (supports (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::mono()));
    }

    SECTION ("with everything off the vocal passes through untouched (stereo)")
    {
        makeTransparent (processor);
        prepare (processor, juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo());
        const auto out = run (processor, input);

        for (size_t i = 0; i < input.size(); ++i)
        {
            REQUIRE (out[0][i] == Catch::Approx (input[i]).margin (1.0e-6));
            REQUIRE (out[1][i] == Catch::Approx (input[i]).margin (1.0e-6));
        }
    }

    SECTION ("with everything off a mono vocal comes out centred and untouched")
    {
        makeTransparent (processor);
        prepare (processor, juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo());
        const auto out = run (processor, input);

        for (size_t i = 0; i < input.size(); ++i)
        {
            REQUIRE (out[0][i] == Catch::Approx (input[i]).margin (1.0e-6));
            REQUIRE (out[1][i] == Catch::Approx (input[i]).margin (1.0e-6));
        }
    }

    SECTION ("host blocks bigger than the internal chunk size are handled")
    {
        makeTransparent (processor);
        prepare (processor, juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo(), 2000);
        const auto out = run (processor, input, 2000);

        for (size_t i = 0; i < input.size(); ++i)
            REQUIRE (out[0][i] == Catch::Approx (input[i]).margin (1.0e-6));
    }

    SECTION ("the stack makes a mono vocal wide and bigger")
    {
        // noise rather than a pure tone: a tone and its detuned copies beat slowly, which hides the level change
        const auto voice = noise (static_cast<int> (testRate), 0.2f);

        setParam (processor, ParamIDs::echo, 0.0f);
        setParam (processor, ParamIDs::space, 0.0f);
        prepare (processor, juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo());
        const auto out = run (processor, voice);

        const auto half = voice.size() / 2;
        float difference = 0.0f;

        for (size_t i = half; i < voice.size(); ++i)
            difference = juce::jmax (difference, std::abs (out[0][i] - out[1][i]));

        CHECK (difference > 0.05f);
        CHECK (rms (out[0].data() + half, static_cast<int> (half)) > rms (voice.data() + half, static_cast<int> (half)));
    }

    SECTION ("Dirt adds grit without making the vocal much louder")
    {
        const auto voice = noise (static_cast<int> (testRate), 0.1f);
        makeTransparent (processor);
        prepare (processor, juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo());
        const auto clean = run (processor, voice);

        setParam (processor, ParamIDs::dirt, 1.0f);
        prepare (processor, juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo());
        const auto dirty = run (processor, voice);

        const auto change = juce::Decibels::gainToDecibels (rms (dirty[0].data(), static_cast<int> (voice.size())) / rms (clean[0].data(), static_cast<int> (voice.size())));
        CHECK (std::abs (change) < 6.0f);
    }

    SECTION ("Stack On off passes the vocal through")
    {
        setParam (processor, ParamIDs::active, 0.0f);
        prepare (processor, juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo());
        const auto out = run (processor, input);

        for (size_t i = 0; i < input.size(); ++i)
        {
            REQUIRE (out[0][i] == Catch::Approx (input[i]).margin (1.0e-6));
            REQUIRE (out[1][i] == Catch::Approx (input[i]).margin (1.0e-6));
        }
    }

    SECTION ("mono in, mono out works")
    {
        prepare (processor, juce::AudioChannelSet::mono(), juce::AudioChannelSet::mono());
        const auto out = run (processor, input);

        REQUIRE (out.size() == 1);

        for (auto s : out[0])
            REQUIRE (std::isfinite (s));

        CHECK (rms (out[0].data(), static_cast<int> (out[0].size())) > 0.1f);
    }

    SECTION ("the UI's input history follows the vocal")
    {
        prepare (processor, juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo());
        run (processor, input);

        std::array<float, 8> history {};
        processor.getInputHistory (history.data(), static_cast<int> (history.size()));
        CHECK (history.back() == Catch::Approx (0.4f).margin (0.01));
    }

    SECTION ("settings are saved and restored with the project")
    {
        setParam (processor, ParamIDs::stack, 5.0f);
        setParam (processor, ParamIDs::active, 0.0f);

        juce::MemoryBlock state;
        processor.getStateInformation (state);

        PluginProcessor restored;
        restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        CHECK (juce::roundToInt (restored.apvts.getRawParameterValue (ParamIDs::stack)->load()) == 5);
        CHECK (restored.apvts.getRawParameterValue (ParamIDs::active)->load() < 0.5f);
    }

    SECTION ("bad state data is ignored")
    {
        const char junk[] = "not a plugin state";
        processor.setStateInformation (junk, sizeof (junk));
        CHECK (juce::roundToInt (processor.apvts.getRawParameterValue (ParamIDs::stack)->load()) == 3);
    }
}

TEST_CASE ("Presets", "[processor]")
{
    PluginProcessor processor;

    SECTION ("every preset has a unique name")
    {
        std::set<juce::String> names;

        for (int i = 0; i < processor.getNumPrograms(); ++i)
        {
            const auto name = processor.getProgramName (i);
            CHECK (name.isNotEmpty());
            names.insert (name);
        }

        CHECK (static_cast<int> (names.size()) == processor.getNumPrograms());
    }

    SECTION ("choosing a preset sets the knobs")
    {
        const auto wide = [&]
        {
            for (int i = 0; i < processor.getNumPrograms(); ++i)
                if (processor.getProgramName (i) == "Wide Stack")
                    return i;

            return -1;
        }();

        REQUIRE (wide >= 0);
        processor.setCurrentProgram (wide);

        CHECK (processor.getCurrentProgram() == wide);
        CHECK (juce::roundToInt (processor.apvts.getRawParameterValue (ParamIDs::stack)->load()) == 6);
        CHECK (processor.apvts.getRawParameterValue (ParamIDs::spread)->load() == Catch::Approx (1.0f));
    }

    SECTION ("every preset stays inside the parameter ranges")
    {
        for (int i = 0; i < processor.getNumPrograms(); ++i)
        {
            processor.setCurrentProgram (i);

            for (auto* parameter : processor.getParameters())
            {
                CHECK (parameter->getValue() >= 0.0f);
                CHECK (parameter->getValue() <= 1.0f);
            }
        }
    }

    SECTION ("out-of-range program numbers are ignored")
    {
        processor.setCurrentProgram (-1);
        processor.setCurrentProgram (999);
        CHECK (processor.getCurrentProgram() == 0);
    }
}
