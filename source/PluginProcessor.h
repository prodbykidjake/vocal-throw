#pragma once

#include "analysis/CaptureFifo.h"
#include "analysis/SignalAnalyzer.h"
#include "dsp/Meters.h"
#include "engine/Chain.h"
#include "params/Parameters.h"
#include <juce_audio_processors/juce_audio_processors.h>

class PluginProcessor : public juce::AudioProcessor
{
public:
    PluginProcessor();
    ~PluginProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock; // keep the double overload visible (it is rejected by supportsDoublePrecisionProcessing)

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

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    // Undo history for every parameter change made from the UI, presets and recommendations.
    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState apvts;

    // --- Metering and analysis taps (written by the audio thread, read lock-free by the UI) ---
    LevelMeter inputMeter, outputMeter;
    LoudnessMeter inputLoudness, outputLoudness;
    CaptureFifo captureFifo;
    SignalAnalyzer analyzer; // message thread only
    // Moves audio captured by the audio thread into the analyzer's history (message thread).
    void pumpAnalysis() { analyzer.drain (captureFifo, getSampleRate()); }
    float getGainReductionDb() const { return chain.getGainReductionDb(); }
    // Output loudness minus input loudness (dB) as measured while the chain is active; this is the
    // offset applied to the original signal when "Match Loudness On Bypass" is on.
    float getLoudnessOffsetDb() const { return loudnessOffsetDb.load (std::memory_order_relaxed); }

    // --- Non-automatable UI/session state, stored in the same saved state ---
    enum class Mode { guided, automatic, creative };
    static constexpr auto mode_ = "mode";
    static constexpr auto goal_ = "goal";
    static constexpr auto intensity_ = "intensity";
    static constexpr auto source_ = "source";
    static constexpr auto autoApply_ = "autoApply";
    static constexpr auto presetName_ = "presetName";

    // A/B comparison: two full parameter snapshots; the active slot is the live one.
    int getActiveSlot() const { return (int) apvts.state.getProperty ("abSlot", 0); }
    void switchABSlot();

    // Reads every audio parameter into a name -> value map / writes a map back (one undo step).
    std::map<std::string, float> getParamValues() const;
    void setParamValues (const std::map<std::string, float>& values, const juce::String& undoName);

private:
    void processChunk (juce::AudioBuffer<float>& buffer, int startSample, int numSamples, int numChannels);
    ChainParams readParams() const;

    // Raw parameter pointers resolved once in the constructor so the audio thread never does a lookup.
    struct Handles
    {
        using P = std::atomic<float>*;
        P inputGain, outputGain, bypass, loudnessMatch;
        P eqEnable, eqHpEnable, eqHpFreq;
        std::array<P, 4> eqFreq, eqGain, eqQ;
        P compEnable, compThreshold, compRatio, compAttack, compRelease, compKnee, compMakeup, compMix;
        P satEnable, satDrive, satMix;
    } h {};

    juce::AudioParameterBool* bypassParam = nullptr;
    std::map<std::string, std::atomic<float>*> rawParams; // message-thread use only (presets, A/B)

    Chain chain;

    // Crossfade between processed and original so bypass never clicks. 1 = processed, 0 = bypassed.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetMix { 1.0f };
    // Gain applied to the original signal so it can be compared at the processed loudness.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> matchGain { 1.0f };
    std::atomic<float> loudnessOffsetDb { 0.0f };
    juce::AudioBuffer<float> dryBuffer; // allocated in prepareToPlay, never in processBlock
    int maxChunkSize = 0;
    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
