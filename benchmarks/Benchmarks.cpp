TEST_CASE ("Boot performance")
{
    BENCHMARK_ADVANCED ("Processor constructor")
    (Catch::Benchmark::Chronometer meter)
    {
        std::vector<Catch::Benchmark::storage_for<PluginProcessor>> storage (size_t (meter.runs()));
        meter.measure ([&] (int i) { storage[(size_t) i].construct(); });
    };

    BENCHMARK_ADVANCED ("Processor destructor")
    (Catch::Benchmark::Chronometer meter)
    {
        std::vector<Catch::Benchmark::destructable_object<PluginProcessor>> storage (size_t (meter.runs()));
        for (auto& s : storage)
            s.construct();
        meter.measure ([&] (int i) { storage[(size_t) i].destruct(); });
    };

    BENCHMARK_ADVANCED ("Editor open and close")
    (Catch::Benchmark::Chronometer meter)
    {
        PluginProcessor plugin;

        // due to complex construction logic of the editor, let's measure open/close together
        meter.measure ([&] (int /* i */) {
            auto editor = plugin.createEditorAndMakeActive();
            plugin.editorBeingDeleted (editor);
            delete editor;
            return plugin.getActiveEditor();
        });
    };
}

TEST_CASE ("Render performance")
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    PluginProcessor processor;
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (juce::AudioChannelSet::mono());
    layout.outputBuses.add (juce::AudioChannelSet::stereo());
    processor.setBusesLayout (layout);
    processor.setRateAndBufferSizeDetails (sampleRate, blockSize);
    processor.prepareToPlay (sampleRate, blockSize);

    // every knob up, so the whole stack and effects chain is working
    auto setParam = [&] (const char* id, float value)
    {
        auto* parameter = processor.apvts.getParameter (id);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    };
    setParam (ParamIDs::stack, 6.0f);
    setParam (ParamIDs::spread, 1.0f);
    setParam (ParamIDs::octave, 1.0f);
    setParam (ParamIDs::tone, 0.6f);
    setParam (ParamIDs::dirt, 0.5f);
    setParam (ParamIDs::echo, 0.5f);
    setParam (ParamIDs::space, 0.5f);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    float phase = 0.0f;

    // one block is blockSize / sampleRate = 10.7 ms of audio, which is the time budget
    BENCHMARK ("512-sample block, 6 voices + octave, all effects")
    {
        for (int i = 0; i < blockSize; ++i)
        {
            buffer.setSample (0, i, 0.4f * std::sin (phase));
            phase += 0.05f;
        }

        processor.processBlock (buffer, midi);
        return buffer.getSample (0, blockSize - 1);
    };
}
