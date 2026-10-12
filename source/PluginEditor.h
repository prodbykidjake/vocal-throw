#pragma once

#include "BinaryData.h"
#include "PluginProcessor.h"
#include "melatonin_inspector/melatonin_inspector.h"
#include "ui/DepthCurveOverlay.h"
#include "ui/ReductionGraph.h"

//==============================================================================
class PluginEditor : public juce::AudioProcessorEditor
{
public:
    explicit PluginEditor (PluginProcessor&);
    ~PluginEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& labelText, juce::Component& parent);

        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        juce::AudioProcessorValueTreeState::SliderAttachment attachment;
    };

    // This reference is provided as a quick way for your editor to
    // access the processor object that created it.
    PluginProcessor& processorRef;
    std::unique_ptr<melatonin::Inspector> inspector;
    juce::TextButton inspectButton { "Inspect the UI" };

    ReductionGraph graph;
    DepthCurveOverlay depthOverlay;

    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<std::unique_ptr<Knob>> tiltKnobs;
    juce::Label tiltLabel { {}, "TILT  (low < 500 Hz, high > 2 kHz)" };

    juce::ComboBox modeBox, stereoBox;
    juce::Label modeLabel { {}, "Mode" }, stereoLabel { {}, "Stereo" };
    juce::ToggleButton deltaButton { "Delta" }, bypassButton { "Bypass" };

    juce::AudioProcessorValueTreeState::ComboBoxAttachment modeAttachment, stereoAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment deltaAttachment, bypassAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
