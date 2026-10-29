#include <DesignTokens.h>
#include <PluginEditor.h>
#include <PluginProcessor.h>
#include <catch2/catch_test_macros.hpp>

// Developer tool, hidden from normal runs. Writes PNGs of the editor so the look can be checked without a DAW:
//   STACKBUDDY_SNAPSHOT_DIR=/some/folder ./Builds/Tests "[snapshot]"
TEST_CASE ("Editor snapshot", "[.][snapshot]")
{
    const auto folder = juce::SystemStats::getEnvironmentVariable ("STACKBUDDY_SNAPSHOT_DIR", {});
    REQUIRE (folder.isNotEmpty());

    constexpr double rate = 44100.0;
    constexpr int blockSize = 512;

    PluginProcessor processor;
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (juce::AudioChannelSet::mono());
    layout.outputBuses.add (juce::AudioChannelSet::stereo());
    REQUIRE (processor.setBusesLayout (layout));
    processor.setRateAndBufferSizeDetails (rate, blockSize);
    processor.prepareToPlay (rate, blockSize);

    auto save = [&] (const juce::String& name, int width)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
        editor->setSize (width, juce::roundToInt (width * static_cast<double> (Design::height) / Design::width));

        // let the timers pick up the input history
        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);

        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        juce::FileOutputStream out (juce::File (folder).getChildFile (name));
        REQUIRE (out.openedOk());
        out.setPosition (0);
        out.truncate();
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, out));
    };

    save ("waiting.png", 1180);

    // about four seconds of ad-lib-shaped bursts with gaps between them
    juce::Random random (7);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    const auto totalBlocks = static_cast<int> (rate * 4.0 / blockSize);

    for (int block = 0; block < totalBlocks; ++block)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            const auto t = static_cast<double> (block * blockSize + i) / rate;
            const auto phrase = std::fmod (t, 1.1);
            const auto envelope = phrase < 0.8 ? std::pow (std::sin (juce::MathConstants<double>::pi * phrase / 0.8), 0.7) * (0.55 + 0.45 * std::sin (t * 23.0)) : 0.0;
            buffer.setSample (0, i, static_cast<float> (envelope * (0.5 * std::sin (t * 1400.0) + 0.3 * (random.nextFloat() - 0.5))));
        }

        processor.processBlock (buffer, midi);
    }

    save ("listening.png", 1180);
    save ("small.png", 800);

    // the new features switched on, with Throw held
    auto set = [&] (const char* id, float value)
    {
        auto* parameter = processor.apvts.getParameter (id);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    };

    set (ParamIDs::stack, 5.0f);
    set (ParamIDs::octave, 0.5f);
    set (ParamIDs::harmony, 0.7f);
    set (ParamIDs::interval, 2.0f);
    set (ParamIDs::key, 9.0f);
    set (ParamIDs::scale, 1.0f);
    set (ParamIDs::duck, 0.6f);
    set (ParamIDs::throwOn, 1.0f);

    for (int block = 0; block < 40; ++block)
    {
        for (int i = 0; i < blockSize; ++i)
            buffer.setSample (0, i, 0.4f * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * 220.0 * (block * blockSize + i) / rate)));

        processor.processBlock (buffer, midi);
    }

    save ("features.png", 1180);
}
