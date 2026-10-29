#pragma once

#include "DesignLookAndFeel.h"
#include "PluginProcessor.h"

// The live vocal coming into the plugin, drawn as scrolling bars (newest on the right).
class InputScope : public juce::Component
{
public:
    static constexpr int maxBars = PluginProcessor::scopeSize;

    // How many bars fit, so the caller knows how much history to fetch.
    int getBarCount() const;
    void setLevels (const float* levels, int count);

    void paint (juce::Graphics&) override;

private:
    std::array<float, maxBars> levels {};
    int numLevels = 0;
};

//==============================================================================
// The editor draws everything at the design's 1180 x 570 and scales it to the window.
class PluginEditor : public juce::AudioProcessorEditor
{
public:
    explicit PluginEditor (PluginProcessor&);
    ~PluginEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Content;

    PluginProcessor& processorRef;
    DesignLookAndFeel lookAndFeel;
    juce::ComponentBoundsConstrainer constrainer;
    std::unique_ptr<Content> content;
    juce::ResizableCornerComponent resizer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
