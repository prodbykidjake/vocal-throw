#include <PluginProcessor.h>
#include <StackEngine.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
    constexpr double testRate = 44100.0;

    std::shared_ptr<SampleData> makeSine (double seconds = 0.5, double frequency = 220.0)
    {
        juce::AudioBuffer<float> buffer (1, static_cast<int> (seconds * testRate));

        for (int i = 0; i < buffer.getNumSamples(); ++i)
            buffer.setSample (0, i, static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * frequency * i / testRate)));

        return SampleData::fromBuffer (buffer, testRate, "sine");
    }

    // Renders until nothing is playing and returns how many samples that took.
    int samplesUntilSilent (StackEngine& engine, int limit = 200000)
    {
        std::vector<float> left (512), right (512);
        int total = 0;

        while (engine.getActiveLayerCount() > 0 && total < limit)
        {
            std::fill (left.begin(), left.end(), 0.0f);
            std::fill (right.begin(), right.end(), 0.0f);
            engine.render (left.data(), right.data(), 512);
            total += 512;
        }

        return total;
    }
}

TEST_CASE ("Stack layout", "[engine]")
{
    SECTION ("the first copy is the untouched original")
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

    SECTION ("no spread means no difference between copies")
    {
        for (int i = 0; i < 4; ++i)
        {
            const auto shape = StackEngine::shapeFor (i, 4, 0.0f);
            CHECK (shape.cents == Catch::Approx (0.0f));
            CHECK (shape.pan == Catch::Approx (0.0f));
            CHECK (shape.delayMs == Catch::Approx (0.0f));
        }
    }
}

TEST_CASE ("Engine playback", "[engine]")
{
    StackEngine engine;
    engine.prepare (testRate);
    auto sample = makeSine();
    StackParams params;

    SECTION ("does nothing without a sample")
    {
        engine.noteOn (60, 1.0f, params);
        CHECK (engine.getActiveLayerCount() == 0);
    }

    engine.setSample (sample.get());

    SECTION ("one note starts one layer per stack copy")
    {
        params.stack = 4;
        engine.noteOn (60, 1.0f, params);
        CHECK (engine.getActiveLayerCount() == 4);
    }

    SECTION ("the octave copy adds one more layer")
    {
        params.stack = 2;
        params.octave = 0.5f;
        engine.noteOn (60, 1.0f, params);
        CHECK (engine.getActiveLayerCount() == 3);
    }

    SECTION ("an octave up plays in half the time")
    {
        params.stack = 1;
        engine.noteOn (60, 1.0f, params);
        const auto original = samplesUntilSilent (engine);

        engine.noteOn (72, 1.0f, params);
        const auto octaveUp = samplesUntilSilent (engine);

        CHECK (octaveUp == Catch::Approx (original / 2.0).margin (1024));
    }

    SECTION ("output is finite, stays in range and ends")
    {
        params.stack = StackEngine::maxStack;
        params.octave = 1.0f;
        params.spread = 1.0f;
        engine.noteOn (60, 1.0f, params);

        std::vector<float> left (512), right (512);
        float peak = 0.0f;
        int blocks = 0;

        while (engine.getActiveLayerCount() > 0 && blocks < 1000)
        {
            std::fill (left.begin(), left.end(), 0.0f);
            std::fill (right.begin(), right.end(), 0.0f);
            engine.render (left.data(), right.data(), 512);

            for (size_t i = 0; i < 512; ++i)
            {
                REQUIRE (std::isfinite (left[i]));
                REQUIRE (std::isfinite (right[i]));
                peak = juce::jmax (peak, std::abs (left[i]), std::abs (right[i]));
            }

            ++blocks;
        }

        CHECK (peak > 0.1f);
        CHECK (peak < 2.5f);
        CHECK (engine.getActiveLayerCount() == 0);
    }

    SECTION ("one-shot ignores note-off")
    {
        params.oneShot = true;
        engine.noteOn (60, 1.0f, params);
        engine.noteOff (60);

        std::vector<float> left (4096), right (4096);
        engine.render (left.data(), right.data(), 4096);
        CHECK (engine.getActiveLayerCount() > 0);
    }

    SECTION ("hold mode releases on note-off")
    {
        params.oneShot = false;
        params.stack = 1;
        params.releaseMs = 20.0f;
        engine.noteOn (60, 1.0f, params);
        engine.noteOff (60);

        // 20 ms is 882 samples
        CHECK (samplesUntilSilent (engine) <= 1024);
    }

    SECTION ("a higher velocity is louder")
    {
        params.stack = 1;
        auto peakFor = [&] (float velocity)
        {
            engine.allNotesOff();
            engine.noteOn (60, velocity, params);
            std::vector<float> left (4096), right (4096);
            engine.render (left.data(), right.data(), 4096);
            return *std::max_element (left.begin(), left.end());
        };

        CHECK (peakFor (1.0f) > peakFor (0.3f));
    }
}

TEST_CASE ("Sample data", "[sample]")
{
    SECTION ("silence at both ends is trimmed and the peak is normalised")
    {
        juce::AudioBuffer<float> buffer (2, 20000);
        buffer.clear();

        for (int i = 5000; i < 10000; ++i)
        {
            buffer.setSample (0, i, 0.25f * std::sin (0.05f * static_cast<float> (i)));
            buffer.setSample (1, i, 0.25f * std::sin (0.05f * static_cast<float> (i)));
        }

        auto sample = SampleData::fromBuffer (buffer, testRate, "test");
        REQUIRE (sample != nullptr);
        CHECK (sample->size() < 5200);
        CHECK (sample->size() > 4900);

        float peak = 0.0f;

        for (auto s : sample->samples)
            peak = juce::jmax (peak, std::abs (s));

        CHECK (peak == Catch::Approx (0.9f).margin (0.001));
    }

    SECTION ("silent audio is rejected")
    {
        juce::AudioBuffer<float> buffer (1, 1000);
        buffer.clear();
        CHECK (SampleData::fromBuffer (buffer, testRate, "silent") == nullptr);
    }

    SECTION ("clips longer than the cap are cut")
    {
        juce::AudioBuffer<float> buffer (1, static_cast<int> (testRate * 12.0));

        for (int i = 0; i < buffer.getNumSamples(); ++i)
            buffer.setSample (0, i, 0.5f);

        auto sample = SampleData::fromBuffer (buffer, testRate, "long");
        REQUIRE (sample != nullptr);
        CHECK (sample->size() <= static_cast<int> (SampleData::maxSeconds * testRate));
    }

    SECTION ("base64 round trip keeps the audio")
    {
        auto original = makeSine (0.1);
        auto restored = SampleData::fromBase64 (original->toBase64(), original->sampleRate, original->name);
        REQUIRE (restored != nullptr);
        REQUIRE (restored->size() == original->size());

        for (int i = 0; i < original->size(); ++i)
            REQUIRE (restored->samples[static_cast<size_t> (i)] == Catch::Approx (original->samples[static_cast<size_t> (i)]).margin (0.0001));
    }

    SECTION ("garbage base64 is rejected")
    {
        CHECK (SampleData::fromBase64 ("", testRate, "x") == nullptr);
    }
}

TEST_CASE ("Processor", "[processor]")
{
    PluginProcessor processor;
    processor.setPlayConfigDetails (0, 2, testRate, 512);
    processor.prepareToPlay (testRate, 512);

    auto renderBlock = [&] (juce::MidiBuffer midi)
    {
        juce::AudioBuffer<float> buffer (2, 512);
        buffer.clear();
        processor.processBlock (buffer, midi);
        return buffer;
    };

    SECTION ("is silent until a note arrives")
    {
        processor.setSample (makeSine());
        CHECK (renderBlock ({}).getMagnitude (0, 0, 512) == Catch::Approx (0.0f));
    }

    SECTION ("a note plays the clip")
    {
        processor.setSample (makeSine());

        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        renderBlock (midi);

        // second block picks up the sample and keeps playing
        const auto buffer = renderBlock ({});
        CHECK (buffer.getMagnitude (0, 0, 512) > 0.05f);
        CHECK (buffer.getMagnitude (1, 0, 512) > 0.05f);
    }

    SECTION ("a note starts exactly where its MIDI event is")
    {
        processor.setSample (makeSine());
        renderBlock ({}); // pick up the sample

        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 300);
        const auto buffer = renderBlock (midi);

        CHECK (buffer.getMagnitude (0, 0, 300) == Catch::Approx (0.0f));
        CHECK (buffer.getMagnitude (0, 300, 212) > 0.0f);
    }

    SECTION ("the clip is saved and restored with the project")
    {
        auto sample = makeSine();
        processor.setSample (sample);
        processor.apvts.getParameter (ParamIDs::stack)->setValueNotifyingHost (processor.apvts.getParameter (ParamIDs::stack)->convertTo0to1 (5.0f));

        juce::MemoryBlock state;
        processor.getStateInformation (state);

        PluginProcessor restored;
        restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));

        auto restoredSample = restored.getSample();
        REQUIRE (restoredSample != nullptr);
        CHECK (restoredSample->size() == sample->size());
        CHECK (restoredSample->sampleRate == Catch::Approx (sample->sampleRate));
        CHECK (juce::roundToInt (restored.apvts.getRawParameterValue (ParamIDs::stack)->load()) == 5);
    }

    SECTION ("bad state data is ignored")
    {
        const char junk[] = "not a plugin state";
        processor.setStateInformation (junk, sizeof (junk));
        CHECK (processor.getSample() == nullptr);
    }
}
