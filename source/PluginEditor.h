#pragma once

#include "PluginProcessor.h"

//==============================================================================
// Phase 2 shell: real controls bound to real parameters. The full SignalPath UI comes later.
class PluginEditor : public juce::AudioProcessorEditor
{
public:
    explicit PluginEditor (PluginProcessor&);
    ~PluginEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    PluginProcessor& processorRef;

    juce::Slider inputSlider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider outputSlider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label inputLabel { {}, "Input" };
    juce::Label outputLabel { {}, "Output" };
    juce::ToggleButton bypassButton { "Bypass" };

    juce::AudioProcessorValueTreeState::SliderAttachment inputAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment outputAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
