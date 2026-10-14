#include "Controls.h"
#include "Fonts.h"
#include "Icons.h"
#include "VoxLookAndFeel.h"

namespace vox
{
namespace
{
    juce::Colour rgba (int r, int g, int b, float a) { return juce::Colour::fromFloatRGBA ((float) r / 255.0f, (float) g / 255.0f, (float) b / 255.0f, a); }

    void prepareParameterSlider (juce::Slider& slider)
    {
        slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters (VoxLookAndFeel::rotaryStart, VoxLookAndFeel::rotaryEnd, true);
        slider.setMouseDragSensitivity (180);
        slider.setScrollWheelEnabled (true);
    }
} // namespace

namespace fmt
{
    juce::String percent (double v) { return juce::String (juce::roundToInt (v)) + "%"; }

    juce::String decibels (double v)
    {
        auto text = juce::String (v, 1);
        if (text == "-0.0")
            text = "0.0";
        return text + " dB";
    }

    juce::String balance (double v)
    {
        const auto n = juce::roundToInt (v);
        return n == 0 ? "EVEN" : (n > 0 ? "+" : juce::String()) + juce::String (n);
    }

    juce::String hertz (double v)
    {
        return v >= 1000.0 ? juce::String (v / 1000.0, 1) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
    }

    juce::String number (double v) { return juce::String (v, 1); }
} // namespace fmt

void drawPill (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, float fontPx)
{
    g.setColour (rgba (237, 245, 248, 0.7f));
    g.fillRoundedRectangle (area, 4.0f);
    g.setColour (rgba (255, 255, 255, 0.38f));
    g.drawLine (area.getX() + 3.0f, area.getY() + 0.5f, area.getRight() - 3.0f, area.getY() + 0.5f, 1.0f);
    fonts::draw (g, text, area, fonts::make (400, fontPx), juce::Colour (0xff3e515b));
}

//==============================================================================
Knob::Knob (Theme& t, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& captionText,
    Format formatToUse, bool isLarge, bool onDeepRail)
    : theme (t),
      caption (captionText),
      format (std::move (formatToUse)),
      large (isLarge),
      attachment (*state.getParameter (paramID), slider, state.undoManager)
{
    prepareParameterSlider (slider);
    slider.getProperties().set ("bg", (int) (onDeepRail ? theme.p().railDeep : theme.p().rail).getARGB());
    slider.setDoubleClickReturnValue (true, state.getParameter (paramID)->convertFrom0to1 (state.getParameter (paramID)->getDefaultValue()));
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);
    setOpaque (false);
}

void Knob::resized()
{
    const auto captionHeight = large ? 18.0f : 15.0f;
    const auto size = large ? 174.0f : 72.0f;
    const auto top = captionHeight + (large ? 2.0f : 0.0f) + 5.0f;
    slider.setBounds (juce::Rectangle<float> (size, size).withCentre ({ (float) getWidth() * 0.5f, top + size * 0.5f }).toNearestInt());
}

void Knob::paint (juce::Graphics& g)
{
    // the ring sits on the rail, which changes with delta and bypass
    const auto deep = (bool) slider.getProperties().contains ("bg") && juce::Colour ((juce::uint32) (int) slider.getProperties()["bg"]) != theme.p().rail;
    slider.getProperties().set ("bg", (int) (deep ? theme.p().railDeep : theme.p().rail).getARGB());

    const auto w = (float) getWidth();
    const auto captionHeight = large ? 18.0f : 15.0f;
    fonts::draw (g, caption, { 0.0f, 0.0f, w, captionHeight }, fonts::make (700, large ? 12.0f : 10.0f, 0.08f), theme.adj (juce::Colour (0xff58707c)));

    const auto pillW = large ? 98.0f : 70.0f;
    const auto pillTop = (float) slider.getBottom() + (large ? 4.0f : 5.0f);
    drawPill (g, juce::Rectangle<float> (pillW, 28.0f).withCentre ({ w * 0.5f, pillTop + 14.0f }), format (slider.getValue()), large ? 16.0f : 13.0f);
}

//==============================================================================
ValueSlider::ValueSlider (Theme& t, juce::AudioProcessorValueTreeState& state, const juce::String& paramID,
    const juce::String& captionText, Format formatToUse)
    : theme (t), caption (captionText), format (std::move (formatToUse)), undo (state.undoManager)
{
    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);
    bindTo (state, paramID);
}

void ValueSlider::bindTo (juce::AudioProcessorValueTreeState& state, const juce::String& paramID)
{
    attachment.reset();
    if (auto* p = state.getParameter (paramID))
    {
        attachment = std::make_unique<juce::SliderParameterAttachment> (*p, slider, undo);
        slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
    }
    repaint();
}

void ValueSlider::setAccent (juce::Colour colour)
{
    slider.getProperties().set ("accent", (int) colour.getARGB());
    repaint();
}

void ValueSlider::setEnabledLook (bool shouldBeEnabled)
{
    setAlpha (shouldBeEnabled ? 1.0f : 0.35f);
    slider.setEnabled (shouldBeEnabled);
}

void ValueSlider::resized()
{
    // caption 13.5 + gap 5, then a 10 px band around the 4 px track (3 px margins)
    slider.setBounds (0, 18, getWidth(), 20);
}

void ValueSlider::paint (juce::Graphics& g)
{
    const auto w = (float) getWidth();
    fonts::draw (g, caption, { 0.0f, 0.0f, w, 13.5f }, fonts::make (600, 9.0f, 0.06f), theme.adj (juce::Colour (0xff667c87)), juce::Justification::centredLeft);
    drawPill (g, { 0.0f, 33.5f + 0.0f, w, 27.0f }, format (slider.getValue()), 11.0f);
}

//==============================================================================
RoundToggle::RoundToggle (Theme& t, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& label)
    : juce::Button (label), theme (t), attachment (*state.getParameter (paramID), *this, state.undoManager)
{
    setClickingTogglesState (true);
}

void RoundToggle::paintButton (juce::Graphics& g, bool, bool)
{
    const auto& pal = theme.p();
    const auto circle = juce::Rectangle<float> (17.0f, 17.0f).withCentre ({ 4.0f + 8.5f, (float) getHeight() * 0.5f });

    if (getToggleState())
    {
        g.setColour (pal.coral);
        g.fillEllipse (circle);
        g.setColour (juce::Colours::white);
        g.fillEllipse (circle.reduced (5.0f));
    }
    else
    {
        g.setColour (rgba (246, 250, 251, 0.8f));
        g.drawEllipse (circle.reduced (1.0f), 2.0f);
    }

    fonts::draw (g, getButtonText(), { circle.getRight() + 8.0f, 0.0f, (float) getWidth() - circle.getRight() - 8.0f, (float) getHeight() },
        fonts::make (600, 10.0f, 0.06f), theme.adj (juce::Colour (0xff5c717c)), juce::Justification::centredLeft);
}

//==============================================================================
SegmentedControl::SegmentedControl (Theme& t, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, juce::StringArray itemNames)
    : theme (t),
      items (std::move (itemNames)),
      attachment (*state.getParameter (paramID), [this] (float v) { selected = juce::roundToInt (v); repaint(); }, state.undoManager)
{
    attachment.sendInitialUpdate();
}

void SegmentedControl::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setColour (rgba (111, 143, 156, 0.18f));
    g.fillRoundedRectangle (area, 7.0f);

    const auto n = juce::jmax (1, items.size());
    const auto cell = area.getWidth() / (float) n;
    for (int i = 0; i < items.size(); ++i)
    {
        const auto r = juce::Rectangle<float> (area.getX() + cell * (float) i, area.getY(), cell, area.getHeight());
        const auto active = i == selected;
        if (active)
        {
            juce::Path p;
            p.addRoundedRectangle (r, 7.0f);
            juce::DropShadow (rgba (70, 96, 107, 0.12f), 10, { 0, 3 }).drawForPath (g, p);
            g.setColour (rgba (249, 252, 253, 0.94f));
            g.fillRoundedRectangle (r, 7.0f);
        }
        fonts::draw (g, items[i], r, fonts::make (600, 11.0f, 0.07f), theme.adj (active ? juce::Colour (0xff3f5965) : juce::Colour (0xff627782)));
    }
}

void SegmentedControl::mouseDown (const juce::MouseEvent& e)
{
    const auto n = juce::jmax (1, items.size());
    const auto index = juce::jlimit (0, n - 1, (int) (e.position.x / ((float) getWidth() / (float) n)));
    attachment.setValueAsCompleteGesture ((float) index);
}

//==============================================================================
ChoiceButton::ChoiceButton (Theme& t, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, juce::StringArray itemNames,
    bool withChevron, float px, int fontWeight)
    : theme (t),
      items (std::move (itemNames)),
      chevron (withChevron),
      fontPx (px),
      weight (fontWeight),
      attachment (*state.getParameter (paramID), [this] (float v) { selected = juce::roundToInt (v); repaint(); }, state.undoManager)
{
    attachment.sendInitialUpdate();
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void ChoiceButton::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    if (chevron)
    {
        g.setColour (rgba (233, 242, 245, 0.33f));
        g.fillRoundedRectangle (area, 4.0f);
        fonts::draw (g, items[selected].toUpperCase(), area.withTrimmedLeft (9.0f), fonts::make (400, fontPx, 0.0f), theme.adj (juce::Colour (0xff5a707b)), juce::Justification::centredLeft);
        icons::stroke (g, icons::chevron (true), juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ area.getRight() - 14.0f, area.getCentreY() }),
            theme.adj (juce::Colour (0xff5a707b)), 1.4f);
    }
    else
    {
        g.setColour (rgba (239, 246, 248, 0.72f));
        g.fillRoundedRectangle (area, 4.0f);
        fonts::draw (g, items[selected], area, fonts::make (weight, fontPx), theme.adj (juce::Colour (0xff3f5661)));
    }
}

void ChoiceButton::mouseDown (const juce::MouseEvent&)
{
    attachment.setValueAsCompleteGesture ((float) ((selected + 1) % juce::jmax (1, items.size())));
}

//==============================================================================
CircleIconToggle::CircleIconToggle (Theme& t, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, Icon i,
    const juce::String& tooltip)
    : juce::Button (paramID), theme (t), icon (i), attachment (*state.getParameter (paramID), *this, state.undoManager)
{
    setClickingTogglesState (true);
    setTooltip (tooltip);
}

void CircleIconToggle::paintButton (juce::Graphics& g, bool over, bool)
{
    const auto& pal = theme.p();
    const auto circle = getLocalBounds().toFloat().withSizeKeepingCentre (37.0f, 37.0f);
    const auto on = getToggleState();

    if (on)
    {
        g.setColour (pal.accent.withAlpha (0.17f));
        g.fillEllipse (circle.expanded (4.0f));
        g.setColour (pal.accent);
        g.fillEllipse (circle);
    }
    else if (over)
    {
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.fillEllipse (circle);
    }

    const auto iconColour = on ? juce::Colours::white : rgba (242, 249, 250, 0.72f);
    const auto iconArea = circle.withSizeKeepingCentre (17.0f, 17.0f);
    icons::stroke (g, icon == Icon::power ? icons::power() : icons::keyListen(), iconArea, iconColour, 1.6f);
}
} // namespace vox
