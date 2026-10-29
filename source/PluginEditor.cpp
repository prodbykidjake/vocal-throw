#include "PluginEditor.h"
#include "DesignTokens.h"

namespace
{
    using Rect = juce::Rectangle<float>;

    constexpr float barPitch = 10.0f;
    constexpr float barWidth = 6.0f;
    constexpr float barMargin = 31.0f;
    constexpr float floorDb = -48.0f;    // quieter than this draws as the flat baseline
    constexpr float signalDb = -50.0f;   // louder than this counts as "listening"

    juce::String percent (double value)
    {
        return juce::String (juce::roundToInt (value * 100.0)) + "%";
    }

    juce::String decibels (double value)
    {
        return (value > 0.05 ? "+" : "") + juce::String (value, 1) + " dB";
    }

    void drawLabel (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area, const juce::Font& font,
                    juce::Colour colour, juce::Justification justification)
    {
        g.setColour (colour);
        g.setFont (font);
        g.drawText (text, area, justification);
    }
}

//==============================================================================
int InputScope::getBarCount() const
{
    return juce::jlimit (1, maxBars, static_cast<int> ((static_cast<float> (getWidth()) - 2.0f * barMargin) / barPitch));
}

void InputScope::setLevels (const float* newLevels, int count)
{
    numLevels = juce::jlimit (0, maxBars, count);
    std::copy (newLevels, newLevels + numLevels, levels.begin());
    repaint();
}

void InputScope::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    g.setColour (Design::waveBackground);
    g.fillRoundedRectangle (bounds, 10.0f);

    // faint grid
    g.setColour (Design::waveGrid);

    for (float x = 57.0f; x < bounds.getWidth(); x += 57.0f)
        g.drawVerticalLine (juce::roundToInt (x), bounds.getY() + 1.0f, bounds.getBottom() - 1.0f);

    g.drawHorizontalLine (juce::roundToInt (bounds.getCentreY()), bounds.getX() + 1.0f, bounds.getRight() - 1.0f);

    g.setColour (Design::waveGrid.brighter (0.15f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 10.0f, 1.0f);

    const auto maxHeight = bounds.getHeight() * 0.74f;
    const auto midY = bounds.getCentreY() - 4.0f;

    for (int bar = 0; bar < numLevels; ++bar)
    {
        const auto peak = levels[static_cast<size_t> (bar)];
        const auto db = juce::Decibels::gainToDecibels (peak, floorDb);
        const auto level = juce::jlimit (0.0f, 1.0f, (db - floorDb) / -floorDb);
        const auto barHeight = 5.0f + level * (maxHeight - 5.0f);
        const auto x = barMargin + static_cast<float> (bar) * barPitch;

        g.setColour (level > 0.0f ? Design::lime.withAlpha (0.8f + 0.2f * level) : Design::waveGrid.brighter (0.4f));
        g.fillRoundedRectangle (x, midY - barHeight * 0.5f, barWidth, barHeight, 3.0f);
    }

    drawLabel (g, "LIVE INPUT", getLocalBounds().withTrimmedLeft (10).withTrimmedBottom (6), Design::mono (10.0f), Design::textDim,
               juce::Justification::bottomLeft);
    drawLabel (g, "LAST " + juce::String (juce::roundToInt (numLevels * 0.035)) + " SEC", getLocalBounds().withTrimmedRight (10).withTrimmedBottom (6),
               Design::mono (10.0f), Design::textDim, juce::Justification::bottomRight);
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
                  [] (double v) { return juce::roundToInt (v) == 1 ? juce::String ("JUST YOU") : juce::String (juce::roundToInt (v)) + " VOICES"; }, Design::lime),
          spread (p.apvts, ParamIDs::spread, "Spread", Design::lime, 86.0f, percent, Design::lime),
          highHype (p.apvts, ParamIDs::octave, "High Hype", Design::lime, 86.0f,
                    [] (double v) { return v < 0.01 ? juce::String ("OFF") : "+12 \xc2\xb7 " + percent (v); }, Design::lime),
          tone (p.apvts, ParamIDs::tone, "Tone", Design::lime, 62.0f,
                [] (double v) { return juce::String (v > 0.999 ? "OPEN" : (v > 0.66 ? "BRIGHT" : (v > 0.33 ? "WARM" : "DARK"))); }, Design::textDim),
          dirt (p.apvts, ParamIDs::dirt, "Dirt", Design::red, 62.0f, percent, Design::textDim),
          output (p.apvts, ParamIDs::gain, "Output", Design::lime, 62.0f, decibels, Design::textDim),
          echo (p.apvts, ParamIDs::echo, "Echo", Design::purple, 62.0f, percent, Design::textDim),
          reverb (p.apvts, ParamIDs::space, "Reverb", Design::purple, 62.0f, percent, Design::textDim)
    {
        addAndMakeVisible (scope);

        for (auto* knob : { &voices, &spread, &highHype, &tone, &dirt, &output, &echo, &reverb })
            addAndMakeVisible (*knob);

        for (int i = 0; i < processor.getNumPrograms(); ++i)
            presetBox.addItem (processor.getProgramName (i), i + 1);

        presetBox.setSelectedItemIndex (processor.getCurrentProgram(), juce::dontSendNotification);
        presetBox.onChange = [this] { processor.setCurrentProgram (presetBox.getSelectedItemIndex()); };
        addAndMakeVisible (presetBox);

        addAndMakeVisible (activeSwitch);
        activeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (processor.apvts, ParamIDs::active, activeSwitch);
        activeSwitch.onStateChange = [this] { repaint (1000, 20, 160, 50); };

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

        timerCallback();
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Design::shellBackground);
        paintHeader (g);
        paintInput (g);
        paintStackPanel (g);
        paintColorPanel (g);
        paintSpacePanel (g);

        drawLabel (g, "STACK BUDDY  V" + juce::String (VERSION), { 34, 546, 300, 16 }, Design::mono (10.0f), Design::textFaint,
                   juce::Justification::centredLeft);
    }

    void resized() override
    {
        scope.setBounds (43, 142, 1099, 130);
        presetBox.setBounds (603, 26, 150, 37);
        activeSwitch.setBounds (1007, 35, 35, 19);

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
    }

private:
    static inline const Rect inputPanel { 33.0f, 102.0f, 1119.0f, 180.0f };
    static inline const Rect stackPanelArea { 33.0f, 293.0f, 464.0f, 244.0f };
    static inline const Rect colorPanelArea { 508.0f, 293.0f, 294.0f, 244.0f };
    static inline const Rect spacePanelArea { 813.0f, 293.0f, 339.0f, 244.0f };

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

        drawLabel (g, "STACK", { 1052, 28, 100, 16 }, Design::bold (11.2f).withExtraKerningFactor (0.06f), Design::text,
                   juce::Justification::centredLeft);
        const auto on = activeSwitch.getToggleState();
        drawLabel (g, on ? "ON" : "BYPASSED", { 1052, 44, 100, 14 }, Design::mono (10.0f, true), on ? Design::lime : Design::textFaint,
                   juce::Justification::centredLeft);
    }

    void paintInput (juce::Graphics& g)
    {
        paintPanel (g, inputPanel, Design::sourceBackground, Design::shellBorder);
        drawLabel (g, "INPUT", { 51, 113, 120, 18 }, Design::mono (11.0f, true), Design::textDim, juce::Justification::centredLeft);

        g.setColour (hearingSignal ? Design::lime : Design::textFaint);
        g.fillEllipse (875.0f, 118.0f, 7.0f, 7.0f);
        drawLabel (g, hearingSignal ? "LISTENING" : "WAITING FOR AUDIO", { 891, 112, 140, 20 }, Design::mono (10.5f, true),
                   hearingSignal ? Design::lime : Design::textDim, juce::Justification::centredLeft);

        const auto peakText = hearingSignal ? "PEAK " + juce::String (juce::Decibels::gainToDecibels (recentPeak), 1) + " DB" : juce::String ("PEAK  --");
        drawLabel (g, peakText, { 1030, 112, 110, 20 }, Design::mono (10.5f, true), Design::textDim, juce::Justification::centredRight);
    }

    void paintStackPanel (juce::Graphics& g)
    {
        paintPanel (g, stackPanelArea, Design::stackPanel, Design::stackPanelBorder);
        paintPanelHeader (g, stackPanelArea, "01", "The main event", "Build your stack", Design::lime);

        // the voices, drawn as overlapping circles that fan out as Spread goes up
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

    void timerCallback() override
    {
        std::array<float, InputScope::maxBars> history {};
        const auto bars = scope.getBarCount();
        processor.getInputHistory (history.data(), bars);
        scope.setLevels (history.data(), bars);

        // status follows the last half second or so
        float peak = 0.0f;

        for (int i = juce::jmax (0, bars - 15); i < bars; ++i)
            peak = juce::jmax (peak, history[static_cast<size_t> (i)]);

        const auto nowHearing = juce::Decibels::gainToDecibels (peak) > signalDb;

        if (nowHearing != hearingSignal || std::abs (peak - recentPeak) > 0.001f)
        {
            hearingSignal = nowHearing;
            recentPeak = peak;
            repaint (860, 102, 292, 40);
        }

        // the circles next to the panel title follow the Voices and Spread knobs
        repaint (410, 316, 80, 34);
    }

    PluginProcessor& processor;

    InputScope scope;
    juce::ComboBox presetBox;
    juce::ToggleButton activeSwitch;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> activeAttachment;

    KnobCell voices, spread, highHype, tone, dirt, output, echo, reverb;

    std::array<juce::TextButton, 4> pills;
    std::unique_ptr<juce::ParameterAttachment> timingAttachment;

    bool hearingSignal = false;
    float recentPeak = 0.0f;
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
    constrainer.setMinimumSize (590, 285);
    constrainer.setMaximumSize (1770, 855);
    constrainer.setFixedAspectRatio (static_cast<double> (Design::width) / Design::height);
    setConstrainer (&constrainer);
    setResizable (true, false);

    setSize (1000, 483);
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
