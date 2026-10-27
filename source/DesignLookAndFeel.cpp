#include "DesignLookAndFeel.h"
#include "DesignTokens.h"

DesignLookAndFeel::DesignLookAndFeel()
{
    setColour (juce::ComboBox::textColourId, Design::text);
    setColour (juce::PopupMenu::backgroundColourId, Design::control);
    setColour (juce::PopupMenu::textColourId, Design::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Design::lime);
    setColour (juce::PopupMenu::highlightedTextColourId, Design::outerBackground);
}

//==============================================================================
void DesignLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                          float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y),
                                                static_cast<float> (width), static_cast<float> (height)).reduced (1.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
    const auto arcRadius = radius - 3.0f;
    const auto stroke = juce::jmax (3.0f, radius * 0.1f);
    const auto angle = startAngle + sliderPos * (endAngle - startAngle);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (Design::knobTrack);
    g.strokePath (track, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (sliderPos > 0.003f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
        g.setColour (accent.withAlpha (slider.isEnabled() ? 1.0f : 0.4f));
        g.strokePath (value, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const auto bodyRadius = radius * 0.74f;

    // soft shadow under the knob
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillEllipse (centre.x - bodyRadius, centre.y - bodyRadius + 3.0f, bodyRadius * 2.0f, bodyRadius * 2.0f);

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2d2f2a), centre.x, centre.y - bodyRadius,
                                             juce::Colour (0xff181a16), centre.x, centre.y + bodyRadius, false));
    g.fillEllipse (centre.x - bodyRadius, centre.y - bodyRadius, bodyRadius * 2.0f, bodyRadius * 2.0f);

    g.setColour (juce::Colour (0xff3d4038));
    g.drawEllipse (centre.x - bodyRadius, centre.y - bodyRadius, bodyRadius * 2.0f, bodyRadius * 2.0f, 1.2f);

    const auto inner = bodyRadius * 0.38f;
    const auto outer = bodyRadius * 0.86f;
    const juce::Line<float> pointer (centre.x + std::sin (angle) * inner, centre.y - std::cos (angle) * inner,
                                     centre.x + std::sin (angle) * outer, centre.y - std::cos (angle) * outer);
    g.setColour (accent);
    g.drawLine (pointer, juce::jmax (2.5f, radius * 0.06f));
}

//==============================================================================
void DesignLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool isHighlighted, bool)
{
    const auto area = button.getLocalBounds().toFloat().reduced (1.0f);
    const auto on = button.getToggleState();

    g.setColour (on ? juce::Colour (0xff3b4a21) : Design::knobTrack);
    g.fillRoundedRectangle (area, area.getHeight() * 0.5f);
    g.setColour (on ? Design::lime.withAlpha (0.6f) : Design::controlBorder);
    g.drawRoundedRectangle (area, area.getHeight() * 0.5f, 1.0f);

    const auto knobSize = area.getHeight() - 5.0f;
    const auto knobX = on ? area.getRight() - knobSize - 2.5f : area.getX() + 2.5f;
    g.setColour (on ? Design::lime : Design::textDim);
    g.fillEllipse (knobX, area.getY() + 2.5f, knobSize, knobSize);

    if (isHighlighted)
    {
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRoundedRectangle (area, area.getHeight() * 0.5f);
    }
}

void DesignLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                              bool isHighlighted, bool isDown)
{
    const auto area = button.getLocalBounds().toFloat().reduced (0.5f);
    const auto isPill = button.getProperties().contains ("pill");
    const auto selected = isPill && button.getToggleState();
    const auto radius = isPill ? 6.0f : 7.0f;

    juce::Colour fill = Design::control;
    juce::Colour border = Design::controlBorder;

    if (selected)
    {
        fill = juce::Colour (0xff2d2848);
        border = Design::purple;
    }
    else if (isHighlighted || isDown)
    {
        fill = fill.brighter (0.08f);
    }

    if (! button.isEnabled())
        fill = fill.withAlpha (0.5f);

    g.setColour (fill);
    g.fillRoundedRectangle (area, radius);
    g.setColour (border);
    g.drawRoundedRectangle (area, radius, 1.0f);
}

void DesignLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool)
{
    const auto isPill = button.getProperties().contains ("pill");
    const auto selected = isPill && button.getToggleState();

    g.setFont (Design::mono (isPill ? 11.0f : 10.5f, true));
    g.setColour (selected ? Design::text : (button.isEnabled() ? Design::textDim : Design::textFaint));
    g.drawText (button.getButtonText(), button.getLocalBounds(), juce::Justification::centred);
}

//==============================================================================
void DesignLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto area = juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (width), static_cast<float> (height)).reduced (0.5f);
    g.setColour (Design::control);
    g.fillRoundedRectangle (area, 11.0f);
    g.setColour (box.hasKeyboardFocus (true) ? Design::lime : Design::controlBorder);
    g.drawRoundedRectangle (area, 11.0f, 1.0f);

    const auto cx = area.getRight() - 22.0f;
    const auto cy = area.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath (cx - 5.0f, cy - 2.5f);
    chevron.lineTo (cx, cy + 2.5f);
    chevron.lineTo (cx + 5.0f, cy - 2.5f);
    g.setColour (Design::text);
    g.strokePath (chevron, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font DesignLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return Design::bold (17.0f);
}

void DesignLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (16, 1, box.getWidth() - 46, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

juce::Font DesignLookAndFeel::getPopupMenuFont()
{
    return Design::medium (16.0f);
}

void DesignLookAndFeel::drawCornerResizer (juce::Graphics& g, int width, int height, bool isMouseOver, bool isMouseDragging)
{
    g.setColour (isMouseOver || isMouseDragging ? Design::lime : Design::textFaint);

    for (int i = 0; i < 3; ++i)
    {
        const auto offset = static_cast<float> (i) * 4.5f;
        g.drawLine (static_cast<float> (width) - 5.0f - offset, static_cast<float> (height) - 3.0f,
                    static_cast<float> (width) - 3.0f, static_cast<float> (height) - 5.0f - offset, 1.4f);
    }
}

//==============================================================================
StackKeyboard::StackKeyboard (juce::MidiKeyboardState& keyboardState)
    : MidiKeyboardComponent (keyboardState, horizontalKeyboard)
{
    // the component fills behind the keys with this, so make it match the panel
    setColour (whiteNoteColourId, Design::sourceBackground);
    setColour (blackNoteColourId, juce::Colour (0xff10110e));
    setColour (keySeparatorLineColourId, Design::sourceBackground);
    setColour (shadowColourId, juce::Colours::transparentBlack);
    setScrollButtonsVisible (false);
    setBlackNoteLengthProportion (0.62f);
}

void StackKeyboard::drawWhiteNote (int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area, bool isDown,
                                   bool, juce::Colour, juce::Colour)
{
    auto key = area.reduced (1.5f, 0.0f);
    key.removeFromTop (0.0f);

    juce::Path shape;
    shape.addRoundedRectangle (key.getX(), key.getY(), key.getWidth(), key.getHeight() - 1.0f, 8.0f, 8.0f, false, false, true, true);

    if (isDown)
        g.setGradientFill (juce::ColourGradient (Design::lime.brighter (0.2f), 0.0f, key.getY(), Design::lime.darker (0.15f), 0.0f, key.getBottom(), false));
    else
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe9e7e1), 0.0f, key.getY(), juce::Colour (0xffd2d0c9), 0.0f, key.getBottom(), false));

    g.fillPath (shape);

    if (midiNoteNumber % 12 == 0)
    {
        g.setColour (isDown ? Design::outerBackground : juce::Colour (0xff7d7f74));
        g.setFont (Design::mono (11.0f));
        g.drawText ("C" + juce::String (midiNoteNumber / 12 - 2), key.withTrimmedBottom (10.0f), juce::Justification::centredBottom);
    }
}

void StackKeyboard::drawBlackNote (int, juce::Graphics& g, juce::Rectangle<float> area, bool isDown, bool, juce::Colour)
{
    juce::Path shape;
    shape.addRoundedRectangle (area.getX(), area.getY() - 4.0f, area.getWidth(), area.getHeight() + 4.0f, 6.0f, 6.0f, false, false, true, true);

    if (isDown)
        g.setGradientFill (juce::ColourGradient (Design::lime.darker (0.3f), 0.0f, area.getY(), Design::lime.darker (0.6f), 0.0f, area.getBottom(), false));
    else
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff23251f), 0.0f, area.getY(), juce::Colour (0xff0c0d0a), 0.0f, area.getBottom(), false));

    g.fillPath (shape);
}
