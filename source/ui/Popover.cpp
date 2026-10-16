#include "Popover.h"
#include "Fonts.h"

namespace vox
{
class Popover::Link : public juce::Button
{
public:
    Link (Theme& t, const juce::String& label, bool segment) : juce::Button (label), theme (t), isSegment (segment)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    bool selected = false;

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        const auto& pal = theme.p();
        const auto area = getLocalBounds().toFloat();
        if (isSegment)
        {
            g.setColour (selected ? pal.accent : pal.tint.withAlpha (over ? 0.3f : 0.18f));
            g.fillRoundedRectangle (area, 5.0f);
            fonts::draw (g, getButtonText(), area, fonts::make (600, 10.0f, 0.04f), selected ? pal.onAccent : pal.ink);
        }
        else
        {
            fonts::draw (g, getButtonText(), area, fonts::make (700, 9.0f, 0.1f), over ? pal.ink : pal.accent, juce::Justification::centredLeft);
        }
    }

private:
    Theme& theme;
    bool isSegment;
};

/** A look in the settings card: a swatch of the look's own rail, graph and accent, then its name. */
class Popover::LookChip : public juce::Button
{
public:
    LookChip (Theme& t, Look l) : juce::Button (lookName (l)), theme (t), look (l)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip (juce::String (lookName (l)) + " look");
    }

    bool selected = false;

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        const auto& pal = theme.p();
        const auto area = getLocalBounds().toFloat();
        g.setColour (selected ? pal.accent.withAlpha (0.22f) : pal.tint.withAlpha (over ? 0.3f : 0.14f));
        g.fillRoundedRectangle (area, 5.0f);
        if (selected)
        {
            g.setColour (pal.accent);
            g.drawRoundedRectangle (area.reduced (0.5f), 5.0f, 1.0f);
        }

        // the swatch shows the look itself, whatever the current one is
        const auto own = paletteFor (look, false);
        const auto swatch = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ area.getX() + 13.0f, area.getCentreY() });
        juce::Path left, right;
        left.addPieSegment (swatch, juce::MathConstants<float>::pi, juce::MathConstants<float>::twoPi, 0.0f);
        right.addPieSegment (swatch, 0.0f, juce::MathConstants<float>::pi, 0.0f);
        g.setColour (theme.adj (own.rail));
        g.fillPath (left);
        g.setColour (theme.adj (own.graph));
        g.fillPath (right);
        g.setColour (theme.adj (own.accent));
        g.fillEllipse (swatch.withSizeKeepingCentre (6.0f, 6.0f));
        g.setColour (pal.shade.withAlpha (0.3f));
        g.drawEllipse (swatch, 1.0f);

        fonts::draw (g, getButtonText(), area.withTrimmedLeft (26.0f), fonts::make (600, 10.0f, 0.04f), pal.ink, juce::Justification::centredLeft);
    }

private:
    Theme& theme;
    Look look;
};

namespace
{
    constexpr int lookTop = 98;
    constexpr int lookChipH = 24;
    constexpr int lookGap = 6;
    constexpr int lookRows = (numLooks + 1) / 2;
    constexpr int lookBottom = lookTop + lookRows * lookChipH + (lookRows - 1) * lookGap;
}

Popover::Popover (Theme& t)
    : theme (t),
      closeLink (std::make_unique<Link> (t, "CLOSE", false)),
      small (std::make_unique<Link> (t, "70%", true)),
      medium (std::make_unique<Link> (t, "85%", true)),
      large (std::make_unique<Link> (t, "100%", true))
{
    closeLink->onClick = [this] { if (onClose) onClose(); };
    small->onClick = [this] { if (onScale) onScale (0.7f); };
    medium->onClick = [this] { if (onScale) onScale (0.85f); };
    large->onClick = [this] { if (onScale) onScale (1.0f); };
    for (auto* c : { closeLink.get(), small.get(), medium.get(), large.get() })
        addChildComponent (c);

    for (int i = 0; i < numLooks; ++i)
    {
        const auto look = (Look) i;
        auto& chip = looks[(size_t) i];
        chip = std::make_unique<LookChip> (t, look);
        chip->onClick = [this, look] { if (onLook) onLook (look); };
        addChildComponent (*chip);
    }
}

Popover::~Popover() = default;

void Popover::show (Kind k, float currentScale)
{
    kind = k;
    scale = currentScale;
    small->selected = std::abs (scale - 0.7f) < 0.02f;
    medium->selected = std::abs (scale - 0.85f) < 0.02f;
    large->selected = std::abs (scale - 1.0f) < 0.02f;
    for (auto* l : { small.get(), medium.get(), large.get() })
        l->setVisible (kind == Kind::settings);
    for (int i = 0; i < numLooks; ++i)
    {
        looks[(size_t) i]->selected = theme.getLook() == (Look) i;
        looks[(size_t) i]->setVisible (kind == Kind::settings);
    }
    closeLink->setVisible (true);

    setSize (width, kind == Kind::settings ? lookBottom + 112 : 214);
    resized();
    repaint();
}

void Popover::resized()
{
    const auto h = getHeight();
    closeLink->setBounds (14, h - 14 - 12, 60, 12);
    if (kind == Kind::settings)
    {
        small->setBounds (14, 50, 56, 22);
        medium->setBounds (14 + 62, 50, 56, 22);
        large->setBounds (14 + 124, 50, 56, 22);

        constexpr int chipW = (192 - lookGap) / 2;
        for (int i = 0; i < numLooks; ++i)
            looks[(size_t) i]->setBounds (14 + (i % 2) * (chipW + lookGap), lookTop + (i / 2) * (lookChipH + lookGap), chipW, lookChipH);
    }
}

void Popover::paint (juce::Graphics& g)
{
    const auto& pal = theme.p();
    const auto area = getLocalBounds().toFloat();
    juce::Path p;
    p.addRoundedRectangle (area.reduced (1.0f), 8.0f);
    juce::DropShadow (pal.shade.withAlpha (0.25f), 30, { 0, 13 }).drawForPath (g, p);

    g.setColour (pal.popover.withAlpha (0.97f));
    g.fillRoundedRectangle (area.reduced (1.0f), 8.0f);
    g.setColour (theme.sheen (0.45f));
    g.drawRoundedRectangle (area.reduced (1.0f), 8.0f, 1.0f);

    const auto ink = pal.ink;
    fonts::draw (g, kind == Kind::settings ? "SETTINGS" : "QUICK HELP", { 14.0f, 14.0f, 190.0f, 12.0f }, fonts::make (700, 9.0f, 0.1f), ink, juce::Justification::centredLeft);

    auto body = juce::Rectangle<float> (14.0f, 34.0f, 192.0f, 16.0f);
    auto line = [&] (const juce::String& text) {
        fonts::draw (g, text, body, fonts::make (400, 11.0f), ink, juce::Justification::centredLeft);
        body = body.translated (0.0f, 16.0f);
    };

    if (kind == Kind::settings)
    {
        line ("UI scale");
        body = body.withY (82.0f);
        line ("Look");
        body = body.withY ((float) lookBottom + 12.0f);
        line ("Tooltips are on.");
        line ("Double-click a knob to reset it.");
    }
    else
    {
        line ("Drag knobs vertically.");
        line ("Double-click a knob to reset it.");
        line ("Select a node to edit its band.");
        line ("Double-click empty space to add");
        line ("a band, double-click a node to");
        line ("bypass it, right-click for shapes.");
        line ("Scroll over a node to change Q.");
    }
}
} // namespace vox
