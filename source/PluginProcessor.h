#pragma once

#include "Parameters.h"
#include "dsp/ResonanceSuppressor.h"
#include <juce_audio_processors/juce_audio_processors.h>

class PluginProcessor : public juce::AudioProcessor
{
public:
    PluginProcessor();
    ~PluginProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    using juce::AudioProcessor::processBlock;
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

    juce::AudioProcessorValueTreeState apvts { *this, nullptr, "VoxSmoove", createParameterLayout() };

    /** Latest gain reduction at ResonanceSuppressor::numDisplayPoints log-spaced frequencies, for the editor. */
    void getReductionDisplay (float* dest) const noexcept { suppressor.getReductionDisplay (dest); }

private:
    vs::SuppressorParams readParams() const noexcept;

    // raw parameter pointers for the depth curve bands, so the audio thread never looks up by name
    std::array<std::array<std::atomic<float>*, 6>, ParamIDs::numBands> bandValues {};

    vs::ResonanceSuppressor suppressor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
