#include "Controls.h"
#include "Fonts.h"
#include "Icons.h"
#include "VoxLookAndFeel.h"

namespace vox
{
namespace
{
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

    juce::String signedPercent (double v)
    {
        const auto n = juce::roundToInt (v);
        return (n > 0 ? "+" : juce::String()) + juce::String (n) + "%";
    }
} // namespace fmt

//==============================================================================
Knob::Knob (Theme& t, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& captionText,
    Format formatToUse, Size sizeToUse)
    : theme (t),
      caption (captionText),
      format (std::move (formatToUse)),
      size (sizeToUse),
      attachment (*state.getParameter (paramID), slider, state.undoManager)
{
    prepareParameterSlider (slider);
    slider.setDoubleClickReturnValue (true, state.getParameter (paramID)->convertFrom0to1 (state.getParameter (paramID)->getDefaultValue()));
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);
    setOpaque (false);
}

void Knob::setBipolar (bool shouldBeBipolar)
{
    if (shouldBeBipolar)
        slider.getProperties().set ("bipolar", true);
    else
        slider.getProperties().remove ("bipolar");
    repaint();
}

void Knob::resized()
{
    const auto d = sizeFor (size);
    slider.setBounds ((getWidth() - d) / 2, 0, d, d);
}

void Knob::paint (juce::Graphics& g)
{
    const auto& pal = theme.p();
    const auto large = size == Size::large;
    const auto top = (float) slider.getBottom() + (large ? 0.0f : 2.0f);
    fonts::draw (g, caption.toLowerCase(), { 0.0f, top, (float) getWidth(), 16.0f }, fonts::make (500, large ? 12.0f : 11.0f, 0.16f), pal.inkSoft);
}

void Knob::paintOverChildren (juce::Graphics& g)
{
    // the value sits in the middle of the arc
    const auto& pal = theme.p();
    const auto centre = slider.getBounds().toFloat().getCentre();
    if (size == Size::large)
        fonts::draw (g, format (slider.getValue()), juce::Rectangle<float> (120.0f, 34.0f).withCentre (centre), fonts::make (500, 26.0f, -0.01f), pal.ink);
    else if (size == Size::medium)
        fonts::draw (g, format (slider.getValue()), juce::Rectangle<float> (90.0f, 24.0f).withCentre (centre), fonts::make (500, 17.0f), pal.ink);
    else
        fonts::draw (g, format (slider.getValue()), juce::Rectangle<float> (64.0f, 18.0f).withCentre (centre), fonts::make (500, 12.0f), pal.ink);
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

void ValueSlider::setBipolar (bool shouldBeBipolar)
{
    if (shouldBeBipolar)
        slider.getProperties().set ("bipolar", true);
    else
        slider.getProperties().remove ("bipolar");
    repaint();
}

void ValueSlider::setEnabledLook (bool shouldBeEnabled)
{
    setAlpha (shouldBeEnabled ? 1.0f : 0.35f);
    slider.setEnabled (shouldBeEnabled);
}

void ValueSlider::resized()
{
    slider.setBounds (0, 18, getWidth(), 20);
}

void ValueSlider::paint (juce::Graphics& g)
{
    const auto& pal = theme.p();
    const auto w = (float) getWidth();
    fonts::draw (g, caption.toLowerCase(), { 0.0f, 0.0f, w, 16.0f }, fonts::make (500, 11.0f, 0.12f), pal.inkSoft, juce::Justification::centredLeft);
    fonts::draw (g, format (slider.getValue()), { 0.0f, 0.0f, w, 16.0f }, fonts::make (500, 12.0f), pal.ink, juce::Justification::centredRight);
}

//==============================================================================
RoundToggle::RoundToggle (Theme& t, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& label)
    : juce::Button (label), theme (t), attachment (*state.getParameter (paramID), *this, state.undoManager)
{
    setClickingTogglesState (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void RoundToggle::paintButton (juce::Graphics& g, bool over, bool)
{
    const auto& pal = theme.p();
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    const auto on = getToggleState();

    // a pill with a status light on the left
    g.setColour (on ? pal.accent.withAlpha (0.12f) : pal.edge.withAlpha (over ? 0.06f : 0.03f));
    g.fillRoundedRectangle (area, area.getHeight() * 0.5f);
    g.setColour (on ? pal.accent.withAlpha (0.55f) : pal.edge.withAlpha (0.1f));
    g.drawRoundedRectangle (area, area.getHeight() * 0.5f, 1.0f);

    const auto light = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ area.getX() + 16.0f, area.getCentreY() });
    if (on && pal.glow > 0.0f)
    {
        g.setColour (pal.accent.withAlpha (0.3f));
        g.fillEllipse (light.expanded (4.0f));
    }
    g.setColour (on ? pal.accent : pal.inkDim);
    g.fillEllipse (light);

    fonts::draw (g, getButtonText().toLowerCase(), area.withTrimmedLeft (28.0f), fonts::make (500, 11.0f, 0.14f), on ? pal.ink : pal.inkSoft,
        juce::Justification::centredLeft);
}

//==============================================================================
SegmentedControl::SegmentedControl (Theme& t, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, juce::StringArray itemNames)
    : theme (t),
      items (std::move (itemNames)),
      attachment (*state.getParameter (paramID), [this] (float v) { selected = juce::roundToInt (v); repaint(); }, state.undoManager)
{
    attachment.sendInitialUpdate();
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void SegmentedControl::paint (juce::Graphics& g)
{
    const auto& pal = theme.p();
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    const auto radius = area.getHeight() * 0.5f;
    g.setColour (pal.edge.withAlpha (0.03f));
    g.fillRoundedRectangle (area, radius);
    g.setColour (pal.edge.withAlpha (0.1f));
    g.drawRoundedRectangle (area, radius, 1.0f);

    const auto n = juce::jmax (1, items.size());
    const auto cell = area.getWidth() / (float) n;
    for (int i = 0; i < items.size(); ++i)
    {
        const auto r = juce::Rectangle<float> (area.getX() + cell * (float) i, area.getY(), cell, area.getHeight());
        const auto active = i == selected;
        if (active)
        {
            const auto pill = r.reduced (3.0f);
            if (pal.glow > 0.0f)
            {
                g.setColour (pal.accent.withAlpha (0.08f));
                g.fillRoundedRectangle (pill.expanded (2.0f), pill.getHeight() * 0.5f + 2.0f);
            }
            g.setColour (pal.accent.withAlpha (0.16f));
            g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
            g.setColour (pal.accent.withAlpha (0.6f));
            g.drawRoundedRectangle (pill, pill.getHeight() * 0.5f, 1.0f);
        }
        fonts::draw (g, items[i].toLowerCase(), r, fonts::make (500, 11.0f, 0.16f), active ? pal.accent : pal.inkSoft);
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
    const auto& pal = theme.p();
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    const auto radius = area.getHeight() * 0.5f;
    g.setColour (pal.edge.withAlpha (isMouseOver() ? 0.06f : 0.03f));
    g.fillRoundedRectangle (area, radius);
    g.setColour (pal.edge.withAlpha (0.1f));
    g.drawRoundedRectangle (area, radius, 1.0f);

    const auto text = items[selected].toLowerCase();
    if (chevron)
    {
        fonts::draw (g, text, area.withTrimmedLeft (14.0f), fonts::make (weight, fontPx, 0.08f), pal.ink, juce::Justification::centredLeft);
        icons::stroke (g, icons::chevron (true), juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ area.getRight() - 16.0f, area.getCentreY() }),
            pal.accent, 1.4f);
    }
    else
    {
        fonts::draw (g, text, area, fonts::make (weight, fontPx, 0.08f), pal.ink);
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
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void CircleIconToggle::paintButton (juce::Graphics& g, bool over, bool)
{
    const auto& pal = theme.p();
    const auto circle = getLocalBounds().toFloat().withSizeKeepingCentre (34.0f, 34.0f);
    const auto on = getToggleState();

    if (on && pal.glow > 0.0f)
    {
        g.setColour (pal.accent.withAlpha (0.1f));
        g.fillEllipse (circle.expanded (4.0f));
    }
    g.setColour (on ? pal.accent.withAlpha (0.14f) : pal.edge.withAlpha (over ? 0.07f : 0.03f));
    g.fillEllipse (circle);
    g.setColour (on ? pal.accent.withAlpha (0.7f) : pal.edge.withAlpha (0.12f));
    g.drawEllipse (circle.reduced (0.5f), 1.0f);

    const auto iconArea = circle.withSizeKeepingCentre (15.0f, 15.0f);
    icons::stroke (g, icon == Icon::power ? icons::power() : icons::keyListen(), iconArea, on ? pal.accent : pal.inkSoft, 1.5f);
}
} // namespace vox
