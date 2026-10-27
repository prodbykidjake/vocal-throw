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
    const auto& pal = theme.p();
    const auto area = getLocalBounds().toFloat();
    auto colour = activeLook ? pal.accent : (over ? pal.ink : pal.inkSoft);
    if (muted)
        colour = pal.inkDim.withMultipliedAlpha (0.6f);

    if (kind == Kind::gear)
    {
        juce::Path gear = icons::gear();
        const auto box = juce::Rectangle<float> (15.0f, 15.0f).withCentre (area.getCentre());
        g.setColour (colour);
        g.fillPath (gear, juce::AffineTransform::scale (box.getWidth(), box.getHeight()).translated (box.getX(), box.getY()));
        return;
    }

    fonts::draw (g, getButtonText(), area, fonts::make (weight, fontPx, 0.04f), colour);
    if (activeLook)
    {
        // a small glowing dot under the active item
        const auto dot = juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ area.getCentreX(), area.getCentreY() + 12.0f });
        if (pal.glow > 0.0f)
        {
            g.setColour (pal.accent.withAlpha (0.3f));
            g.fillEllipse (dot.expanded (3.0f));
        }
        g.setColour (pal.accent);
        g.fillEllipse (dot);
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
    const auto& pal = theme.p();
    const auto colour = getToggleState() ? pal.accent : (over ? pal.ink : pal.inkDim);
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
        const auto h = (float) getHeight() * (0.35f + 0.65f * (float) i / (float) (bars - 1));
        g.setColour (active ? pal.accent.withAlpha (0.5f + 0.5f * (float) i / (float) bars) : pal.track);
        g.fillRoundedRectangle ((float) i * (w + gap), (float) getHeight() - h, w, h, 1.5f);
    }
}
} // namespace vox
