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


TEST_CASE ("processBlock CPU cost", "[cpu]")
{
    // Stereo, 48 kHz, 512-sample blocks. A block is 10.67 ms of audio, so any time below that is real-time safe;
    // the printed ratio (time / audio time) is the CPU load a single instance would use on this machine.
    PluginProcessor plugin;
    plugin.setPlayConfigDetails (2, 2, 48000.0, 512);
    plugin.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::Random rng (1);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 512; ++i)
            buffer.setSample (ch, i, 0.3f * (rng.nextFloat() * 2.0f - 1.0f));
    juce::MidiBuffer midi;

    auto set = [&] (const char* id, float v) {
        auto* p = plugin.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    };

    BENCHMARK ("passthrough (all modules flat/off)")
    {
        plugin.processBlock (buffer, midi);
        return buffer.getSample (0, 0);
    };

    set (ParamIDs::compEnable, 1.0f);
    set (ParamIDs::satEnable, 1.0f);
    set (ParamIDs::satDrive, 12.0f);
    for (int b = 0; b < 4; ++b)
        set (ParamIDs::eqGain[b], b % 2 == 0 ? 3.0f : -3.0f);
    set (ParamIDs::eqHpEnable, 1.0f);

    BENCHMARK ("EQ (4 bands + HP) + compressor + saturation")
    {
        plugin.processBlock (buffer, midi);
        return buffer.getSample (0, 0);
    };
}
