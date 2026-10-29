#include <PluginProcessor.h>
#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>

// Developer tool, hidden from normal runs. Runs an audio file through Stack Buddy with a preset and writes the result:
//   STACKBUDDY_RENDER_IN=in.wav STACKBUDDY_RENDER_OUT=out.wav STACKBUDDY_PRESET="Wide Stack" ./Builds/Tests "[render]"
TEST_CASE ("Render a file", "[.][render]")
{
    const auto inPath = juce::SystemStats::getEnvironmentVariable ("STACKBUDDY_RENDER_IN", {});
    const auto outPath = juce::SystemStats::getEnvironmentVariable ("STACKBUDDY_RENDER_OUT", {});
    const auto presetName = juce::SystemStats::getEnvironmentVariable ("STACKBUDDY_PRESET", "Default");
    REQUIRE (inPath.isNotEmpty());
    REQUIRE (outPath.isNotEmpty());

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (inPath)));
    REQUIRE (reader != nullptr);

    const auto rate = reader->sampleRate;
    const auto length = static_cast<int> (reader->lengthInSamples);
    juce::AudioBuffer<float> source (static_cast<int> (reader->numChannels), length);
    REQUIRE (reader->read (&source, 0, length, 0, true, true));

    PluginProcessor processor;
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (juce::AudioChannelSet::mono());
    layout.outputBuses.add (juce::AudioChannelSet::stereo());
    REQUIRE (processor.setBusesLayout (layout));

    auto found = false;

    for (int i = 0; i < processor.getNumPrograms(); ++i)
    {
        if (processor.getProgramName (i) == presetName)
        {
            processor.setCurrentProgram (i);
            found = true;
        }
    }

    if (presetName == "Bypass")
    {
        processor.apvts.getParameter (ParamIDs::active)->setValueNotifyingHost (0.0f);
        found = true;
    }

    REQUIRE (found);

    constexpr int blockSize = 512;
    processor.setRateAndBufferSizeDetails (rate, blockSize);
    processor.prepareToPlay (rate, blockSize);

    // two seconds of extra room so the echo and reverb can ring out
    const auto total = length + static_cast<int> (rate * 2.0);
    juce::AudioBuffer<float> output (2, total);
    juce::AudioBuffer<float> block (2, blockSize);
    juce::MidiBuffer midi;

    for (int start = 0; start < total; start += blockSize)
    {
        const auto count = juce::jmin (blockSize, total - start);
        block.setSize (2, count, false, false, true);
        block.clear();

        if (start < length)
            block.copyFrom (0, 0, source, 0, start, juce::jmin (count, length - start));

        processor.processBlock (block, midi);
        output.copyFrom (0, start, block, 0, 0, count);
        output.copyFrom (1, start, block, 1, 0, count);
    }

    juce::File outFile (outPath);
    outFile.deleteFile();
    juce::WavAudioFormat wav;
    auto stream = outFile.createOutputStream();
    REQUIRE (stream != nullptr);
    std::unique_ptr<juce::OutputStream> streamPtr (stream.release());
    auto writer = wav.createWriterFor (streamPtr, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    REQUIRE (writer->writeFromAudioSampleBuffer (output, 0, total));
}
