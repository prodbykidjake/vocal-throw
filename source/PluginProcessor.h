#pragma once

#include "Effects.h"
#include "SampleData.h"
#include "StackEngine.h"
#include <juce_audio_processors/juce_audio_processors.h>

#if (MSVC)
#include "ipps.h"
#endif

namespace ParamIDs
{
    inline constexpr auto stack = "stack";
    inline constexpr auto spread = "spread";
    inline constexpr auto octave = "octave";
    inline constexpr auto tone = "tone";
    inline constexpr auto dirt = "dirt";
    inline constexpr auto echo = "echo";
    inline constexpr auto echoTime = "echoTime";
    inline constexpr auto space = "space";
    inline constexpr auto gain = "gain";
    inline constexpr auto oneShot = "oneShot";
}

class PluginProcessor : public juce::AudioProcessor
{
public:
    PluginProcessor();
    ~PluginProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    using AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Message thread only. Returns false and fills errorMessage if the file can't be used.
    bool loadSampleFromFile (const juce::File& file, juce::String& errorMessage);
    void setSample (std::shared_ptr<const SampleData> newSample);
    std::shared_ptr<const SampleData> getSample() const;

    juce::AudioProcessorValueTreeState apvts;
    juce::MidiKeyboardState keyboardState; // lets the editor's keyboard play the plugin

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    StackParams readStackParams() const;
    void renderEffects (float* left, float* right, int numSamples);

    StackEngine engine;
    PingPongEcho echo;
    juce::Reverb reverb;
    OnePoleLowpass toneL, toneR;

    juce::SmoothedValue<float> echoMix, spaceMix, dirtAmount, outputGain;
    double currentSampleRate = 44100.0;

    // The message thread hands finished samples to the audio thread through `pendingSample`.
    // `recentSamples` keeps the last few alive so the audio thread never frees one.
    mutable juce::CriticalSection sampleLock;
    std::shared_ptr<const SampleData> displaySample;
    std::array<std::shared_ptr<const SampleData>, 4> recentSamples;
    size_t recentIndex = 0;
    std::atomic<const SampleData*> pendingSample { nullptr };

    juce::AudioFormatManager formatManager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
