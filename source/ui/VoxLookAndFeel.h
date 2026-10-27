#pragma once

#include "Fonts.h"
#include "Theme.h"

namespace vox
{
/** Draws sliders in the editor's style: knobs are thin glowing arcs (the large one gets tick marks and a
    violet-to-mint sweep), linear sliders are hairline tracks with a glowing fill. Both use design pixels;
    the editor scales everything with a transform.

    Slider properties (set with getProperties().set): "bipolar" makes the fill start from the centre,
    "accent" (ARGB int) overrides the colour of a linear slider. */
class VoxLookAndFeel : public juce::LookAndFeel_V4, private juce::ChangeListener
{
public:
    explicit VoxLookAndFeel (Theme& themeToUse);
    ~VoxLookAndFeel() override;

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
    void changeListenerCallback (juce::ChangeBroadcaster*) override { applyThemeColours(); }
    void applyThemeColours();

    Theme& theme;
    juce::SharedResourcePointer<fonts::Typefaces> typefaces; // keeps DM Sans loaded while the UI exists
};
} // namespace vox
