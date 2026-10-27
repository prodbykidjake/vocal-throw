#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

// Draws the knobs, switch, buttons and preset box the way the Figma design shows them.
class DesignLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DesignLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPosProportional,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool isHighlighted, bool isDown) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool isHighlighted, bool isDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool isHighlighted, bool isDown) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getPopupMenuFont() override;

    void drawCornerResizer (juce::Graphics&, int width, int height, bool isMouseOver, bool isMouseDragging) override;
};

// The on-screen keyboard: rounded keys that light up when played.
class StackKeyboard : public juce::MidiKeyboardComponent
{
public:
    StackKeyboard (juce::MidiKeyboardState&);

protected:
    void drawWhiteNote (int midiNoteNumber, juce::Graphics&, juce::Rectangle<float> area, bool isDown, bool isOver,
                        juce::Colour lineColour, juce::Colour textColour) override;
    void drawBlackNote (int midiNoteNumber, juce::Graphics&, juce::Rectangle<float> area, bool isDown, bool isOver,
                        juce::Colour noteFillColour) override;
};
