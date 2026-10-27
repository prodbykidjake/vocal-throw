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
        const auto area = getLocalBounds().toFloat().reduced (0.5f);
        if (isSegment)
        {
            g.setColour (selected ? pal.accent.withAlpha (0.16f) : pal.edge.withAlpha (over ? 0.07f : 0.03f));
            g.fillRoundedRectangle (area, area.getHeight() * 0.5f);
            g.setColour (selected ? pal.accent.withAlpha (0.6f) : pal.edge.withAlpha (0.1f));
            g.drawRoundedRectangle (area, area.getHeight() * 0.5f, 1.0f);
            fonts::draw (g, getButtonText(), area, fonts::make (500, 11.0f, 0.06f), selected ? pal.accent : pal.inkSoft);
        }
        else
        {
            fonts::draw (g, getButtonText(), area, fonts::make (600, 10.0f, 0.2f), over ? pal.ink : pal.accent, juce::Justification::centredLeft);
        }
    }

private:
    Theme& theme;
    bool isSegment;
};

Popover::Popover (Theme& t)
    : theme (t),
      closeLink (std::make_unique<Link> (t, "close", false)),
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

    setSize (width, kind == Kind::settings ? 196 : 232);
    resized();
    repaint();
}

void Popover::resized()
{
    const auto h = getHeight();
    closeLink->setBounds (18, h - 18 - 14, 60, 14);
    if (kind == Kind::settings)
    {
        small->setBounds (18, 66, 58, 26);
        medium->setBounds (18 + 64, 66, 58, 26);
        large->setBounds (18 + 128, 66, 58, 26);
    }
}

void Popover::paint (juce::Graphics& g)
{
    const auto& pal = theme.p();
    theme.paintGlass (g, getLocalBounds().toFloat().reduced (1.0f), 14.0f, 0.96f);

    fonts::draw (g, kind == Kind::settings ? "settings" : "quick help", { 18.0f, 16.0f, 190.0f, 16.0f }, fonts::make (600, 11.0f, 0.22f), pal.accent,
        juce::Justification::centredLeft);

    auto body = juce::Rectangle<float> (18.0f, 42.0f, 200.0f, 17.0f);
    auto line = [&] (const juce::String& text, juce::Colour colour) {
        fonts::draw (g, text, body, fonts::make (400, 11.0f, 0.02f), colour, juce::Justification::centredLeft);
        body = body.translated (0.0f, 17.0f);
    };

    if (kind == Kind::settings)
    {
        line ("ui scale", pal.inkSoft);
        body = body.withY (106.0f);
        line ("Tooltips are on.", pal.inkDim);
        line ("Double-click a knob to reset it.", pal.inkDim);
    }
    else
    {
        for (const auto* text : { "Drag knobs vertically.", "Double-click a knob to reset it.", "Select a node to edit its band.",
                 "Double-click empty space to add", "a band, double-click a node to", "bypass it, right-click for shapes.",
                 "Scroll over a node to change Q." })
            line (text, pal.inkSoft);
    }
}
} // namespace vox
