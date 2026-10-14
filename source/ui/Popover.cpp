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
            g.setColour (selected ? pal.accent : juce::Colour::fromFloatRGBA (111.0f / 255.0f, 143.0f / 255.0f, 156.0f / 255.0f, over ? 0.3f : 0.18f));
            g.fillRoundedRectangle (area, 5.0f);
            fonts::draw (g, getButtonText(), area, fonts::make (600, 10.0f, 0.04f), selected ? juce::Colours::white : theme.adj (juce::Colour (0xff4a606b)));
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
    closeLink->setVisible (true);

    setSize (width, kind == Kind::settings ? 190 : 214);
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
    }
}

void Popover::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    juce::Path p;
    p.addRoundedRectangle (area.reduced (1.0f), 8.0f);
    juce::DropShadow (juce::Colour::fromFloatRGBA (49.0f / 255.0f, 72.0f / 255.0f, 82.0f / 255.0f, 0.25f), 30, { 0, 13 }).drawForPath (g, p);

    g.setColour (juce::Colour::fromFloatRGBA (228.0f / 255.0f, 238.0f / 255.0f, 241.0f / 255.0f, 0.97f));
    g.fillRoundedRectangle (area.reduced (1.0f), 8.0f);
    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.drawRoundedRectangle (area.reduced (1.0f), 8.0f, 1.0f);

    const auto ink = theme.adj (juce::Colour (0xff415863));
    fonts::draw (g, kind == Kind::settings ? "SETTINGS" : "QUICK HELP", { 14.0f, 14.0f, 190.0f, 12.0f }, fonts::make (700, 9.0f, 0.1f), ink, juce::Justification::centredLeft);

    auto body = juce::Rectangle<float> (14.0f, 34.0f, 192.0f, 16.0f);
    auto line = [&] (const juce::String& text) {
        fonts::draw (g, text, body, fonts::make (400, 11.0f), ink, juce::Justification::centredLeft);
        body = body.translated (0.0f, 16.0f);
    };

    if (kind == Kind::settings)
    {
        body = body.translated (0.0f, 0.0f);
        line ("UI scale");
        body = body.translated (0.0f, 40.0f);
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
