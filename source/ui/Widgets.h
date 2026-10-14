#pragma once

#include "Theme.h"
#include <juce_audio_processors/juce_audio_processors.h>

class PluginProcessor;

namespace vox
{
/** A text button in the top bar: lower-case label, underlined when active, dimmed when unavailable. */
class TopBarButton : public juce::Button
{
public:
    enum class Kind { text, gear };

    TopBarButton (Theme& theme, const juce::String& label, Kind kind = Kind::text, float fontPx = 13.0f, int weight = 400);

    void setActiveLook (bool active);
    void setMuted (bool muted);
    void setLabel (const juce::String& label);
    float getIdealWidth() const;

    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

private:
    Theme& theme;
    Kind kind;
    float fontPx;
    int weight;
    bool activeLook = false;
    bool muted = false;
};

/** The padlock next to QUALITY, bound to the quality-lock parameter. */
class LockButton : public juce::Button
{
public:
    LockButton (Theme& theme, juce::AudioProcessorValueTreeState& state);
    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

private:
    Theme& theme;
    juce::ButtonParameterAttachment attachment;
};

/** Nine small bars showing the sidechain key level. */
class MiniMeter : public juce::Component, private juce::Timer
{
public:
    MiniMeter (Theme& theme, PluginProcessor& processor);
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    Theme& theme;
    PluginProcessor& processor;
    float shown = 0.0f;
};
} // namespace vox
