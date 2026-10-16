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
    setColour (juce::PopupMenu::backgroundColourId, pal.popover);
    setColour (juce::PopupMenu::textColourId, pal.ink);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, pal.accent);
    setColour (juce::PopupMenu::highlightedTextColourId, pal.onAccent);
    setColour (juce::PopupMenu::headerTextColourId, pal.muted);
}

juce::Font VoxLookAndFeel::getPopupMenuFont()
{
    return fonts::make (500, 13.0f);
}

void VoxLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (findColour (juce::PopupMenu::backgroundColourId));
    g.setColour (theme.p().shade.withAlpha (0.25f));
    g.drawRect (0, 0, width, height);
}

void VoxLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
    float startAngle, float endAngle, juce::Slider& slider)
{
    const auto& pal = theme.p();
    const auto area = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
    const auto d = juce::jmin (area.getWidth(), area.getHeight());
    const auto centre = area.getCentre();
    const bool large = d > 120.0f;

    const auto thickness = d * 0.11f;
    const auto ringRadius = d * 0.5f - thickness * 0.5f;
    const auto valueAngle = startAngle + pos * (endAngle - startAngle);
    const auto background = slider.getProperties().contains ("bg")
        ? juce::Colour ((juce::uint32) (int) slider.getProperties()["bg"])
        : pal.rail;

    auto arc = [&] (float from, float to) {
        juce::Path p;
        p.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f, from, to, true);
        return p;
    };
    const juce::PathStrokeType butt (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt);

    // what casts the soft shadow: the inner disc and the white progress arc
    juce::Path shadowShape;
    shadowShape.addEllipse (area.withSizeKeepingCentre (d * 0.78f, d * 0.78f));
    juce::Path progress;
    butt.createStrokedPath (progress, arc (startAngle, valueAngle));
    shadowShape.addPath (progress);
    juce::DropShadow (pal.shade.withAlpha (0.17f), 5, { 4, 5 }).drawForPath (g, shadowShape);

    // the inner disc hides the shadow inside the ring, as the design's ::before does
    g.setColour (background);
    g.fillEllipse (area.withSizeKeepingCentre (d * 0.78f, d * 0.78f));

    g.setColour (pal.shade.withAlpha (0.18f));
    g.strokePath (arc (valueAngle, endAngle), butt);
    g.setColour (pal.knobArc);
    g.strokePath (arc (startAngle, valueAngle), butt);

    // the face
    const auto face = area.withSizeKeepingCentre (d * 0.68f, d * 0.68f);
    juce::Path facePath;
    facePath.addEllipse (face);
    juce::DropShadow (pal.shade.withAlpha (0.25f), 10, { 6, 8 }).drawForPath (g, facePath);
    g.setColour (pal.charcoal);
    g.fillEllipse (face);
    {
        // 1px top highlight (the design's inset shadow)
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (juce::Rectangle<int> (face.toNearestInt()));
        juce::Path clip;
        clip.addEllipse (face);
        g.reduceClipRegion (clip);
        g.setColour (juce::Colours::white.withAlpha (0.08f)); // the face is always dark
        g.fillEllipse (face);
        g.setColour (pal.charcoal);
        g.fillEllipse (face.translated (0.0f, 1.0f));
    }

    // pointer: a 2px white line near the rim of the face
    const auto faceRadius = face.getWidth() * 0.5f;
    const auto inset = large ? 8.0f : 3.0f;
    const auto length = large ? 19.0f : 10.0f;
    const auto outer = faceRadius - inset;
    const auto inner = outer - length;
    const juce::Point<float> a (centre.x + std::sin (valueAngle) * outer, centre.y - std::cos (valueAngle) * outer);
    const juce::Point<float> b (centre.x + std::sin (valueAngle) * inner, centre.y - std::cos (valueAngle) * inner);
    g.setColour (pal.barText);
    g.drawLine (juce::Line<float> (a, b), 2.0f);
    g.fillEllipse (juce::Rectangle<float> (2.0f, 2.0f).withCentre (a));
    g.fillEllipse (juce::Rectangle<float> (2.0f, 2.0f).withCentre (b));
}

void VoxLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
    float, float, juce::Slider::SliderStyle, juce::Slider& slider)
{
    const auto& pal = theme.p();
    const auto area = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).expanded ((float) getSliderThumbRadius (slider), 0.0f);
    const auto cy = area.getCentreY();

    g.setColour (pal.shade.withAlpha (0.23f));
    g.fillRoundedRectangle (area.getX(), cy - 2.0f, area.getWidth(), 4.0f, 2.0f);

    const auto accent = slider.getProperties().contains ("accent")
        ? theme.adj (juce::Colour ((juce::uint32) (int) slider.getProperties()["accent"]))
        : pal.accent;

    const auto thumb = juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ sliderPos, cy });
    juce::Path thumbPath;
    thumbPath.addEllipse (thumb);
    juce::DropShadow (pal.shade.withAlpha (0.3f), 5, { 0, 2 }).drawForPath (g, thumbPath);
    g.setColour (pal.barText);
    g.fillEllipse (thumb);
    g.setColour (accent);
    g.fillEllipse (thumb.reduced (2.0f));
}
} // namespace vox
