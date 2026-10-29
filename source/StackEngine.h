#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>

struct StackParams
{
    int stack = 3;          // voices including the original, 1 to StackEngine::maxStack
    float spread = 0.5f;    // 0 to 1: how far the copies drift apart in pitch, pan and time
    float octave = 0.0f;    // 0 to 1: level of an extra copy an octave up
    float harmony = 0.0f;   // 0 to 1: level of the harmony copies
    std::array<float, 2> harmonySemitones {}; // pitch of each harmony copy relative to the voice
    int harmonyCount = 0;   // how many harmony copies, 0 to StackEngine::maxHarmonies
};

// How one copy in the stack differs from the original.
struct LayerShape
{
    float cents = 0.0f;
    float pan = 0.0f;       // -1 left, +1 right
    float delayMs = 0.0f;
    float gain = 1.0f;
};

// Turns a live vocal into a stack of copies, as an insert effect.
// Each copy is the input re-pitched by a few cents (or an octave), delayed a little and panned.
// The original isn't touched here: process() adds the copies on top of whatever is already in left/right.
// Everything is preallocated in prepare(), so process() is safe on the audio thread.
class StackEngine
{
public:
    static constexpr int maxStack = 6;
    static constexpr int maxCopies = maxStack - 1;
    static constexpr int maxHarmonies = 2;

    void prepare (double sampleRate);
    void reset();

    // Call once per block before process().
    void setParams (const StackParams& params);

    void process (const float* monoInput, float* left, float* right, int numSamples);

    // Copy `index` of `count` voices. Voice 0 is always the untouched original.
    static LayerShape shapeFor (int index, int count, float spread);

    // How loud each copy is, so a bigger stack sounds bigger without jumping in level.
    static float copyGain (int count);

private:
    // Re-pitches by reading the delay line faster or slower than it's written. When the read head runs out of
    // room it crossfades to a new position, chosen where the audio lines up best with what's playing, so the
    // join doesn't cancel or warble.
    struct Voice
    {
        double ratio = 1.0;
        double delayA = 0.0, delayB = 0.0;   // playing tap, and the tap being faded in
        float minDelay = 0.0f, maxDelay = 0.0f, fadeLength = 1.0f, searchRange = 0.0f;
        float fade = 0.0f;
        bool fading = false;
        bool started = false;
        float offset = 0.0f;                 // extra delay in samples, smoothed
        float targetOffset = 0.0f;
        float gainL = 0.0f, gainR = 0.0f;
        float targetGainL = 0.0f, targetGainR = 0.0f;
    };

    float readDelayed (double delaySamples) const noexcept;
    double findJump (const Voice& voice) const noexcept;
    float processVoice (Voice& voice) noexcept;
    void setVoice (Voice& voice, double ratio, float windowMs, float delayMs, float pan, float gain);

    std::vector<float> buffer;
    int mask = 0;
    int writeIndex = 0;
    double sampleRate = 44100.0;
    float smoothing = 0.001f;

    std::array<Voice, maxCopies> copies;
    Voice octaveVoice;
    std::array<Voice, maxHarmonies> harmonyVoices;
    std::array<float, 1025> fadeCurve {};
};
