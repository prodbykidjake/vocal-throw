#include "PluginEditor.h"

namespace
{
const auto kBackground = juce::Colour (0xff1b1e24);
const auto kAccent = juce::Colour (0xff4fd1c5);
constexpr int kMargin = 16;
constexpr int kHeaderHeight = 40;
constexpr int kGraphHeight = 240;
constexpr int kKnobSize = 92;
constexpr int kKnobGap = 8;
constexpr int kNumKnobs = 9;
constexpr int kTiltKnobSize = 70;
constexpr int kTiltRowHeight = 18 + kTiltKnobSize + 20;
} // namespace

PluginEditor::Knob::Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& labelText, juce::Component& parent)
    : attachment (state, paramID, slider)
{
    slider.setColour (juce::Slider::rotarySliderFillColourId, kAccent);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 18);
    label.setText (labelText, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.attachToComponent (&slider, false);
    parent.addAndMakeVisible (slider);
    parent.addAndMakeVisible (label);
}

PluginEditor::PluginEditor (PluginProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
      graph (p),
      depthOverlay (p.apvts),
      keyMeter (p),
      modeAttachment (p.apvts, ParamIDs::mode, modeBox),
      stereoAttachment (p.apvts, ParamIDs::stereoMode, stereoBox),
      deltaAttachment (p.apvts, ParamIDs::delta, deltaButton),
      bypassAttachment (p.apvts, ParamIDs::bypass, bypassButton),
      sidechainAttachment (p.apvts, ParamIDs::sidechain, sidechainButton),
      keyListenAttachment (p.apvts, ParamIDs::keyListen, keyListenButton)
{
    addAndMakeVisible (graph);
    addAndMakeVisible (depthOverlay);

    const std::pair<const char*, const char*> knobDefs[] = {
        { ParamIDs::depth, "Depth" },
        { ParamIDs::detail, "Detail" },
        { ParamIDs::attack, "Attack" },
        { ParamIDs::release, "Release" },
        { ParamIDs::maxCut, "Max Cut" },
        { ParamIDs::link, "Link" },
        { ParamIDs::wetTrim, "Wet Trim" },
        { ParamIDs::mix, "Mix" },
        { ParamIDs::outGain, "Out Gain" },
    };
    for (const auto& [id, name] : knobDefs)
        knobs.push_back (std::make_unique<Knob> (processorRef.apvts, id, name, *this));

    const std::pair<const char*, const char*> tiltDefs[] = {
        { ParamIDs::detailLow, "Detail Lo" },
        { ParamIDs::detailHigh, "Detail Hi" },
        { ParamIDs::attackLow, "Attack Lo" },
        { ParamIDs::attackHigh, "Attack Hi" },
        { ParamIDs::releaseLow, "Release Lo" },
        { ParamIDs::releaseHigh, "Release Hi" },
    };
    for (const auto& [id, name] : tiltDefs)
        tiltKnobs.push_back (std::make_unique<Knob> (processorRef.apvts, id, name, *this));

    tiltLabel.setFont (juce::FontOptions (11.0f));
    tiltLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (tiltLabel);

    // The choice items must exist before the attachments pick a selection, so add them via the parameter
    modeBox.addItemList ({ "Soft", "Hard" }, 1);
    stereoBox.addItemList ({ "L/R", "M/S" }, 1);
    modeBox.setSelectedItemIndex ((int) processorRef.apvts.getRawParameterValue (ParamIDs::mode)->load(), juce::dontSendNotification);
    stereoBox.setSelectedItemIndex ((int) processorRef.apvts.getRawParameterValue (ParamIDs::stereoMode)->load(), juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*> { &modeBox, &stereoBox, &modeLabel, &stereoLabel, &deltaButton, &bypassButton, &sidechainButton, &keyListenButton, &keyMeter })
        addAndMakeVisible (c);

    addAndMakeVisible (inspectButton);

    // this chunk of code instantiates and opens the melatonin inspector
    inspectButton.onClick = [&] {
        if (!inspector)
        {
            inspector = std::make_unique<melatonin::Inspector> (*this);
            inspector->onClose = [this]() { inspector.reset(); };
        }

        inspector->setVisible (true);
    };

    setSize (kMargin * 2 + kNumKnobs * kKnobSize + (kNumKnobs - 1) * kKnobGap, kHeaderHeight + kGraphHeight + 2 * kMargin + 18 + kKnobSize + 20 + 28 + 30 + kMargin + 16 + 6 + kTiltRowHeight);
}

PluginEditor::~PluginEditor()
{
}

void PluginEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    auto header = getLocalBounds().removeFromTop (kHeaderHeight).reduced (kMargin, 0);
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("VOX SMOOVE", header, juce::Justification::centredLeft, false);

    g.setColour (juce::Colours::grey);
    g.setFont (12.0f);
    g.drawText (juce::String ("v") + VERSION, header, juce::Justification::centredRight, false);
}

void PluginEditor::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop (kHeaderHeight);

    graph.setBounds (area.removeFromTop (kGraphHeight).reduced (kMargin, 0));
    depthOverlay.setBounds (graph.getBounds());
    area.removeFromTop (kMargin + 18); // room for knob labels

    auto knobRow = area.removeFromTop (kKnobSize + 20).reduced (kMargin, 0);
    for (auto& k : knobs)
    {
        auto cell = knobRow.removeFromLeft (kKnobSize);
        k->slider.setBounds (cell);
        knobRow.removeFromLeft (kKnobGap);
    }

    area.removeFromTop (kMargin);
    auto controls = area.removeFromTop (28).reduced (kMargin, 0);
    modeLabel.setBounds (controls.removeFromLeft (44));
    modeBox.setBounds (controls.removeFromLeft (90));
    controls.removeFromLeft (20);
    stereoLabel.setBounds (controls.removeFromLeft (52));
    stereoBox.setBounds (controls.removeFromLeft (90));
    controls.removeFromLeft (20);
    deltaButton.setBounds (controls.removeFromLeft (80));
    bypassButton.setBounds (controls.removeFromLeft (80));
    controls.removeFromLeft (20);
    sidechainButton.setBounds (controls.removeFromLeft (100));
    keyListenButton.setBounds (controls.removeFromLeft (100));
    keyMeter.setBounds (controls.removeFromLeft (80).reduced (0, 5));

    area.removeFromTop (kMargin);
    tiltLabel.setBounds (area.removeFromTop (16).reduced (kMargin, 0));
    area.removeFromTop (24);
    auto tiltRow = area.removeFromTop (kTiltKnobSize + 20).reduced (kMargin, 0);
    for (auto& k : tiltKnobs)
    {
        k->slider.setBounds (tiltRow.removeFromLeft (kTiltKnobSize));
        tiltRow.removeFromLeft (kKnobGap);
    }

    inspectButton.setBounds (getLocalBounds().removeFromBottom (30).removeFromRight (120).reduced (4));
}
