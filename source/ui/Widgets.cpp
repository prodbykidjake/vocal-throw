#include "Widgets.h"
#include "../PluginProcessor.h"
#include "Fonts.h"
#include "Icons.h"

namespace vox
{
TopBarButton::TopBarButton (Theme& t, const juce::String& label, Kind k, float px, int w)
    : juce::Button (label), theme (t), kind (k), fontPx (px), weight (w)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void TopBarButton::setActiveLook (bool active)
{
    if (activeLook != active)
    {
        activeLook = active;
        repaint();
    }
}

void TopBarButton::setMuted (bool shouldBeMuted)
{
    if (muted != shouldBeMuted)
    {
        muted = shouldBeMuted;
        repaint();
    }
}

void TopBarButton::setLabel (const juce::String& label)
{
    if (getButtonText() != label)
    {
        setButtonText (label);
        repaint();
    }
}

float TopBarButton::getIdealWidth() const
{
    if (kind == Kind::gear)
        return 16.0f;
    return fonts::width (fonts::make (weight, fontPx), getButtonText()) + 8.0f; // padding keeps drawText from dropping glyphs
}

void TopBarButton::paintButton (juce::Graphics& g, bool over, bool)
{
    const auto base = juce::Colour::fromFloatRGBA (244.0f / 255.0f, 249.0f / 255.0f, 250.0f / 255.0f, over ? 1.0f : 0.88f);
    const auto colour = muted ? base.withMultipliedAlpha (0.38f / 0.88f) : base;
    const auto area = getLocalBounds().toFloat();

    if (kind == Kind::gear)
    {
        juce::Path gear = icons::gear();
        const auto box = juce::Rectangle<float> (15.0f, 15.0f).withCentre (area.getCentre());
        g.setColour (colour);
        g.fillPath (gear, juce::AffineTransform::scale (box.getWidth(), box.getHeight()).translated (box.getX(), box.getY()));
        return;
    }

    fonts::draw (g, getButtonText(), area, fonts::make (weight, fontPx), colour);
    if (activeLook)
    {
        g.setColour (juce::Colours::white);
        g.fillRect (area.getX(), area.getCentreY() + 10.0f, area.getWidth(), 1.0f);
    }
}

//==============================================================================
LockButton::LockButton (Theme& t, juce::AudioProcessorValueTreeState& state)
    : juce::Button ("lock"), theme (t), attachment (*state.getParameter (ParamIDs::qualityLock), *this, state.undoManager)
{
    setClickingTogglesState (true);
    setTooltip ("Quality lock: while on, loading a preset keeps your quality setting");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void LockButton::paintButton (juce::Graphics& g, bool over, bool)
{
    const auto colour = theme.adj (over ? juce::Colour (0xff3e515b) : juce::Colour (0xff607884));
    icons::stroke (g, icons::lock (getToggleState()), getLocalBounds().toFloat().withSizeKeepingCentre (15.0f, 15.0f), colour, 1.4f);
}

//==============================================================================
MiniMeter::MiniMeter (Theme& t, PluginProcessor& p) : theme (t), processor (p)
{
    startTimerHz (30);
}

void MiniMeter::timerCallback()
{
    const auto level = processor.getKeyLevel();
    const auto next = level > shown ? level : shown * 0.85f;
    if (std::abs (next - shown) > 1.0e-4f)
    {
        shown = next;
        repaint();
    }
}

void MiniMeter::paint (juce::Graphics& g)
{
    const auto& pal = theme.p();
    const auto on = processor.apvts.getRawParameterValue (ParamIDs::sidechain)->load() >= 0.5f;
    const auto db = juce::Decibels::gainToDecibels (shown, -60.0f);
    const auto lit = on ? juce::roundToInt (juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f) * 9.0f) : 0;

    constexpr int bars = 9;
    constexpr float gap = 2.0f;
    const auto w = ((float) getWidth() - gap * (float) (bars - 1)) / (float) bars;
    for (int i = 0; i < bars; ++i)
    {
        const auto active = i < lit;
        const auto h = active ? (i % 3 == 2 ? 10.0f : 6.0f) : 3.0f;
        g.setColour (active ? pal.accent : juce::Colour::fromFloatRGBA (68.0f / 255.0f, 94.0f / 255.0f, 106.0f / 255.0f, 0.18f));
        g.fillRoundedRectangle ((float) i * (w + gap), (float) getHeight() - h, w, h, 2.0f);
    }
}
} // namespace vox
