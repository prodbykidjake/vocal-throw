#include "PluginEditor.h"

PluginEditor::PluginEditor (PluginProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
      inputAttachment (p.apvts, ParamIDs::inputGain, inputSlider),
      outputAttachment (p.apvts, ParamIDs::outputGain, outputSlider),
      bypassAttachment (p.apvts, ParamIDs::bypass, bypassButton)
{
    for (auto* s : { &inputSlider, &outputSlider })
    {
        s->setTextValueSuffix (" dB");
        s->setDoubleClickReturnValue (true, 0.0);
        addAndMakeVisible (s);
    }

    for (auto* l : { &inputLabel, &outputLabel })
    {
        l->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (l);
    }

    addAndMakeVisible (bypassButton);
    setSize (360, 240);
}

PluginEditor::~PluginEditor() = default;

void PluginEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff101216));
    g.setColour (juce::Colour (0xff3ad7c4));
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText ("SignalPath", getLocalBounds().removeFromTop (40).reduced (12, 0), juce::Justification::centredLeft);
}

void PluginEditor::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop (40);
    bypassButton.setBounds (area.removeFromBottom (36).reduced (12, 4));

    auto left = area.removeFromLeft (area.getWidth() / 2).reduced (8);
    auto right = area.reduced (8);
    inputLabel.setBounds (left.removeFromTop (22));
    inputSlider.setBounds (left);
    outputLabel.setBounds (right.removeFromTop (22));
    outputSlider.setBounds (right);
}
