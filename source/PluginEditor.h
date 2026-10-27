#pragma once

#include "DesignLookAndFeel.h"
#include "PluginProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>

// Shows the loaded clip as bars, and accepts a dropped audio file or a click to browse.
class WaveformView : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    std::function<void (const juce::File&)> onFileChosen;

    void setSample (std::shared_ptr<const SampleData> newSample);
    void setMessage (const juce::String& newMessage);
    void openChooser();

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    std::shared_ptr<const SampleData> sample;
    juce::String message;
    bool dragging = false;
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
// The editor draws everything at the design's 1180 x 700 and scales it to the window.
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
