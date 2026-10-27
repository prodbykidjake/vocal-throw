#pragma once

#include "Theme.h"
#include <juce_audio_processors/juce_audio_processors.h>

/** The editor's controls. All are sized in design pixels and read their colours from the Theme when they
    paint, so delta and bypass restyle them without being rebuilt. Each one is bound to a parameter. */
namespace vox
{
using Format = std::function<juce::String (double)>;

namespace fmt
{
    juce::String percent (double v);     // "40%"
    juce::String decibels (double v);    // "-3.0 dB"
    juce::String balance (double v);     // "EVEN", "+12", "-20"
    juce::String hertz (double v);       // "850 Hz", "4.6 kHz"
    juce::String number (double v);      // "3.7"
    juce::String signedPercent (double v); // "+30%", "-20%", "0%"
}

/** A knob: a glowing arc with the value in its middle and the caption underneath. */
class Knob : public juce::Component
{
public:
    enum class Size { small, medium, large };

    Knob (Theme& theme, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& caption,
        Format format, Size size = Size::small);

    /** Fills the arc from the centre (for signed values such as the stereo balance). */
    void setBipolar (bool shouldBeBipolar);

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    juce::Slider& getSlider() noexcept { return slider; }

    static constexpr int sizeFor (Size s) noexcept { return s == Size::large ? 176 : (s == Size::medium ? 110 : 74); }
    static constexpr int heightFor (Size s) noexcept { return sizeFor (s) + 20; }

private:
    Theme& theme;
    juce::Slider slider;
    juce::String caption;
    Format format;
    Size size;
    juce::SliderParameterAttachment attachment;
};

/** A caption and value on one line over a hairline slider. */
class ValueSlider : public juce::Component
{
public:
    ValueSlider (Theme& theme, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& caption,
        Format format);

    /** Overrides the fill colour (the band editor uses the selected band's colour). */
    void setAccent (juce::Colour colour);
    /** Fills from the centre (for signed values). */
    void setBipolar (bool shouldBeBipolar);
    /** Re-binds to another parameter (the band editor follows the selected band). */
    void bindTo (juce::AudioProcessorValueTreeState& state, const juce::String& paramID);

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::Slider& getSlider() noexcept { return slider; }
    void setEnabledLook (bool shouldBeEnabled);

    static constexpr int height = 38;

private:
    Theme& theme;
    juce::Slider slider;
    juce::String caption;
    Format format;
    std::unique_ptr<juce::SliderParameterAttachment> attachment;
    juce::UndoManager* undo = nullptr;
};

/** A ring that lights up when on (delta, bypass). */
class RoundToggle : public juce::Button
{
public:
    RoundToggle (Theme& theme, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& label);

    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

private:
    Theme& theme;
    juce::ButtonParameterAttachment attachment;
};

/** Two or more labelled segments bound to a choice parameter (soft | hard). */
class SegmentedControl : public juce::Component
{
public:
    SegmentedControl (Theme& theme, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, juce::StringArray items);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    Theme& theme;
    juce::StringArray items;
    int selected = 0;
    juce::ParameterAttachment attachment;
};

/** A button showing a choice parameter's current value; clicking steps to the next one (L/R <-> M/S). */
class ChoiceButton : public juce::Component, public juce::SettableTooltipClient
{
public:
    ChoiceButton (Theme& theme, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, juce::StringArray items,
        bool chevron = false, float fontPx = 12.0f, int weight = 600);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    Theme& theme;
    juce::StringArray items;
    int selected = 0;
    bool chevron;
    float fontPx;
    int weight;
    juce::ParameterAttachment attachment;
};

/** A round icon button bound to a bool parameter (sidechain, key listen). */
class CircleIconToggle : public juce::Button
{
public:
    enum class Icon { power, keyListen };

    CircleIconToggle (Theme& theme, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, Icon icon,
        const juce::String& tooltip);

    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

private:
    Theme& theme;
    Icon icon;
    juce::ButtonParameterAttachment attachment;
};
} // namespace vox
