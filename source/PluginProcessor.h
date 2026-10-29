#pragma once

#include "Effects.h"
#include "Harmony.h"
#include "PitchTracker.h"
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
    inline constexpr auto active = "active";
    inline constexpr auto duck = "duck";
    inline constexpr auto throwOn = "throw";
    inline constexpr auto harmony = "harmony";
    inline constexpr auto interval = "interval";
    inline constexpr auto key = "key";
    inline constexpr auto scale = "scale";
}

// An insert effect for an ad-lib track: the vocal passes through untouched and the stack is added on top.
class PluginProcessor : public juce::AudioProcessor
{
public:
    static constexpr int scopeSize = 256;

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

    // Message thread. Fills dest with the input's recent peak levels, oldest first (one value per ~35 ms).
    void getInputHistory (float* dest, int count) const;

    // Message thread. The loudest output sample per channel since the last call (for the meter).
    std::pair<float, float> takeOutputPeaks();

    // Message thread. How far the echo and reverb are currently ducked, as a gain from 0 to 1.
    float getDuckGain() const { return duckGainForUi.load (std::memory_order_relaxed); }

    // Message thread. What Harmony mode is hearing and playing.
    struct HarmonyState
    {
        float sungNote = -1.0f;                 // MIDI note of the voice, -1 while nothing pitched is heard
        std::array<int, Harmony::maxVoices> semitones {};
        int count = 0;
    };

    HarmonyState getHarmonyState() const;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    StackParams readStackParams() const;
    void processChunk (const float* inL, const float* inR, float* outL, float* outR, int numSamples);
    void updateHarmony (int numSamples, StackParams& params);
    void renderEffects (const float* dry, float* left, float* right, int numSamples);
    void pushScope (const float* mono, int numSamples) noexcept;

    struct RawParams
    {
        std::atomic<float>* stack = nullptr;
        std::atomic<float>* spread = nullptr;
        std::atomic<float>* octave = nullptr;
        std::atomic<float>* tone = nullptr;
        std::atomic<float>* dirt = nullptr;
        std::atomic<float>* echo = nullptr;
        std::atomic<float>* echoTime = nullptr;
        std::atomic<float>* space = nullptr;
        std::atomic<float>* gain = nullptr;
        std::atomic<float>* active = nullptr;
        std::atomic<float>* duck = nullptr;
        std::atomic<float>* throwOn = nullptr;
        std::atomic<float>* harmony = nullptr;
        std::atomic<float>* interval = nullptr;
        std::atomic<float>* key = nullptr;
        std::atomic<float>* scale = nullptr;
    };

    RawParams raw;
    StackEngine engine;
    PingPongEcho echo;
    juce::Reverb reverb;
    OnePoleLowpass toneL, toneR;
    PitchTracker pitchTracker;

    juce::SmoothedValue<float> echoMix, spaceMix, dirtAmount, outputGain, activeMix, duckDepth;

    // Throw: the send opens quickly, and the long feedback and duck bypass let go slowly so the tail rings out
    juce::SmoothedValue<float> throwSend;
    float throwTail = 0.0f, throwTailAttack = 0.0f, throwTailRelease = 0.0f;

    // ducking follows the dry vocal: fast attack, then it holds through short gaps between words before letting go
    float duckEnvelope = 0.0f, duckAttack = 0.0f, duckRelease = 0.0f;
    int duckHold = 0, duckHoldSamples = 0;

    // Harmony mode: the interval held from the last pitched moment, and the smoothed value the engine plays
    std::array<float, Harmony::maxVoices> harmonyTarget {}, harmonySemitones {};
    bool harmonyPrimed = false;
    double currentSampleRate = 44100.0;
    int currentProgram = 0;

    // scratch space for one chunk of audio, sized in prepareToPlay
    std::vector<float> reverbL, reverbR, wetL, wetR, mono, duckGains;

    // read by the UI
    std::atomic<float> outputPeakL { 0.0f }, outputPeakR { 0.0f }, duckGainForUi { 1.0f };
    std::atomic<float> sungNoteForUi { -1.0f };
    std::array<std::atomic<int>, Harmony::maxVoices> harmonySemitonesForUi {};
    std::atomic<int> harmonyCountForUi { 0 };

    // input levels for the UI's scrolling display: written by the audio thread, read by the message thread
    std::array<std::atomic<float>, scopeSize> scopePeaks {};
    std::atomic<int> scopeWrite { 0 };
    float scopePeak = 0.0f;
    int scopeCount = 0;
    int scopeStride = 1500;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
