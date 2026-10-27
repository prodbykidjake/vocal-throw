#include "VoxLookAndFeel.h"
#include "Fonts.h"

namespace vox
{
VoxLookAndFeel::VoxLookAndFeel (Theme& themeToUse) : theme (themeToUse)
{
    applyThemeColours();
    theme.addChangeListener (this);
}

VoxLookAndFeel::~VoxLookAndFeel()
{
    theme.removeChangeListener (this);
}

void VoxLookAndFeel::applyThemeColours()
{
    const auto& pal = theme.p();
    setColour (juce::PopupMenu::backgroundColourId, pal.glass);
    setColour (juce::PopupMenu::textColourId, pal.ink);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, pal.accent.withAlpha (0.18f));
    setColour (juce::PopupMenu::highlightedTextColourId, pal.accent);
    setColour (juce::PopupMenu::headerTextColourId, pal.inkDim);
}

juce::Font VoxLookAndFeel::getPopupMenuFont()
{
    return fonts::make (500, 13.0f);
}

void VoxLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (findColour (juce::PopupMenu::backgroundColourId));
    g.setColour (theme.p().edge.withAlpha (0.12f));
    g.drawRect (0, 0, width, height);
}

void VoxLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
    float startAngle, float endAngle, juce::Slider& slider)
{
    const auto& pal = theme.p();
    const auto area = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
    const auto d = juce::jmin (area.getWidth(), area.getHeight());
    const auto centre = area.getCentre();
    const bool large = d > 110.0f;
    const auto thickness = large ? 4.0f : 2.6f;
    const auto radius = d * 0.5f - thickness * 3.0f; // room for the glow
    const auto valueAngle = startAngle + pos * (endAngle - startAngle);
    const bool bipolar = slider.getProperties().contains ("bipolar");
    const auto fromAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;

    auto arc = [&] (float r, float from, float to) {
        juce::Path p;
        p.addCentredArc (centre.x, centre.y, r, r, 0.0f, juce::jmin (from, to), juce::jmax (from, to), true);
        return p;
    };

    // a faint inner disc and tick marks make it read as an instrument rather than a ring
    g.setColour (pal.edge.withAlpha (0.025f));
    g.fillEllipse (juce::Rectangle<float> (radius * 2.0f - 10.0f, radius * 2.0f - 10.0f).withCentre (centre));
    if (large)
    {
        g.setColour (pal.edge.withAlpha (0.12f));
        for (int i = 0; i <= 24; ++i)
        {
            const auto a = startAngle + (float) i / 24.0f * (endAngle - startAngle);
            const auto r1 = radius + thickness + 5.0f;
            const auto r2 = r1 + (i % 6 == 0 ? 6.0f : 3.0f);
            g.drawLine (centre.x + std::sin (a) * r1, centre.y - std::cos (a) * r1, centre.x + std::sin (a) * r2, centre.y - std::cos (a) * r2, 1.0f);
        }
    }

    g.setColour (pal.track);
    g.strokePath (arc (radius, startAngle, endAngle), juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (std::abs (valueAngle - fromAngle) > 0.01f)
    {
        const auto value = arc (radius, fromAngle, valueAngle);
        if (large)
        {
            // a violet-to-mint sweep along the value arc
            g.setGradientFill (juce::ColourGradient (pal.accent2, centre.x - radius, centre.y + radius, pal.accent, centre.x + radius, centre.y - radius, false));
            g.strokePath (value, juce::PathStrokeType (thickness * 3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (pal.bg.withAlpha (0.55f));
            g.strokePath (value, juce::PathStrokeType (thickness * 3.0f - 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        theme.strokeGlow (g, value, pal.accent, thickness);
    }

    // the end of the value arc is a bright dot
    const juce::Point<float> tip (centre.x + std::sin (valueAngle) * radius, centre.y - std::cos (valueAngle) * radius);
    if (pal.glow > 0.0f)
    {
        g.setColour (pal.accent.withAlpha (0.25f));
        g.fillEllipse (juce::Rectangle<float> (thickness * 4.0f, thickness * 4.0f).withCentre (tip));
    }
    g.setColour (pal.ink);
    g.fillEllipse (juce::Rectangle<float> (thickness * 1.6f, thickness * 1.6f).withCentre (tip));
}

void VoxLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
    float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle, juce::Slider& slider)
{
    const auto& pal = theme.p();
    const auto area = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
    const auto cy = area.getCentreY();
    const auto accent = slider.getProperties().contains ("accent")
        ? theme.adj (juce::Colour ((juce::uint32) (int) slider.getProperties()["accent"]))
        : pal.accent;

    g.setColour (pal.track);
    g.fillRoundedRectangle (area.getX(), cy - 1.0f, area.getWidth(), 2.0f, 1.0f);

    // signed sliders (trims, tilts) fill from their centre
    const auto from = slider.getProperties().contains ("bipolar") ? (minSliderPos + maxSliderPos) * 0.5f : minSliderPos;
    juce::Path fill;
    fill.startNewSubPath (juce::jmin (from, sliderPos), cy);
    fill.lineTo (juce::jmax (from, sliderPos) + 0.01f, cy);
    theme.strokeGlow (g, fill, accent, 2.0f);

    const auto thumb = juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ sliderPos, cy });
    if (pal.glow > 0.0f)
    {
        g.setColour (accent.withAlpha (0.22f));
        g.fillEllipse (thumb.expanded (5.0f));
    }
    g.setColour (pal.ink);
    g.fillEllipse (thumb);
    g.setColour (accent);
    g.drawEllipse (thumb.expanded (1.5f), 1.0f);
}
} // namespace vox
