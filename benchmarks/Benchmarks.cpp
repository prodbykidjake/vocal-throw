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
    processor.setPlayConfigDetails (0, 2, sampleRate, blockSize);
    processor.prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> clip (1, static_cast<int> (sampleRate * 5.0));

    for (int i = 0; i < clip.getNumSamples(); ++i)
        clip.setSample (0, i, 0.5f * std::sin (0.03f * static_cast<float> (i)) * std::sin (0.0002f * static_cast<float> (i)));

    processor.setSample (SampleData::fromBuffer (clip, sampleRate, "bench"));

    // every knob up, so the whole effects chain is working
    auto setParam = [&] (const char* id, float value)
    {
        auto* parameter = processor.apvts.getParameter (id);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    };
    setParam (ParamIDs::stack, 6.0f);
    setParam (ParamIDs::spread, 1.0f);
    setParam (ParamIDs::octave, 1.0f);
    setParam (ParamIDs::dirt, 0.5f);
    setParam (ParamIDs::echo, 0.5f);
    setParam (ParamIDs::space, 0.5f);

    juce::AudioBuffer<float> buffer (2, blockSize);

    // each block restarts the notes, so exactly noteCount * 7 layers are playing in it
    auto renderBlock = [&] (int noteCount)
    {
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);

        for (int n = 0; n < noteCount; ++n)
            midi.addEvent (juce::MidiMessage::noteOn (1, 36 + n, 1.0f), 0);

        buffer.clear();
        processor.processBlock (buffer, midi);
        return buffer.getSample (0, blockSize - 1);
    };

    renderBlock (1); // pick up the sample

    // one block is blockSize / sampleRate = 10.7 ms of audio, which is the time budget
    BENCHMARK ("512-sample block, 8 notes x 7 layers (56)") { return renderBlock (8); };
    BENCHMARK ("512-sample block, 32 notes x 7 layers (224)") { return renderBlock (32); };
}
