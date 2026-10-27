#pragma once

#include "SampleData.h"
#include <array>

struct StackParams
{
    int stack = 3;          // copies per note, 1 to StackEngine::maxStack
    float spread = 0.5f;    // 0 to 1: how far the copies drift apart in pitch, pan and time
    float octave = 0.0f;    // 0 to 1: level of an extra copy an octave up
    bool oneShot = true;    // true: the whole clip plays and note-off is ignored
    float releaseMs = 60.0f;
};

// How one copy in the stack differs from the original.
struct LayerShape
{
    float cents = 0.0f;
    float pan = 0.0f;       // -1 left, +1 right
    float delayMs = 0.0f;
    float gain = 1.0f;
};

// Plays a SampleData from MIDI. Each note starts a "stack" of copies of the clip.
// Everything is preallocated, so noteOn/noteOff/render are safe on the audio thread.
class StackEngine
{
public:
    static constexpr int maxStack = 6;
    static constexpr int maxLayers = 256;
    static constexpr int rootNote = 60; // this key plays the clip at its original pitch

    void prepare (double sampleRate);

    // The sample must outlive its use by the engine. Silences everything that was playing.
    void setSample (const SampleData* newSample);

    void noteOn (int note, float velocity, const StackParams& params);
    void noteOff (int note);
    void allNotesOff();

    // Adds into left/right.
    void render (float* left, float* right, int numSamples);

    int getActiveLayerCount() const noexcept;

    // Copy `index` of `count`. Copy 0 is always the untouched original.
    static LayerShape shapeFor (int index, int count, float spread);

private:
    struct Layer
    {
        bool active = false;
        bool releasing = false;
        bool oneShot = true;
        int note = -1;
        int delaySamples = 0;
        uint64_t age = 0;
        double position = 0.0;
        double ratio = 1.0;
        float gainL = 0.0f;
        float gainR = 0.0f;
        float attack = 0.0f;
        float release = 1.0f;
        float releaseStep = 0.0f;
    };

    Layer* claimLayer();
    void startLayer (int note, double semitoneOffset, const LayerShape& shape, float level, const StackParams& params);
    static float readInterpolated (const std::vector<float>& data, double position) noexcept;

    std::array<Layer, maxLayers> layers;
    const SampleData* sample = nullptr;
    double sampleRate = 44100.0;
    uint64_t ageCounter = 0;
};
