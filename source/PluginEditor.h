#pragma once

#include "PluginProcessor.h"
#include "engine/PresetManager.h"
#include "engine/RecommendationSession.h"
#include <memory>

//==============================================================================
// SignalPath editor. Laid out on a fixed 1380 x 914 design canvas (see ui/Theme.h) and scaled to the
// window. Guided, Automatic and Creative are views over the same panels and the same parameters.
class PluginEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit PluginEditor (PluginProcessor&);
    ~PluginEditor() override;

    void resized() override;

    // Exposed for tests: runs one UI refresh tick and the analysis action without a message loop.
    void updateLive();
    void runAnalysis();
    RecommendationSession& getSession() { return session; }

private:
    class Content;
    void timerCallback() override;

    PluginProcessor& processorRef;
    RecommendationSession session;
    PresetManager presets;
    std::unique_ptr<Content> content;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
