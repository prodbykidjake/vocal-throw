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
    constexpr float meterFloorDb = -48.0f;
    constexpr float meterTopDb = 3.0f;

    juce::String percent (double value)
    {
        return juce::String (juce::roundToInt (value * 100.0)) + "%";
    }

    juce::String percentOrOff (double value)
    {
        return value < 0.01 ? juce::String ("OFF") : percent (value);
    }

    // a middle dot, spelled out as UTF-8 so it isn't read as two Latin-1 characters
    const juce::String dot = juce::String::fromUTF8 (" \xc2\xb7 ");

    juce::String decibels (double value)
    {
        return (value > 0.05 ? "+" : "") + juce::String (value, 1) + " dB";
    }

    // 0 at floorDb or quieter, 1 at 0 dB
    float levelFromPeak (float peak)
    {
        const auto db = juce::Decibels::gainToDecibels (peak, floorDb);
        return juce::jlimit (0.0f, 1.0f, (db - floorDb) / -floorDb);
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

void InputScope::setStack (int voices, float spread, float octave, float harmony, int harmonyCount)
{
    stackVoices = voices;
    stackSpread = spread;
    octaveLevel = octave;
    harmonyLevel = harmony;
    harmonyVoices = harmonyCount;
}

void InputScope::paintLayer (juce::Graphics& g, float dx, float heightScale, juce::Colour colour, bool filled) const
{
    const auto bounds = getLocalBounds().toFloat();
    const auto maxHeight = bounds.getHeight() * 0.74f;
    const auto midY = bounds.getCentreY() - 4.0f;
    g.setColour (colour);

    for (int bar = 0; bar < numLevels; ++bar)
    {
        const auto level = levelFromPeak (levels[static_cast<size_t> (bar)]);

        if (level <= 0.0f)
            continue;

        const auto barHeight = (5.0f + level * (maxHeight - 5.0f)) * heightScale;
        const auto x = barMargin + static_cast<float> (bar) * barPitch + dx;

        if (filled)
            g.fillRoundedRectangle (x, midY - barHeight * 0.5f, barWidth, barHeight, 3.0f);
        else
            g.drawRoundedRectangle (x, midY - barHeight * 0.5f, barWidth, barHeight, 3.0f, 1.0f);
    }
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

    // the stack behind the vocal: copies fan out left and right with Spread, harmonies and the octave sit on top
    for (int voice = stackVoices - 1; voice >= 1; --voice)
    {
        const auto side = (voice % 2 == 1) ? -1.0f : 1.0f;
        const auto rank = static_cast<float> ((voice + 1) / 2);
        paintLayer (g, side * rank * (2.0f + 4.0f * stackSpread), 1.0f - 0.07f * rank, Design::lime.withAlpha (0.24f), false);
    }

    // harmonies are drawn taller than the vocal so they show above and below it
    if (harmonyLevel > 0.01f)
        for (int voice = harmonyVoices - 1; voice >= 0; --voice)
            paintLayer (g, 0.0f, voice == 0 ? 1.12f : 1.22f, Design::cyan.withAlpha (0.15f + 0.4f * harmonyLevel), true);

    const auto maxHeight = bounds.getHeight() * 0.74f;
    const auto midY = bounds.getCentreY() - 4.0f;

    for (int bar = 0; bar < numLevels; ++bar)
    {
        const auto level = levelFromPeak (levels[static_cast<size_t> (bar)]);
        const auto barHeight = 5.0f + level * (maxHeight - 5.0f);
        const auto x = barMargin + static_cast<float> (bar) * barPitch;

        g.setColour (level > 0.0f ? Design::lime.withAlpha (0.8f + 0.2f * level) : Design::waveGrid.brighter (0.4f));
        g.fillRoundedRectangle (x, midY - barHeight * 0.5f, barWidth, barHeight, 3.0f);

        // the octave copy, as a bright core inside each bar
        if (octaveLevel > 0.01f && level > 0.0f)
        {
            const auto coreHeight = barHeight * 0.55f;
            g.setColour (juce::Colours::white.withAlpha (0.25f + 0.55f * octaveLevel));
            g.fillRoundedRectangle (x + 2.0f, midY - coreHeight * 0.5f, barWidth - 4.0f, coreHeight, 1.0f);
        }
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
        // neighbouring cells can overlap, so only the knob itself takes clicks
        setInterceptsMouseClicks (false, true);

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
// A row of pill buttons that picks one choice of a parameter.
class PillGroup
{
public:
    PillGroup (juce::Component& parent, juce::AudioProcessorValueTreeState& apvts, const char* parameterId,
               const juce::StringArray& labels, juce::Colour accent)
    {
        auto* parameter = apvts.getParameter (parameterId);
        attachment = std::make_unique<juce::ParameterAttachment> (*parameter, [this] (float value) { select (juce::roundToInt (value)); });

        for (int i = 0; i < labels.size(); ++i)
        {
            auto* button = buttons.add (new juce::TextButton (labels[i]));
            button->getProperties().set ("pill", true);
            button->getProperties().set ("accent", static_cast<juce::int64> (accent.getARGB()));
            button->onClick = [this, i] { attachment->setValueAsCompleteGesture (static_cast<float> (i)); };
            parent.addAndMakeVisible (button);
        }

        attachment->sendInitialUpdate();
    }

    int size() const { return buttons.size(); }
    juce::TextButton& operator[] (int index) { return *buttons[index]; }

private:
    void select (int index)
    {
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setToggleState (i == index, juce::dontSendNotification);
    }

    juce::OwnedArray<juce::TextButton> buttons;
    std::unique_ptr<juce::ParameterAttachment> attachment;
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
                    [] (double v) { return v < 0.01 ? juce::String ("OFF") : "+12" + dot + percent (v); }, Design::lime),
          tone (p.apvts, ParamIDs::tone, "Tone", Design::lime, 62.0f,
                [] (double v) { return juce::String (v > 0.999 ? "OPEN" : (v > 0.66 ? "BRIGHT" : (v > 0.33 ? "WARM" : "DARK"))); }, Design::textDim),
          dirt (p.apvts, ParamIDs::dirt, "Dirt", Design::red, 62.0f, percent, Design::textDim),
          output (p.apvts, ParamIDs::gain, "Output", Design::lime, 62.0f, decibels, Design::textDim),
          echo (p.apvts, ParamIDs::echo, "Echo", Design::purple, 62.0f, percent, Design::textDim),
          reverb (p.apvts, ParamIDs::space, "Reverb", Design::purple, 62.0f, percent, Design::textDim),
          duck (p.apvts, ParamIDs::duck, "Duck", Design::purple, 62.0f, percentOrOff, Design::textDim),
          harmony (p.apvts, ParamIDs::harmony, "Harmony", Design::cyan, 54.0f, percentOrOff, Design::cyan),
          timing (*this, p.apvts, ParamIDs::echoTime, { "1/16", "1/8", "1/8.", "1/4" }, Design::purple),
          interval (*this, p.apvts, ParamIDs::interval, { "3RD UP", "5TH UP", "3RD+5TH", "3RD DN" }, Design::cyan),
          key (*this, p.apvts, ParamIDs::key, { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, Design::cyan),
          scale (*this, p.apvts, ParamIDs::scale, { "MAJOR", "MINOR" }, Design::cyan)
    {
        addAndMakeVisible (scope);

        for (auto* knob : { &voices, &spread, &highHype, &tone, &dirt, &output, &echo, &reverb, &duck, &harmony })
            addAndMakeVisible (*knob);

        for (int i = 0; i < processor.getNumPrograms(); ++i)
            presetBox.addItem (processor.getProgramName (i), i + 1);

        presetBox.setSelectedItemIndex (processor.getCurrentProgram(), juce::dontSendNotification);
        presetBox.onChange = [this] { processor.setCurrentProgram (presetBox.getSelectedItemIndex()); };
        addAndMakeVisible (presetBox);

        addAndMakeVisible (activeSwitch);
        activeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (processor.apvts, ParamIDs::active, activeSwitch);
        activeSwitch.onStateChange = [this] { repaint (1000, 20, 160, 50); };

        throwButton.setButtonText ("THROW");
        throwButton.setClickingTogglesState (true);
        throwButton.getProperties().set ("throw", true);
        throwButton.setTooltip ("Sends the vocal into a big echo and reverb tail while it's on. Automate it to throw single words.");
        addAndMakeVisible (throwButton);
        throwAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (processor.apvts, ParamIDs::throwOn, throwButton);

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
        paintHarmonyPanel (g);

        drawLabel (g, "STACK BUDDY  V" + juce::String (VERSION), { 34, 666, 300, 16 }, Design::mono (10.0f), Design::textFaint,
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

        place (echo, 862, 401);
        place (reverb, 1030, 401);
        place (duck, 1105, 401);
        throwButton.setBounds (1040, 309, 104, 44);

        for (int i = 0; i < timing.size(); ++i)
            timing[i].setBounds (925, 413 + i * 24, 50, 20);

        place (harmony, 330, 557);

        for (int i = 0; i < interval.size(); ++i)
            interval[i].setBounds (410 + i * 82, 592, 76, 28);

        for (int i = 0; i < key.size(); ++i)
            key[i].setBounds (770 + (i % 6) * 34, 588 + (i / 6) * 28, 30, 24);

        for (int i = 0; i < scale.size(); ++i)
            scale[i].setBounds (970 + i * 82, 592, 76, 28);
    }

private:
    static inline const Rect inputPanel { 33.0f, 102.0f, 1119.0f, 180.0f };
    static inline const Rect stackPanelArea { 33.0f, 293.0f, 464.0f, 244.0f };
    static inline const Rect colorPanelArea { 508.0f, 293.0f, 294.0f, 244.0f };
    static inline const Rect spacePanelArea { 813.0f, 293.0f, 339.0f, 244.0f };
    static inline const Rect harmonyPanelArea { 33.0f, 548.0f, 1119.0f, 112.0f };
    static inline const juce::Rectangle<int> bubbleArea { 330, 306, 166, 54 };
    static inline const juce::Rectangle<int> meterArea { 780, 22, 212, 44 };
    static inline const juce::Rectangle<int> duckReadoutArea { 925, 333, 115, 22 };
    static inline const juce::Rectangle<int> noteReadoutArea { 60, 608, 240, 24 };

    // centres the knob cell on x, with the knob's top edge at y
    void place (KnobCell& cell, int centreX, int top)
    {
        const auto size = cell.getCellSize();
        cell.setBounds (centreX - size.x / 2, top, size.x, size.y);
    }

    float param (const char* id) const
    {
        return processor.apvts.getRawParameterValue (id)->load();
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

        // logo tile with four level bars, which bounce with the vocal
        const auto tile = Rect (40.0f, 25.0f, 39.0f, 39.0f);
        g.setColour (Design::lime);
        g.fillRoundedRectangle (tile, 10.0f);

        const float barHeights[] = { 12.0f, 23.0f, 17.0f, 9.0f };
        g.setColour (Design::outerBackground);

        for (int i = 0; i < 4; ++i)
        {
            const auto bounce = 1.0f + inputLevel * 0.45f * std::sin (phase * 9.0f + static_cast<float> (i) * 1.7f);
            const auto height = juce::jlimit (4.0f, 27.0f, barHeights[i] * bounce);
            const auto x = tile.getX() + 8.5f + static_cast<float> (i) * 6.6f;
            g.fillRoundedRectangle (x, tile.getCentreY() - height * 0.5f, 3.6f, height, 1.8f);
        }

        drawLabel (g, "STACK BUDDY", { 92, 24, 260, 28 }, Design::bold (24.0f).withExtraKerningFactor (-0.02f), Design::text,
                   juce::Justification::centredLeft);
        drawLabel (g, "BY PLAYTIME AUDIO", { 92, 47, 260, 14 }, Design::mono (10.5f), Design::textDim, juce::Justification::centredLeft);

        drawLabel (g, "PRESET", { 548, 36, 60, 16 }, Design::mono (10.5f, true), Design::textDim, juce::Justification::centredLeft);

        paintMeter (g);

        drawLabel (g, "STACK", { 1052, 28, 100, 16 }, Design::bold (11.2f).withExtraKerningFactor (0.06f), Design::text,
                   juce::Justification::centredLeft);
        const auto on = activeSwitch.getToggleState();
        drawLabel (g, on ? "ON" : "BYPASSED", { 1052, 44, 100, 14 }, Design::mono (10.0f, true), on ? Design::lime : Design::textFaint,
                   juce::Justification::centredLeft);
    }

    // stereo output meter: lime, turning red near the top, with a peak-hold tick
    void paintMeter (juce::Graphics& g)
    {
        drawLabel (g, "OUT", { 785, 33, 30, 22 }, Design::mono (10.5f, true), Design::textDim, juce::Justification::centredLeft);

        const auto x = 818.0f;
        const auto width = 164.0f;

        for (int channel = 0; channel < 2; ++channel)
        {
            const auto y = 37.0f + static_cast<float> (channel) * 9.0f;
            g.setColour (Design::knobTrack);
            g.fillRoundedRectangle (x, y, width, 5.0f, 2.5f);

            const auto level = meterLevels[static_cast<size_t> (channel)];

            if (level > 0.0f)
            {
                g.setGradientFill (juce::ColourGradient::horizontal (Design::lime, x, Design::red, x + width));
                g.fillRoundedRectangle (x, y, juce::jmax (5.0f, width * level), 5.0f, 2.5f);
            }

            const auto hold = meterHolds[static_cast<size_t> (channel)];

            if (hold > 0.0f)
            {
                g.setColour (hold > 0.93f ? Design::red : Design::text);
                g.fillRect (x + width * hold - 1.0f, y - 1.0f, 2.0f, 7.0f);
            }
        }
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
        paintBubbles (g);
    }

    // The stack as bubbles: copies fan out with Spread and drift, everything swells with the vocal,
    // the octave copy floats above and the harmonies sit to the left in their own colour.
    void paintBubbles (juce::Graphics& g)
    {
        const auto count = juce::jlimit (1, StackEngine::maxStack, juce::roundToInt (param (ParamIDs::stack)));
        const auto fan = param (ParamIDs::spread);
        const auto octave = param (ParamIDs::octave);
        const auto harmonyAmount = param (ParamIDs::harmony);
        const auto step = 4.0f + fan * 6.0f;
        const auto cx = 450.0f;
        const auto cy = 333.0f;
        const auto radius = 9.0f * (1.0f + 0.22f * inputLevel);

        for (int i = count - 1; i >= 1; --i)
        {
            const auto side = (i % 2 == 1) ? -1.0f : 1.0f;
            const auto drift = 0.4f + fan;
            const auto x = cx + side * static_cast<float> ((i + 1) / 2) * step + std::sin (phase * 1.7f + static_cast<float> (i)) * 1.3f * drift;
            const auto y = cy + std::cos (phase * 1.3f + static_cast<float> (i) * 0.9f) * 1.1f * drift;
            g.setColour (Design::lime.withAlpha (0.4f + 0.3f * inputLevel));
            g.drawEllipse (x - radius, y - radius, radius * 2.0f, radius * 2.0f, 1.2f);
        }

        if (harmonyAmount > 0.01f)
        {
            const auto left = cx - static_cast<float> ((count + 1) / 2) * step - 22.0f;

            for (int i = 0; i < harmonyState.count; ++i)
            {
                const auto x = left - static_cast<float> (i) * 15.0f;
                const auto y = cy + std::sin (phase * 2.1f + static_cast<float> (i) * 2.0f) * 1.5f;
                const auto r = 6.0f * (1.0f + 0.25f * inputLevel);
                g.setColour (Design::cyan.withAlpha (0.25f + 0.6f * harmonyAmount));
                g.fillEllipse (x - r, y - r, r * 2.0f, r * 2.0f);
            }
        }

        if (octave > 0.01f)
        {
            const auto y = cy - 13.0f + std::sin (phase * 2.6f) * 1.0f;
            g.setColour (juce::Colours::white.withAlpha (0.3f + 0.6f * octave));
            g.fillEllipse (cx + 9.0f, y - 4.0f, 8.0f, 8.0f);
        }

        // the original voice, with a glow that grows with the input
        if (inputLevel > 0.01f)
        {
            const auto glow = radius * (1.5f + 0.6f * inputLevel);
            g.setColour (Design::lime.withAlpha (0.18f * inputLevel));
            g.fillEllipse (cx - glow, cy - glow, glow * 2.0f, glow * 2.0f);
        }

        g.setColour (Design::lime);
        g.fillEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
    }

    void paintColorPanel (juce::Graphics& g)
    {
        paintPanel (g, colorPanelArea, Design::colorPanel, Design::colorPanelBorder);
        paintPanelHeader (g, colorPanelArea, "02", "Shape it", "Color", Design::text);
    }

    void paintSpacePanel (juce::Graphics& g)
    {
        paintPanel (g, spacePanelArea, Design::spacePanel, Design::spacePanelBorder);

        // while throwing, the panel's edge pulses
        if (throwButton.getToggleState())
        {
            g.setColour (Design::purple.withAlpha (0.45f + 0.3f * std::sin (phase * 7.0f)));
            g.drawRoundedRectangle (spacePanelArea.reduced (1.0f), 16.0f, 2.0f);
        }

        paintPanelHeader (g, spacePanelArea, "03", "Send it back", "Space", Design::text);

        if (param (ParamIDs::duck) > 0.005f)
        {
            const auto reduction = juce::Decibels::gainToDecibels (duckGain, -60.0f);
            const auto ducking = reduction < -0.5f;
            drawLabel (g, ducking ? "DUCK " + juce::String (reduction, 1) + " DB" : juce::String ("DUCK READY"), duckReadoutArea,
                       Design::mono (10.0f, true), ducking ? Design::purple : Design::textFaint, juce::Justification::centredLeft);
        }

        drawLabel (g, "TIMING", { 900, 391, 100, 16 }, Design::bold (12.5f).withExtraKerningFactor (0.06f), Design::text,
                   juce::Justification::centred);
        drawLabel (g, "SYNCED", { 900, 512, 100, 14 }, Design::mono (10.0f), Design::textDim, juce::Justification::centred);
    }

    void paintHarmonyPanel (juce::Graphics& g)
    {
        paintPanel (g, harmonyPanelArea, Design::harmonyPanel, Design::harmonyPanelBorder);

        drawLabel (g, "04", { 49, 578, 24, 16 }, Design::mono (10.5f), Design::textFaint, juce::Justification::centredLeft);
        drawLabel (g, "INSTANT HARMONIES", { 71, 566, 220, 14 }, Design::mono (9.5f, true), Design::textDim, juce::Justification::centredLeft);
        drawLabel (g, "HARMONY", { 71, 581, 220, 28 }, Design::bold (18.5f), Design::cyan, juce::Justification::centredLeft);
        paintNoteReadout (g);

        g.setColour (juce::Colours::white.withAlpha (0.07f));

        for (auto x : { 270.0f, 390.0f, 750.0f, 955.0f })
            g.drawVerticalLine (static_cast<int> (x), 562.0f, 646.0f);

        const auto heading = Design::bold (12.5f).withExtraKerningFactor (0.06f);
        drawLabel (g, "INTERVAL", { 410, 566, 200, 16 }, heading, Design::text, juce::Justification::centredLeft);
        drawLabel (g, "KEY", { 770, 562, 100, 16 }, heading, Design::text, juce::Justification::centredLeft);
        drawLabel (g, "SCALE", { 970, 566, 100, 16 }, heading, Design::text, juce::Justification::centredLeft);
        drawLabel (g, "FOLLOWS YOUR PITCH, SNAPS TO THE KEY", { 410, 628, 330, 14 }, Design::mono (9.5f), Design::textFaint,
                   juce::Justification::centredLeft);
        drawLabel (g, "SET TO YOUR SONG", { 970, 628, 160, 14 }, Design::mono (9.5f), Design::textFaint, juce::Justification::centredLeft);
    }

    // what the singer is on and the notes the harmony is adding
    void paintNoteReadout (juce::Graphics& g)
    {
        const auto area = noteReadoutArea.withTrimmedLeft (11);

        if (harmonyState.sungNote < 0.0f)
        {
            drawLabel (g, "SING TO SEE THE NOTES", area, Design::mono (10.0f, true), Design::textFaint, juce::Justification::centredLeft);
            return;
        }

        const auto sung = juce::roundToInt (harmonyState.sungNote);
        juce::String notes;

        for (int i = 0; i < harmonyState.count; ++i)
            notes << (i > 0 ? dot : juce::String()) << Harmony::noteName (sung + harmonyState.semitones[static_cast<size_t> (i)]);

        const auto font = Design::mono (11.0f, true);
        const auto you = "YOU " + Harmony::noteName (sung);
        drawLabel (g, you, area, font, Design::text, juce::Justification::centredLeft);

        const auto offset = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, you + "  "));
        const auto playing = param (ParamIDs::harmony) > 0.01f;
        drawLabel (g, "+ " + notes, area.withTrimmedLeft (offset), font, playing ? Design::cyan : Design::textDim,
                   juce::Justification::centredLeft);
    }

    void timerCallback() override
    {
        constexpr float frame = 1.0f / 30.0f;
        phase += frame;

        std::array<float, InputScope::maxBars> history {};
        const auto bars = scope.getBarCount();
        processor.getInputHistory (history.data(), bars);

        harmonyState = processor.getHarmonyState();
        scope.setStack (juce::roundToInt (param (ParamIDs::stack)), param (ParamIDs::spread), param (ParamIDs::octave),
                        param (ParamIDs::harmony), harmonyState.count);
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

        // the bubbles and logo follow the newest input level, rising fast and falling smoothly
        const auto latest = levelFromPeak (bars > 0 ? juce::jmax (history[static_cast<size_t> (bars - 1)], bars > 1 ? history[static_cast<size_t> (bars - 2)] : 0.0f) : 0.0f);
        inputLevel = latest > inputLevel ? latest : inputLevel * 0.85f;

        const auto [left, right] = processor.takeOutputPeaks();
        const std::array<float, 2> peaks { left, right };

        for (size_t channel = 0; channel < 2; ++channel)
        {
            const auto db = juce::Decibels::gainToDecibels (peaks[channel], meterFloorDb);
            const auto level = juce::jlimit (0.0f, 1.0f, (db - meterFloorDb) / (meterTopDb - meterFloorDb));
            meterLevels[channel] = juce::jmax (level, meterLevels[channel] - 0.03f);

            if (level >= meterHolds[channel])
            {
                meterHolds[channel] = level;
                holdFrames[channel] = 45;
            }
            else if (--holdFrames[channel] <= 0)
            {
                meterHolds[channel] = juce::jmax (0.0f, meterHolds[channel] - 0.02f);
            }
        }

        duckGain = processor.getDuckGain();

        repaint (bubbleArea);
        repaint (meterArea);
        repaint (36, 22, 46, 46);
        repaint (duckReadoutArea);
        repaint (noteReadoutArea);

        if (throwButton.getToggleState() || wasThrowing)
            repaint (spacePanelArea.toNearestInt());

        wasThrowing = throwButton.getToggleState();
    }

    PluginProcessor& processor;

    InputScope scope;
    juce::ComboBox presetBox;
    juce::ToggleButton activeSwitch;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> activeAttachment;

    KnobCell voices, spread, highHype, tone, dirt, output, echo, reverb, duck, harmony;
    PillGroup timing, interval, key, scale;

    juce::TextButton throwButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> throwAttachment;
    juce::TooltipWindow tooltips { this, 600 };

    bool hearingSignal = false;
    bool wasThrowing = false;
    float recentPeak = 0.0f;
    float phase = 0.0f;
    float inputLevel = 0.0f;
    float duckGain = 1.0f;
    std::array<float, 2> meterLevels {}, meterHolds {};
    std::array<int, 2> holdFrames {};
    PluginProcessor::HarmonyState harmonyState;
};

//==============================================================================
PluginEditor::PluginEditor (PluginProcessor& p)
    : AudioProcessorEditor (&p),
      resizer (this, &constrainer)
{
    setLookAndFeel (&lookAndFeel);

    content = std::make_unique<Content> (p);
    addAndMakeVisible (*content);
    addAndMakeVisible (resizer);

    // our own corner handle (drawn in the design's style) keeps the window at the design's shape
    constrainer.setMinimumSize (590, 345);
    constrainer.setMaximumSize (1770, 1035);
    constrainer.setFixedAspectRatio (static_cast<double> (Design::width) / Design::height);
    setConstrainer (&constrainer);
    setResizable (true, false);

    setSize (1000, juce::roundToInt (1000.0 * Design::height / Design::width));
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
