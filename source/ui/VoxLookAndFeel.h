#pragma once

#include "Theme.h"

namespace vox
{
/** Draws sliders the way the design does: soft knobs (a white value ring around a charcoal face) and the
    thin value sliders. Both use design pixels, the editor scales everything with a transform.

    Slider properties (set with getProperties().set): "bg" (ARGB int) is the colour the knob ring sits on,
    "accent" (ARGB int) overrides the thumb colour of a linear slider. */
class VoxLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit VoxLookAndFeel (const Theme& themeToUse);

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPosProportional,
        float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
        float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;

    int getSliderThumbRadius (juce::Slider&) override { return 6; }

    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;

    static constexpr float rotaryStart = juce::MathConstants<float>::pi * 1.25f;
    static constexpr float rotaryEnd = juce::MathConstants<float>::pi * 2.75f;

private:
    const Theme& theme;
};
} // namespace vox
