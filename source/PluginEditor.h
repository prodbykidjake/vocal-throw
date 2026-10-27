#pragma once

#include "PluginProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>

// Shows the loaded clip, and accepts a dropped audio file or a click to browse.
class WaveformView : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    std::function<void (const juce::File&)> onFileChosen;

    void setSample (std::shared_ptr<const SampleData> newSample);
    void setMessage (const juce::String& newMessage);

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

class StackLookAndFeel : public juce::LookAndFeel_V4
{
public:
    StackLookAndFeel();
};

//==============================================================================
class PluginEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit PluginEditor (PluginProcessor&);
    ~PluginEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    void timerCallback() override;
    void addKnob (Knob& knob, const juce::String& name, const char* parameterId);
    void loadFile (const juce::File& file);

    PluginProcessor& processorRef;
    StackLookAndFeel lookAndFeel;

    WaveformView waveform;
    Knob stackKnob, spreadKnob, octaveKnob, toneKnob, dirtKnob, echoKnob, spaceKnob, gainKnob;

    juce::ComboBox echoTimeBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> echoTimeAttachment;
    juce::Label echoTimeLabel { {}, "Echo time" };
    juce::ToggleButton oneShotButton { "One-Shot" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> oneShotAttachment;
    juce::Label hintLabel { {}, "C3 plays the clip at its original pitch" };

    juce::MidiKeyboardComponent keyboard;
    std::shared_ptr<const SampleData> shownSample;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
