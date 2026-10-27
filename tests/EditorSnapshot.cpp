#include <PluginEditor.h>
#include <PluginProcessor.h>
#include <catch2/catch_test_macros.hpp>

// Developer tool, hidden from normal runs. Writes PNGs of the editor so the look can be checked without a DAW:
//   STACKBUDDY_SNAPSHOT_DIR=/some/folder ./Builds/Tests "[snapshot]"
TEST_CASE ("Editor snapshot", "[.][snapshot]")
{
    const auto folder = juce::SystemStats::getEnvironmentVariable ("STACKBUDDY_SNAPSHOT_DIR", {});
    REQUIRE (folder.isNotEmpty());

    juce::ScopedJuceInitialiser_GUI gui;

    PluginProcessor processor;

    auto save = [&] (const juce::String& name, int width)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
        editor->setSize (width, juce::roundToInt (width * 700.0 / 1180.0));

        // let the timers and layout settle
        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);

        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        juce::FileOutputStream out (juce::File (folder).getChildFile (name));
        REQUIRE (out.openedOk());
        out.setPosition (0);
        out.truncate();
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, out));
    };

    save ("empty.png", 1180);

    // something shaped like a spoken "yeah": a few syllable-sized bursts
    constexpr double rate = 44100.0;
    juce::AudioBuffer<float> clip (1, static_cast<int> (rate * 0.84));
    juce::Random random (7);

    for (int i = 0; i < clip.getNumSamples(); ++i)
    {
        const auto t = static_cast<double> (i) / clip.getNumSamples();
        const auto envelope = std::pow (std::sin (juce::MathConstants<double>::pi * t), 0.6)
                              * (0.55 + 0.45 * std::sin (t * 41.0) * std::sin (t * 17.0));
        clip.setSample (0, i, static_cast<float> (envelope * (0.6 * std::sin (i * 0.05) + 0.4 * (random.nextFloat() - 0.5))));
    }

    processor.setSample (SampleData::fromBuffer (clip, rate, "yeah_02"));
    processor.keyboardState.noteOn (1, 62, 1.0f);
    processor.keyboardState.noteOn (1, 64, 1.0f);

    save ("loaded.png", 1180);
    save ("small.png", 800);
}
