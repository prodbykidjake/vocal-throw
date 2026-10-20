#include "helpers/dsp_test_utils.h"
#include <PluginEditor.h>
#include <catch2/catch_test_macros.hpp>

// Renders the real editor to PNGs (in $SIGNALPATH_SNAPSHOT_DIR if set) and checks it builds and paints.
// Run with SIGNALPATH_SNAPSHOT_DIR=/some/dir ./Tests "[ui]" to look at the result.

namespace
{
juce::AudioBuffer<float> resonantSignal (int seconds, double toneHz, float toneAmp)
{
    constexpr double fs = 48000.0;
    juce::Random rng (99);
    const int n = (int) fs * seconds;
    juce::AudioBuffer<float> b (2, n);
    for (int ch = 0; ch < 2; ++ch)
    {
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (int i = 0; i < n; ++i)
        {
            const float white = rng.nextFloat() * 2.0f - 1.0f;
            b0 = 0.99886f * b0 + white * 0.0555179f;
            b1 = 0.99332f * b1 + white * 0.0750759f;
            b2 = 0.96900f * b2 + white * 0.1538520f;
            b3 = 0.86650f * b3 + white * 0.3104856f;
            b4 = 0.55000f * b4 + white * 0.5329522f;
            b5 = -0.7616f * b5 - white * 0.0168980f;
            const float pink = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f) * 0.11f;
            b6 = white * 0.115926f;
            b.setSample (ch, i, 0.25f * pink + toneAmp * (float) std::sin (2.0 * std::numbers::pi * toneHz * i / fs));
        }
    }
    return b;
}

void play (PluginProcessor& p, const juce::AudioBuffer<float>& in, PluginEditor* editor = nullptr)
{
    auto out = in;
    juce::MidiBuffer midi;
    for (int s = 0; s + 480 <= in.getNumSamples(); s += 480)
    {
        juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, s, 480);
        p.processBlock (view, midi);
        if (editor != nullptr && (s / 480) % 16 == 0)
            editor->updateLive();
    }
}

void snapshot (PluginEditor& e, const juce::String& name)
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("SIGNALPATH_SNAPSHOT_DIR", {});
    auto img = e.createComponentSnapshot (e.getLocalBounds(), true, 1.0f);
    REQUIRE (img.isValid());
    CHECK (img.getWidth() == e.getWidth());
    if (dir.isNotEmpty())
    {
        juce::File folder (dir);
        folder.createDirectory();
        auto f = folder.getChildFile (name + ".png");
        f.deleteFile();
        juce::FileOutputStream out (f);
        juce::PNGImageFormat().writeImageToStream (img, out);
    }
}
}

TEST_CASE ("Editor paints in every mode and recommendation state", "[ui]")
{
    PluginProcessor p;
    p.setPlayConfigDetails (2, 2, 48000.0, 480);
    p.prepareToPlay (48000.0, 480);
    auto editor = std::unique_ptr<PluginEditor> (static_cast<PluginEditor*> (p.createEditor()));
    editor->setSize (1380, 914);
    play (p, resonantSignal (6, 4200.0, 0.02f), editor.get());
    editor->updateLive();

    p.apvts.state.setProperty (PluginProcessor::goal_, (int) Goal::reducedHarshness, nullptr);
    p.apvts.state.setProperty (PluginProcessor::source_, (int) Source::other, nullptr);

    snapshot (*editor, "01_guided_idle");
    editor->runAnalysis();
    REQUIRE (editor->getSession().getRecommendation().outcome == Outcome::suggested);
    snapshot (*editor, "02_guided_recommendation");

    REQUIRE (editor->getSession().preview());
    play (p, resonantSignal (1, 4200.0, 0.02f), editor.get());
    snapshot (*editor, "03_guided_previewing");
    REQUIRE (editor->getSession().apply());
    snapshot (*editor, "04_guided_applied");

    p.apvts.state.setProperty (PluginProcessor::mode_, 2, nullptr);
    p.apvts.state.setProperty ("advancedOpen", true, nullptr);
    editor.reset();
    editor = std::unique_ptr<PluginEditor> (static_cast<PluginEditor*> (p.createEditor()));
    editor->setSize (1380, 914);
    editor->updateLive();
    snapshot (*editor, "05_creative_eq");

    p.apvts.state.setProperty ("selectedModule", 1, nullptr);
    editor.reset();
    editor = std::unique_ptr<PluginEditor> (static_cast<PluginEditor*> (p.createEditor()));
    editor->setSize (1380, 914);
    editor->updateLive();
    snapshot (*editor, "06_creative_compressor");

    // empty / no-change states
    PluginProcessor q;
    q.setPlayConfigDetails (2, 2, 48000.0, 480);
    q.prepareToPlay (48000.0, 480);
    auto empty = std::unique_ptr<PluginEditor> (static_cast<PluginEditor*> (q.createEditor()));
    empty->setSize (1104, 731);
    empty->runAnalysis(); // no audio captured yet
    CHECK (empty->getSession().getRecommendation().outcome == Outcome::insufficientSignal);
    snapshot (*empty, "07_no_signal_small");
}
