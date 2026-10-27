#include "PluginEditor.h"
#include "DesignTokens.h"

namespace
{
    using Rect = juce::Rectangle<float>;

    juce::String formatDuration (double seconds)
    {
        const auto totalHundredths = juce::roundToInt (seconds * 100.0);
        const auto minutes = totalHundredths / 6000;
        const auto secs = (totalHundredths / 100) % 60;
        const auto hundredths = totalHundredths % 100;
        return juce::String (minutes) + ":" + juce::String (secs).paddedLeft ('0', 2) + "." + juce::String (hundredths).paddedLeft ('0', 2);
    }

    juce::String percent (double value)
    {
        return juce::String (juce::roundToInt (value * 100.0)) + "%";
    }

    juce::String decibels (double value)
    {
        return (value > 0.05 ? "+" : "") + juce::String (value, 1) + " dB";
    }

    // small spaced-out caps label
    void drawLabel (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area, const juce::Font& font,
                    juce::Colour colour, juce::Justification justification)
    {
        g.setColour (colour);
        g.setFont (font);
        g.drawText (text, area, justification);
    }
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
    const auto bounds = getLocalBounds().toFloat();

    g.setColour (Design::waveBackground);
    g.fillRoundedRectangle (bounds, 10.0f);

    // faint grid
    g.setColour (Design::waveGrid);

    for (float x = 57.0f; x < bounds.getWidth(); x += 57.0f)
        g.drawVerticalLine (juce::roundToInt (x), bounds.getY() + 1.0f, bounds.getBottom() - 1.0f);

    g.drawHorizontalLine (juce::roundToInt (bounds.getCentreY()), bounds.getX() + 1.0f, bounds.getRight() - 1.0f);

    g.setColour (dragging ? Design::lime : Design::waveGrid.brighter (0.15f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 10.0f, dragging ? 2.0f : 1.0f);

    if (sample == nullptr || sample->size() == 0)
    {
        drawLabel (g, "DROP A VOCAL HERE, OR CLICK TO LOAD ONE", getLocalBounds(), Design::mono (12.0f, true),
                   dragging ? Design::lime : Design::textDim, juce::Justification::centred);
    }
    else
    {
        const auto margin = 31.0f;
        const auto pitch = 10.0f;
        const auto barWidth = 6.0f;
        const auto barCount = juce::jmax (1, static_cast<int> ((bounds.getWidth() - 2.0f * margin) / pitch));
        const auto maxHeight = bounds.getHeight() * 0.74f;
        const auto samplesPerBar = static_cast<double> (sample->size()) / barCount;
        const auto midY = bounds.getCentreY() - 4.0f;

        for (int bar = 0; bar < barCount; ++bar)
        {
            const auto start = static_cast<int> (bar * samplesPerBar);
            const auto end = juce::jmin (sample->size(), juce::jmax (start + 1, static_cast<int> ((bar + 1) * samplesPerBar)));
            float peak = 0.0f;

            for (int i = start; i < end; ++i)
                peak = juce::jmax (peak, std::abs (sample->samples[static_cast<size_t> (i)]));

            const auto level = juce::jlimit (0.0f, 1.0f, peak / 0.9f);
            const auto barHeight = 5.0f + level * (maxHeight - 5.0f);
            const auto x = margin + static_cast<float> (bar) * pitch;

            g.setColour (Design::lime.withAlpha (0.8f + 0.2f * level));
            g.fillRoundedRectangle (x, midY - barHeight * 0.5f, barWidth, barHeight, 3.0f);
        }

        drawLabel (g, sample->name.toUpperCase(), getLocalBounds().withTrimmedLeft (10).withTrimmedBottom (6), Design::mono (10.0f),
                   Design::textDim, juce::Justification::bottomLeft);
        drawLabel (g, formatDuration (static_cast<double> (sample->size()) / sample->sampleRate),
                   getLocalBounds().withTrimmedRight (10).withTrimmedBottom (6), Design::mono (10.0f), Design::textDim,
                   juce::Justification::bottomRight);

        // the key that plays the clip at its original pitch
        const auto tag = Rect (bounds.getRight() - 33.0f, 11.0f, 24.0f, 17.0f);
        g.setColour (Design::lime.withAlpha (0.14f));
        g.fillRoundedRectangle (tag, 5.0f);
        g.setColour (Design::lime.withAlpha (0.7f));
        g.drawRoundedRectangle (tag, 5.0f, 1.0f);
        drawLabel (g, "C3", tag.toNearestInt(), Design::mono (10.0f, true), Design::lime, juce::Justification::centred);
    }

    if (message.isNotEmpty())
        drawLabel (g, message.toUpperCase(), getLocalBounds().reduced (12, 8), Design::mono (10.5f, true), Design::red,
                   juce::Justification::topLeft);
}

void WaveformView::openChooser()
{
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

void WaveformView::mouseUp (const juce::MouseEvent& event)
{
    if (contains (event.getPosition()))
        openChooser();
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
// One knob with its name and a value readout underneath.
class KnobCell : public juce::Component
{
public:
    KnobCell (juce::AudioProcessorValueTreeState& apvts, const char* parameterId, const juce::String& labelText,
              juce::Colour accent, float knobDiameter, std::function<juce::String (double)> readout, juce::Colour readoutColour)
        : name (labelText.toUpperCase()), diameter (knobDiameter), readoutText (std::move (readout)), readoutColor (readoutColour)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        slider.setColour (juce::Slider::rotarySliderFillColourId, accent);

        auto* parameter = apvts.getParameter (parameterId);
        slider.setDoubleClickReturnValue (true, parameter->convertFrom0to1 (parameter->getDefaultValue()));
        slider.onValueChange = [this] { repaint(); };
        addAndMakeVisible (slider);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, parameterId, slider);
    }

    float getDiameter() const { return diameter; }
    juce::Point<int> getCellSize() const { return { static_cast<int> (diameter) + 40, static_cast<int> (diameter) + 46 }; }

    void resized() override
    {
        slider.setBounds ((getWidth() - static_cast<int> (diameter)) / 2, 0, static_cast<int> (diameter), static_cast<int> (diameter));
    }

    void paint (juce::Graphics& g) override
    {
        const auto top = static_cast<int> (diameter);
        drawLabel (g, name, { 0, top + 6, getWidth(), 18 }, Design::bold (13.3f).withExtraKerningFactor (0.06f), Design::text,
                   juce::Justification::centred);
        drawLabel (g, readoutText (slider.getValue()), { 0, top + 25, getWidth(), 16 }, Design::mono (10.5f, true), readoutColor,
                   juce::Justification::centred);
    }

private:
    juce::Slider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    juce::String name;
    float diameter;
    std::function<juce::String (double)> readoutText;
    juce::Colour readoutColor;
};

//==============================================================================
class PluginEditor::Content : public juce::Component, private juce::Timer
{
public:
    explicit Content (PluginProcessor& p)
        : processor (p),
          voices (p.apvts, ParamIDs::stack, "Voices", Design::lime, 86.0f,
                  [] (double v) { return juce::String (juce::roundToInt (v)) + (juce::roundToInt (v) == 1 ? " COPY" : " COPIES"); }, Design::lime),
          spread (p.apvts, ParamIDs::spread, "Spread", Design::lime, 86.0f, percent, Design::lime),
          highHype (p.apvts, ParamIDs::octave, "High Hype", Design::lime, 86.0f,
                    [] (double v) { return v < 0.01 ? juce::String ("OFF") : "+12 \xc2\xb7 " + percent (v); }, Design::lime),
          tone (p.apvts, ParamIDs::tone, "Tone", Design::lime, 62.0f,
                [] (double v) { return juce::String (v > 0.66 ? "BRIGHT" : (v > 0.33 ? "WARM" : "DARK")); }, Design::textDim),
          dirt (p.apvts, ParamIDs::dirt, "Dirt", Design::red, 62.0f, percent, Design::textDim),
          output (p.apvts, ParamIDs::gain, "Output", Design::lime, 62.0f, decibels, Design::textDim),
          echo (p.apvts, ParamIDs::echo, "Echo", Design::purple, 62.0f, percent, Design::textDim),
          reverb (p.apvts, ParamIDs::space, "Reverb", Design::purple, 62.0f, percent, Design::textDim),
          keyboard (p.keyboardState)
    {
        addAndMakeVisible (waveform);
        waveform.onFileChosen = [this] (const juce::File& file) { loadFile (file); };

        for (auto* knob : { &voices, &spread, &highHype, &tone, &dirt, &output, &echo, &reverb })
            addAndMakeVisible (*knob);

        for (int i = 0; i < processor.getNumPrograms(); ++i)
            presetBox.addItem (processor.getProgramName (i), i + 1);

        presetBox.setSelectedItemIndex (processor.getCurrentProgram(), juce::dontSendNotification);
        presetBox.onChange = [this] { processor.setCurrentProgram (presetBox.getSelectedItemIndex()); };
        addAndMakeVisible (presetBox);

        addAndMakeVisible (oneShotSwitch);
        oneShotAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (processor.apvts, ParamIDs::oneShot, oneShotSwitch);
        oneShotSwitch.onStateChange = [this] { repaint ({ 1000, 20, 160, 50 }); };

        replaceButton.onClick = [this] { waveform.openChooser(); };
        clearButton.onClick = [this]
        {
            processor.clearSample();
            waveform.setMessage ({});
        };
        addAndMakeVisible (replaceButton);
        addAndMakeVisible (clearButton);

        static const char* timings[] = { "1/16", "1/8", "1/8.", "1/4" };
        auto* timingParameter = processor.apvts.getParameter (ParamIDs::echoTime);

        timingAttachment = std::make_unique<juce::ParameterAttachment> (*timingParameter,
                                                                       [this] (float value)
                                                                       {
                                                                           for (size_t i = 0; i < pills.size(); ++i)
                                                                               pills[i].setToggleState (juce::roundToInt (value) == static_cast<int> (i), juce::dontSendNotification);
                                                                       });

        for (size_t i = 0; i < pills.size(); ++i)
        {
            pills[i].setButtonText (timings[i]);
            pills[i].getProperties().set ("pill", true);
            pills[i].onClick = [this, i] { timingAttachment->setValueAsCompleteGesture (static_cast<float> (i)); };
            addAndMakeVisible (pills[i]);
        }

        timingAttachment->sendInitialUpdate();

        keyboard.setAvailableRange (48, 72);
        keyboard.setLowestVisibleKey (48);
        keyboard.setKeyWidth (1029.0f / 15.0f);
        addAndMakeVisible (keyboard);

        refreshSample();
        startTimerHz (15);
    }

    ~Content() override = default;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Design::shellBackground);
        paintHeader (g);
        paintSource (g);
        paintStackPanel (g);
        paintColorPanel (g);
        paintSpacePanel (g);

        // keyboard panel
        paintPanel (g, keyboardPanel, Design::sourceBackground, Design::shellBorder);
        drawLabel (g, "KEYBOARD", { 46, 560, 80, 16 }, Design::mono (10.0f, true), Design::textDim, juce::Justification::centredLeft);
        drawLabel (g, "PLAY A NOTE", { 46, 636, 90, 16 }, Design::mono (10.0f, true), Design::lime, juce::Justification::centredLeft);

        drawLabel (g, "STACK BUDDY  V" + juce::String (VERSION), { 34, 675, 300, 16 }, Design::mono (10.0f), Design::textFaint,
                   juce::Justification::centredLeft);
    }

    void resized() override
    {
        waveform.setBounds (43, 142, 1099, 130);
        replaceButton.setBounds (1025, 111, 56, 23);
        clearButton.setBounds (1088, 111, 52, 23);

        presetBox.setBounds (603, 26, 150, 37);
        oneShotSwitch.setBounds (1007, 35, 35, 19);

        place (voices, 120, 391);
        place (spread, 264, 391);
        place (highHype, 409, 391);
        place (tone, 565, 401);
        place (dirt, 655, 401);
        place (output, 745, 401);
        place (echo, 876, 401);
        place (reverb, 1089, 401);

        for (size_t i = 0; i < pills.size(); ++i)
            pills[i].setBounds (958, 413 + static_cast<int> (i) * 24, 50, 20);

        keyboard.setBounds (116, 557, 1029, 107);
    }

private:
    static inline const Rect sourcePanel { 33.0f, 102.0f, 1119.0f, 180.0f };
    static inline const Rect stackPanelArea { 33.0f, 293.0f, 464.0f, 244.0f };
    static inline const Rect colorPanelArea { 508.0f, 293.0f, 294.0f, 244.0f };
    static inline const Rect spacePanelArea { 813.0f, 293.0f, 339.0f, 244.0f };
    static inline const Rect keyboardPanel { 33.0f, 548.0f, 1119.0f, 117.0f };

    // centres the knob cell on x, with the knob's top edge at y
    void place (KnobCell& cell, int centreX, int top)
    {
        const auto size = cell.getCellSize();
        cell.setBounds (centreX - size.x / 2, top, size.x, size.y);
    }

    static void paintPanel (juce::Graphics& g, Rect area, juce::Colour fill, juce::Colour border)
    {
        g.setColour (fill);
        g.fillRoundedRectangle (area, 16.0f);
        g.setColour (border);
        g.drawRoundedRectangle (area.reduced (0.5f), 16.0f, 1.0f);
    }

    static void paintPanelHeader (juce::Graphics& g, Rect area, const juce::String& number, const juce::String& kicker,
                                  const juce::String& title, juce::Colour titleColour)
    {
        const auto x = static_cast<int> (area.getX());
        const auto y = static_cast<int> (area.getY());

        drawLabel (g, number, { x + 16, y + 29, 24, 16 }, Design::mono (10.5f), Design::textFaint, juce::Justification::centredLeft);
        drawLabel (g, kicker.toUpperCase(), { x + 38, y + 21, 240, 14 }, Design::mono (9.5f, true), Design::textDim,
                   juce::Justification::centredLeft);
        drawLabel (g, title.toUpperCase(), { x + 38, y + 37, 300, 28 }, Design::bold (18.5f), titleColour, juce::Justification::centredLeft);

        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawHorizontalLine (y + 72, area.getX() + 16.0f, area.getRight() - 16.0f);
    }

    void paintHeader (juce::Graphics& g)
    {
        g.setColour (Design::headerBackground);
        g.fillRect (0, 0, Design::width, 83);
        g.setColour (Design::shellBorder);
        g.drawHorizontalLine (83, 0.0f, static_cast<float> (Design::width));

        // logo tile with four level bars
        const auto tile = Rect (40.0f, 25.0f, 39.0f, 39.0f);
        g.setColour (Design::lime);
        g.fillRoundedRectangle (tile, 10.0f);

        const float barHeights[] = { 12.0f, 23.0f, 17.0f, 9.0f };
        g.setColour (Design::outerBackground);

        for (int i = 0; i < 4; ++i)
        {
            const auto x = tile.getX() + 8.5f + static_cast<float> (i) * 6.6f;
            g.fillRoundedRectangle (x, tile.getCentreY() - barHeights[i] * 0.5f, 3.6f, barHeights[i], 1.8f);
        }

        drawLabel (g, "STACK BUDDY", { 92, 24, 260, 28 }, Design::bold (24.0f).withExtraKerningFactor (-0.02f), Design::text,
                   juce::Justification::centredLeft);
        drawLabel (g, "BY PLAYTIME AUDIO", { 92, 47, 260, 14 }, Design::mono (10.5f), Design::textDim, juce::Justification::centredLeft);

        drawLabel (g, "PRESET", { 548, 36, 60, 16 }, Design::mono (10.5f, true), Design::textDim, juce::Justification::centredLeft);

        drawLabel (g, "ONE SHOT", { 1052, 28, 100, 16 }, Design::bold (11.2f).withExtraKerningFactor (0.06f), Design::text,
                   juce::Justification::centredLeft);
        const auto on = oneShotSwitch.getToggleState();
        drawLabel (g, on ? "ON" : "OFF", { 1052, 44, 100, 14 }, Design::mono (10.0f, true), on ? Design::lime : Design::textFaint,
                   juce::Justification::centredLeft);
    }

    void paintSource (juce::Graphics& g)
    {
        paintPanel (g, sourcePanel, Design::sourceBackground, Design::shellBorder);
        drawLabel (g, "SOURCE", { 51, 113, 120, 18 }, Design::mono (11.0f, true), Design::textDim, juce::Justification::centredLeft);

        const auto loaded = shownSample != nullptr;
        g.setColour (loaded ? Design::lime : Design::textFaint);
        g.fillEllipse (913.0f, 118.0f, 7.0f, 7.0f);
        drawLabel (g, loaded ? "SAMPLE READY" : "NO SAMPLE", { 929, 112, 90, 20 }, Design::mono (10.5f, true),
                   loaded ? Design::lime : Design::textDim, juce::Justification::centredLeft);
    }

    void paintStackPanel (juce::Graphics& g)
    {
        paintPanel (g, stackPanelArea, Design::stackPanel, Design::stackPanelBorder);
        paintPanelHeader (g, stackPanelArea, "01", "The main event", "Build your stack", Design::lime);

        // the copies, drawn as overlapping circles that fan out as Spread goes up
        const auto count = juce::jlimit (1, StackEngine::maxStack, juce::roundToInt (processor.apvts.getRawParameterValue (ParamIDs::stack)->load()));
        const auto fan = processor.apvts.getRawParameterValue (ParamIDs::spread)->load();
        const auto step = 3.0f + fan * 4.5f;
        const auto cx = 450.0f;
        const auto cy = 333.0f;

        for (int i = count - 1; i >= 1; --i)
        {
            const auto side = (i % 2 == 1) ? -1.0f : 1.0f;
            const auto x = cx + side * static_cast<float> ((i + 1) / 2) * step;
            g.setColour (Design::lime.withAlpha (0.55f));
            g.drawEllipse (x - 9.0f, cy - 9.0f, 18.0f, 18.0f, 1.2f);
        }

        g.setColour (Design::lime);
        g.fillEllipse (cx - 9.0f, cy - 9.0f, 18.0f, 18.0f);
    }

    void paintColorPanel (juce::Graphics& g)
    {
        paintPanel (g, colorPanelArea, Design::colorPanel, Design::colorPanelBorder);
        paintPanelHeader (g, colorPanelArea, "02", "Shape it", "Color", Design::text);
    }

    void paintSpacePanel (juce::Graphics& g)
    {
        paintPanel (g, spacePanelArea, Design::spacePanel, Design::spacePanelBorder);
        paintPanelHeader (g, spacePanelArea, "03", "Send it back", "Space", Design::text);

        drawLabel (g, "TIMING", { 933, 391, 100, 16 }, Design::bold (12.5f).withExtraKerningFactor (0.06f), Design::text,
                   juce::Justification::centred);
        drawLabel (g, "SYNCED", { 933, 512, 100, 14 }, Design::mono (10.0f), Design::textDim, juce::Justification::centred);
    }

    void loadFile (const juce::File& file)
    {
        juce::String error;

        if (processor.loadSampleFromFile (file, error))
            waveform.setMessage ({});
        else
            waveform.setMessage (error);
    }

    void refreshSample()
    {
        shownSample = processor.getSample();
        waveform.setSample (shownSample);
        replaceButton.setButtonText (shownSample != nullptr ? "REPLACE" : "LOAD");
        clearButton.setEnabled (shownSample != nullptr);
        repaint (900, 102, 260, 40);
    }

    void timerCallback() override
    {
        // the clip can also change when the host restores a project
        if (processor.getSample() != shownSample)
            refreshSample();

        // the circles next to the panel title follow the Voices and Spread knobs
        repaint (410, 316, 80, 34);
    }

    PluginProcessor& processor;

    WaveformView waveform;
    juce::TextButton replaceButton { "LOAD" }, clearButton { "CLEAR" };
    juce::ComboBox presetBox;
    juce::ToggleButton oneShotSwitch;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> oneShotAttachment;

    KnobCell voices, spread, highHype, tone, dirt, output, echo, reverb;

    std::array<juce::TextButton, 4> pills;
    std::unique_ptr<juce::ParameterAttachment> timingAttachment;

    StackKeyboard keyboard;
    std::shared_ptr<const SampleData> shownSample;
};

//==============================================================================
PluginEditor::PluginEditor (PluginProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
      resizer (this, &constrainer)
{
    setLookAndFeel (&lookAndFeel);

    content = std::make_unique<Content> (p);
    addAndMakeVisible (*content);
    addAndMakeVisible (resizer);

    // our own corner handle (drawn in the design's style) keeps the window at the design's shape
    constrainer.setMinimumSize (590, 350);
    constrainer.setMaximumSize (1770, 1050);
    constrainer.setFixedAspectRatio (static_cast<double> (Design::width) / Design::height);
    setConstrainer (&constrainer);
    setResizable (true, false);

    setSize (1000, 593);
}

PluginEditor::~PluginEditor()
{
    setLookAndFeel (nullptr);
}

void PluginEditor::paint (juce::Graphics& g)
{
    g.fillAll (Design::outerBackground);
}

void PluginEditor::resized()
{
    const auto scale = static_cast<float> (getWidth()) / static_cast<float> (Design::width);
    content->setTransform (juce::AffineTransform::scale (scale));
    content->setBounds (0, 0, Design::width, Design::height);

    resizer.setBounds (getWidth() - 24, getHeight() - 24, 24, 24);
    resizer.toFront (false);
}
