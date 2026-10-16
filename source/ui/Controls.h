#pragma once

#include "Theme.h"
#include <juce_audio_processors/juce_audio_processors.h>

/** The design's controls. All are sized in design pixels and read their colours from the Theme when they
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

/** Draws the rounded value read-out used under knobs and sliders. */
void drawPill (juce::Graphics& g, const Theme& theme, juce::Rectangle<float> area, const juce::String& text, float fontPx);

/** A knob with its caption above and a value pill below. */
class Knob : public juce::Component
{
public:
    Knob (Theme& theme, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& caption,
        Format format, bool large = false, bool onDeepRail = false);

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::Slider& getSlider() noexcept { return slider; }

    static int heightFor (bool large) noexcept { return large ? 231 : 125; }

private:
    Theme& theme;
    juce::Slider slider;
    juce::String caption;
    Format format;
    bool large;
    bool deepRail;
    juce::SliderParameterAttachment attachment;
};

/** A caption, a thin slider and a value pill (the design's ".value-slider"). */
class ValueSlider : public juce::Component
{
public:
    ValueSlider (Theme& theme, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& caption,
        Format format);

    /** Overrides the thumb colour (the band editor uses the selected band's colour). */
    void setAccent (juce::Colour colour);
    /** Re-binds to another parameter (the band editor follows the selected band). */
    void bindTo (juce::AudioProcessorValueTreeState& state, const juce::String& paramID);

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::Slider& getSlider() noexcept { return slider; }
    void setEnabledLook (bool shouldBeEnabled);

    static constexpr int height = 61;

private:
    Theme& theme;
    juce::Slider slider;
    juce::String caption;
    Format format;
    std::unique_ptr<juce::SliderParameterAttachment> attachment;
    juce::UndoManager* undo = nullptr;
};

/** A ring that fills with coral when on (DELTA, BYPASS). */
class RoundToggle : public juce::Button
{
public:
    RoundToggle (Theme& theme, juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& label);

    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

private:
    Theme& theme;
    juce::ButtonParameterAttachment attachment;
};

/** Two or more labelled segments bound to a choice parameter (SOFT | HARD). */
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
