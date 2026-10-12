#pragma once

#include "../PluginProcessor.h"

/** Small level meter for the sidechain key, with a warning when the sidechain is on but nothing is routed. */
class KeyMeter : public juce::Component, private juce::Timer
{
public:
    explicit KeyMeter (PluginProcessor& p) : processor (p)
    {
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff14161a));
        g.fillRoundedRectangle (area, 3.0f);

        const auto db = juce::Decibels::gainToDecibels (shown, -60.0f);
        const auto fill = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        const auto sidechainOn = processor.apvts.getRawParameterValue (ParamIDs::sidechain)->load() >= 0.5f;
        const auto warning = sidechainOn && ! processor.isKeyConnected();

        g.setColour (warning ? juce::Colour (0xffff9f6b) : juce::Colour (0xff4fd1c5).withAlpha (sidechainOn ? 1.0f : 0.35f));
        g.fillRoundedRectangle (area.withWidth (area.getWidth() * fill), 3.0f);

        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.setFont (10.0f);
        g.drawText (warning ? "no key" : "key", getLocalBounds(), juce::Justification::centred, false);
    }

private:
    void timerCallback() override
    {
        const auto level = processor.getKeyLevel();
        shown = level > shown ? level : shown * 0.85f;
        repaint();
    }

    PluginProcessor& processor;
    float shown = 0.0f;
};
