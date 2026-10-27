#include "PluginEditor.h"

namespace
{
    const auto backgroundColour = juce::Colour (0xff14141c);
    const auto panelColour = juce::Colour (0xff1e1e2a);
    const auto accentColour = juce::Colour (0xffff5a8a);
    const auto textColour = juce::Colour (0xffe8e8f0);
    const auto dimTextColour = juce::Colour (0xff8a8aa0);
}

//==============================================================================
void WaveformView::setSample (std::shared_ptr<const SampleData> newSample)
{
    sample = std::move (newSample);
    repaint();
}

void WaveformView::setMessage (const juce::String& newMessage)
{
    message = newMessage;
    repaint();
}

void WaveformView::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour (panelColour);
    g.fillRoundedRectangle (bounds, 8.0f);

    g.setColour (dragging ? accentColour : accentColour.withAlpha (0.25f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, dragging ? 2.0f : 1.0f);

    if (sample == nullptr || sample->size() == 0)
    {
        g.setColour (dimTextColour);
        g.setFont (juce::FontOptions (15.0f));
        g.drawText ("Drop a vocal here, or click to load one", getLocalBounds(), juce::Justification::centred);
    }
    else
    {
        const auto inner = bounds.reduced (12.0f, 10.0f);
        const auto width = juce::jmax (1, static_cast<int> (inner.getWidth()));
        const auto samplesPerPixel = static_cast<double> (sample->size()) / width;
        const auto midY = inner.getCentreY();
        const auto halfHeight = inner.getHeight() * 0.5f;

        juce::Path path;

        for (int x = 0; x < width; ++x)
        {
            const auto start = static_cast<int> (x * samplesPerPixel);
            const auto end = juce::jmin (sample->size(), juce::jmax (start + 1, static_cast<int> ((x + 1) * samplesPerPixel)));
            float peak = 0.0f;

            for (int i = start; i < end; ++i)
                peak = juce::jmax (peak, std::abs (sample->samples[static_cast<size_t> (i)]));

            const auto px = inner.getX() + static_cast<float> (x);
            path.addRectangle (px, midY - peak * halfHeight, 1.0f, juce::jmax (1.0f, peak * halfHeight * 2.0f));
        }

        g.setColour (accentColour);
        g.fillPath (path);

        g.setColour (dimTextColour);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (sample->name + "   " + juce::String (static_cast<double> (sample->size()) / sample->sampleRate, 2) + " s",
                    getLocalBounds().reduced (14, 6), juce::Justification::topLeft);
    }

    if (message.isNotEmpty())
    {
        g.setColour (textColour);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (message, getLocalBounds().reduced (14, 6), juce::Justification::bottomLeft);
    }
}

void WaveformView::mouseUp (const juce::MouseEvent& event)
{
    if (! contains (event.getPosition()))
        return;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    chooser = std::make_unique<juce::FileChooser> ("Choose a vocal", juce::File(), formats.getWildcardForAllFormats());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();

                              if (file.existsAsFile() && onFileChosen != nullptr)
                                  onFileChosen (file);
                          });
}

bool WaveformView::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1;
}

void WaveformView::fileDragEnter (const juce::StringArray&, int, int)
{
    dragging = true;
    repaint();
}

void WaveformView::fileDragExit (const juce::StringArray&)
{
    dragging = false;
    repaint();
}

void WaveformView::filesDropped (const juce::StringArray& files, int, int)
{
    dragging = false;
    repaint();

    if (files.size() == 1 && onFileChosen != nullptr)
        onFileChosen (juce::File (files[0]));
}

//==============================================================================
StackLookAndFeel::StackLookAndFeel()
{
    setColour (juce::Slider::rotarySliderFillColourId, accentColour);
    setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff343446));
    setColour (juce::Slider::thumbColourId, textColour);
    setColour (juce::Slider::textBoxTextColourId, textColour);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, dimTextColour);
    setColour (juce::ToggleButton::textColourId, textColour);
    setColour (juce::ToggleButton::tickColourId, accentColour);
    setColour (juce::ComboBox::backgroundColourId, panelColour);
    setColour (juce::ComboBox::textColourId, textColour);
    setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff343446));
    setColour (juce::ComboBox::arrowColourId, accentColour);
    setColour (juce::PopupMenu::backgroundColourId, panelColour);
    setColour (juce::PopupMenu::textColourId, textColour);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accentColour);
}

//==============================================================================
PluginEditor::PluginEditor (PluginProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
      keyboard (p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (waveform);
    waveform.onFileChosen = [this] (const juce::File& file) { loadFile (file); };

    addKnob (stackKnob, "Stack", ParamIDs::stack);
    addKnob (spreadKnob, "Spread", ParamIDs::spread);
    addKnob (octaveKnob, "Octave", ParamIDs::octave);
    addKnob (toneKnob, "Tone", ParamIDs::tone);
    addKnob (dirtKnob, "Dirt", ParamIDs::dirt);
    addKnob (echoKnob, "Echo", ParamIDs::echo);
    addKnob (spaceKnob, "Space", ParamIDs::space);
    addKnob (gainKnob, "Gain", ParamIDs::gain);

    echoTimeBox.addItemList (juce::StringArray { "1/16", "1/8", "1/8 dotted", "1/4" }, 1);
    addAndMakeVisible (echoTimeBox);
    echoTimeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (processorRef.apvts, ParamIDs::echoTime, echoTimeBox);

    echoTimeLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (echoTimeLabel);

    addAndMakeVisible (oneShotButton);
    oneShotAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (processorRef.apvts, ParamIDs::oneShot, oneShotButton);

    hintLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (hintLabel);

    keyboard.setAvailableRange (48, 83);
    keyboard.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, accentColour);
    keyboard.setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, accentColour.withAlpha (0.4f));
    addAndMakeVisible (keyboard);

    setSize (720, 450);
    startTimerHz (10);
    timerCallback();
}

PluginEditor::~PluginEditor()
{
    setLookAndFeel (nullptr);
}

void PluginEditor::addKnob (Knob& knob, const juce::String& name, const char* parameterId)
{
    knob.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 18);
    knob.slider.setDoubleClickReturnValue (true, processorRef.apvts.getParameter (parameterId)->convertFrom0to1 (processorRef.apvts.getParameter (parameterId)->getDefaultValue()));
    addAndMakeVisible (knob.slider);

    knob.label.setText (name.toUpperCase(), juce::dontSendNotification);
    knob.label.setJustificationType (juce::Justification::centred);
    knob.label.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    addAndMakeVisible (knob.label);

    knob.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processorRef.apvts, parameterId, knob.slider);
}

void PluginEditor::loadFile (const juce::File& file)
{
    juce::String error;

    if (processorRef.loadSampleFromFile (file, error))
        waveform.setMessage ({});
    else
        waveform.setMessage (error);
}

void PluginEditor::timerCallback()
{
    // the clip can also change when the host restores a project
    auto current = processorRef.getSample();

    if (current != shownSample)
    {
        shownSample = current;
        waveform.setSample (shownSample);
    }
}

void PluginEditor::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);

    g.setColour (textColour);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("STACK BUDDY", 20, 12, 300, 30, juce::Justification::centredLeft);

    g.setColour (dimTextColour);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("Playtime Audio", getWidth() - 220, 12, 200, 30, juce::Justification::centredRight);
}

void PluginEditor::resized()
{
    auto area = getLocalBounds().reduced (20, 12);
    area.removeFromTop (36);

    waveform.setBounds (area.removeFromTop (120));
    area.removeFromTop (12);

    auto knobRow = area.removeFromTop (120);
    Knob* knobs[] = { &stackKnob, &spreadKnob, &octaveKnob, &toneKnob, &dirtKnob, &echoKnob, &spaceKnob, &gainKnob };
    const auto cellWidth = knobRow.getWidth() / 8;

    for (auto* knob : knobs)
    {
        auto cell = knobRow.removeFromLeft (cellWidth);
        knob->label.setBounds (cell.removeFromTop (18));
        knob->slider.setBounds (cell);
    }

    area.removeFromTop (6);
    auto controls = area.removeFromTop (28);
    oneShotButton.setBounds (controls.removeFromLeft (110));
    hintLabel.setBounds (controls.removeFromLeft (260));
    echoTimeBox.setBounds (controls.removeFromRight (110));
    echoTimeLabel.setBounds (controls.removeFromRight (90));

    area.removeFromTop (8);
    keyboard.setBounds (area);
    keyboard.setKeyWidth (static_cast<float> (area.getWidth()) / 21.0f);
}
