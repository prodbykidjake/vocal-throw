#include "TopBar.h"
#include "../PluginProcessor.h"
#include "Fonts.h"
#include "Icons.h"

namespace vox
{
class TopBar::PresetTitle : public juce::Button
{
public:
    PresetTitle (Theme& t, PluginProcessor& p) : juce::Button ("preset"), theme (t), processor (p)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("Next preset");
    }

    static juce::Font font() { return fonts::make (500, 15.0f, 0.02f); }
    float getIdealWidth() const { return fonts::width (font(), processor.getProgramName (processor.getCurrentProgram())) + 40.0f; }

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        // the preset name in a glass capsule
        const auto& pal = theme.p();
        const auto area = getLocalBounds().toFloat().reduced (0.5f, 9.0f);
        g.setColour (pal.edge.withAlpha (over ? 0.07f : 0.035f));
        g.fillRoundedRectangle (area, area.getHeight() * 0.5f);
        g.setColour (pal.edge.withAlpha (0.1f));
        g.drawRoundedRectangle (area, area.getHeight() * 0.5f, 1.0f);
        fonts::draw (g, processor.getProgramName (processor.getCurrentProgram()), area, font(), pal.ink);
    }

private:
    Theme& theme;
    PluginProcessor& processor;
};

TopBar::TopBar (Theme& t, PluginProcessor& p)
    : theme (t),
      processor (p),
      previous (t, juce::CharPointer_UTF8 ("\xe2\x80\xb9"), TopBarButton::Kind::text, 24.0f),
      next (t, juce::CharPointer_UTF8 ("\xe2\x80\xba"), TopBarButton::Kind::text, 24.0f),
      undo (t, "undo"),
      redo (t, "redo"),
      a (t, "a"),
      b (t, "b"),
      copy (t, juce::CharPointer_UTF8 ("a\xe2\x86\x92" "b")),
      advanced (t, "adv"),
      help (t, "?"),
      settings (t, "settings", TopBarButton::Kind::gear),
      title (std::make_unique<PresetTitle> (t, p))
{
    previous.setTooltip ("Previous preset");
    next.setTooltip ("Next preset");
    undo.setTooltip ("Undo");
    redo.setTooltip ("Redo");
    a.setTooltip ("Switch to setting A");
    b.setTooltip ("Switch to setting B");
    copy.setTooltip ("Copy this setting over the other one");
    advanced.setTooltip ("Advanced: Max Cut, Wet Trim and tilts");
    help.setTooltip ("Help");
    settings.setTooltip ("Settings");

    previous.onClick = [this] { step (-1); };
    next.onClick = [this] { step (1); };
    title->onClick = [this] { step (1); };
    undo.onClick = [this] { processor.undoManager.undo(); };
    redo.onClick = [this] { processor.undoManager.redo(); };
    a.onClick = [this] { processor.switchToSlot (0); };
    b.onClick = [this] { processor.switchToSlot (1); };
    copy.onClick = [this] { processor.copyActiveToOtherSlot(); };
    advanced.onClick = [this] { if (onAdvanced) onAdvanced(); };
    help.onClick = [this] { if (onHelp) onHelp(); };
    settings.onClick = [this] { if (onSettings) onSettings(); };

    for (auto* c : std::initializer_list<juce::Component*> { &previous, &next, title.get(), &undo, &redo, &a, &b, &copy, &advanced, &help, &settings })
        addAndMakeVisible (c);

    startTimerHz (10);
    timerCallback();
}

TopBar::~TopBar() = default;

void TopBar::step (int direction)
{
    const auto n = processor.getNumPrograms();
    processor.setCurrentProgram ((processor.getCurrentProgram() + direction + n) % n);
    timerCallback();
}

void TopBar::timerCallback()
{
    undo.setMuted (! processor.undoManager.canUndo());
    redo.setMuted (! processor.undoManager.canRedo());
    a.setActiveLook (processor.getActiveSlot() == 0);
    b.setActiveLook (processor.getActiveSlot() == 1);
    copy.setLabel (processor.getActiveSlot() == 0 ? juce::String (juce::CharPointer_UTF8 ("a\xe2\x86\x92" "b")) : juce::String (juce::CharPointer_UTF8 ("b\xe2\x86\x92" "a")));

    if (shownProgram != processor.getCurrentProgram())
    {
        shownProgram = processor.getCurrentProgram();
        title->repaint();
        resized();
    }
}

void TopBar::paint (juce::Graphics& g)
{
    // no bar: the wordmark and the buttons float over the graph
    const auto& pal = theme.p();
    const auto h = (float) getHeight();
    icons::stroke (g, icons::wave(), juce::Rectangle<float> (28.0f, 15.0f).withCentre ({ 14.0f, h * 0.5f }), pal.accent, 2.0f);
    const auto wordmark = fonts::make (600, 16.0f, 0.22f);
    fonts::draw (g, "vox smoove", { 38.0f, 0.0f, 160.0f, h }, wordmark, pal.ink, juce::Justification::centredLeft);
    auto x = 38.0f + fonts::width (wordmark, "vox smoove") + 14.0f;
    fonts::draw (g, "playbox audio", { x, 0.0f, 140.0f, h }, fonts::make (400, 10.0f, 0.2f), pal.inkDim, juce::Justification::centredLeft);
    x += fonts::width (fonts::make (400, 10.0f, 0.2f), "playbox audio") + 18.0f;

    // a status chip while delta or bypass is on
    if (theme.isDelta() || theme.isBypassed())
    {
        const juce::String text = theme.isBypassed() ? "bypassed" : "delta";
        const auto font = fonts::make (600, 10.0f, 0.2f);
        const auto chip = juce::Rectangle<float> (x, h * 0.5f - 11.0f, fonts::width (font, text) + 24.0f, 22.0f);
        const auto colour = theme.isBypassed() ? pal.inkSoft : pal.accent;
        g.setColour (colour.withAlpha (0.14f));
        g.fillRoundedRectangle (chip, 11.0f);
        g.setColour (colour.withAlpha (0.6f));
        g.drawRoundedRectangle (chip.reduced (0.5f), 11.0f, 1.0f);
        fonts::draw (g, text, chip, font, colour);
    }
}

void TopBar::resized()
{
    const auto h = getHeight();

    // centre: [‹][ name ][›]
    const auto titleW = juce::jmax (180, juce::roundToInt (title->getIdealWidth()));
    const auto titleX = (getWidth() - titleW) / 2;
    title->setBounds (titleX, 0, titleW, h);
    previous.setBounds (titleX - 34, 0, 30, h);
    next.setBounds (titleX + titleW + 4, 0, 30, h);

    // right: undo redo a b a→b adv ? ⚙
    float x = (float) getWidth() + 4.0f;
    for (auto* button : { &settings, &help, &advanced, &copy, &b, &a, &redo, &undo })
    {
        const auto w = button->getIdealWidth();
        button->setBounds (juce::Rectangle<float> (x - w, 0.0f, w, (float) h).toNearestInt());
        x -= w + 10.0f; // 18 px between the labels, 8 of which is padding inside the buttons
    }
}
} // namespace vox
