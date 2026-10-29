#pragma once

#include "DesignLookAndFeel.h"
#include "PluginProcessor.h"

// The live vocal coming into the plugin, drawn as scrolling bars (newest on the right),
// with the stack behind it as ghost copies of the bars.
class InputScope : public juce::Component
{
public:
    static constexpr int maxBars = PluginProcessor::scopeSize;

    // How many bars fit, so the caller knows how much history to fetch.
    int getBarCount() const;
    void setLevels (const float* levels, int count);

    // What the ghost layers show: the extra voices, the octave copy and the harmony copies.
    void setStack (int voices, float spread, float octave, float harmony, int harmonyCount);

    void paint (juce::Graphics&) override;

private:
    void paintLayer (juce::Graphics&, float dx, float heightScale, juce::Colour, bool filled) const;

    std::array<float, maxBars> levels {};
    int numLevels = 0;
    int stackVoices = 1;
    float stackSpread = 0.0f, octaveLevel = 0.0f, harmonyLevel = 0.0f;
    int harmonyVoices = 0;
};

//==============================================================================
// The editor draws everything at the design's 1180 x 690 and scales it to the window.
class PluginEditor : public juce::AudioProcessorEditor
{
public:
    explicit PluginEditor (PluginProcessor&);
    ~PluginEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Content;

    DesignLookAndFeel lookAndFeel;
    juce::ComponentBoundsConstrainer constrainer;
    std::unique_ptr<Content> content;
    juce::ResizableCornerComponent resizer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
