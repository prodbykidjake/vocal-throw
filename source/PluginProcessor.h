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

    // Declared before the state so the state can use it. Undo/redo of parameter changes.
    juce::UndoManager undoManager;

    juce::AudioProcessorValueTreeState apvts { *this, &undoManager, "VoxSmoove", createParameterLayout() };

    // A/B: two complete parameter states. Only the inactive slot is stored; the active one is the live state.
    /** Editor size as a fraction of the design's 1480 px width. Saved with the session. */
    float getUiScale() const noexcept { return uiScale; }
    void setUiScale (float scale) noexcept { uiScale = juce::jlimit (0.7f, 1.0f, scale); }

    int getActiveSlot() const noexcept { return activeSlot; }
    void switchToSlot (int slot);
    void copyActiveToOtherSlot();

    /** Latest gain reduction at ResonanceSuppressor::numDisplayPoints log-spaced frequencies, for the editor. */
    void getReductionDisplay (float* dest) const noexcept { suppressor.getReductionDisplay (dest); }

    /** Peak of the sidechain key over the last block (0 if there is none). */
    float getKeyLevel() const noexcept { return suppressor.getKeyLevel(); }

    /** True if the host has the sidechain input enabled. */
    bool isKeyConnected() const;

private:
    vs::SuppressorParams readParams() const noexcept;

    int currentProgram = 0;
    float uiScale = 0.85f;
    int activeSlot = 0;
    std::array<juce::ValueTree, 2> abState;

    // raw parameter pointers for the depth curve bands, so the audio thread never looks up by name
    std::array<std::array<std::atomic<float>*, (size_t) ParamIDs::numBandParams>, ParamIDs::numBands> bandValues {};

    vs::ResonanceSuppressor suppressor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
