#include "TopBar.h"
#include "../PluginProcessor.h"
#include "Fonts.h"

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

    static float dotsWidth() { return fonts::width (fonts::make (500, 16.0f, 0.13f), juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2"))); }
    float getIdealWidth() const
    {
        return dotsWidth() + 11.0f + fonts::width (fonts::make (500, 16.0f, 0.03f), processor.getProgramName (processor.getCurrentProgram())) + 8.0f;
    }

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        const auto colour = theme.p().topBarInk.withAlpha (over ? 1.0f : 0.88f);
        const auto area = getLocalBounds().toFloat();
        fonts::draw (g, juce::CharPointer_UTF8 ("\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2"), area.withWidth (dotsWidth()), fonts::make (500, 16.0f, 0.13f), colour, juce::Justification::centredLeft);
        fonts::draw (g, processor.getProgramName (processor.getCurrentProgram()), area.withTrimmedLeft (dotsWidth() + 11.0f), fonts::make (500, 16.0f, 0.03f), colour, juce::Justification::centredLeft);
    }

private:
    Theme& theme;
    PluginProcessor& processor;
};

TopBar::TopBar (Theme& t, PluginProcessor& p)
    : theme (t),
      processor (p),
      previous (t, juce::CharPointer_UTF8 ("\xe2\x80\xb9"), TopBarButton::Kind::text, 27.0f),
      next (t, juce::CharPointer_UTF8 ("\xe2\x80\xba"), TopBarButton::Kind::text, 27.0f),
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
    g.fillAll (theme.p().topBar);
}

void TopBar::resized()
{
    const auto h = getHeight();

    // left: [‹][›][••• name]
    previous.setBounds (11, 0, 28, h);
    next.setBounds (39, 0, 28, h);
    title->setBounds (67, 0, juce::roundToInt (title->getIdealWidth()) + 4, h);

    // right: undo redo a b a→b adv ? ⚙, 15 px apart, 11 px from the edge
    float x = (float) getWidth() - 11.0f + 4.0f;
    for (auto* button : { &settings, &help, &advanced, &copy, &b, &a, &redo, &undo })
    {
        const auto w = button->getIdealWidth();
        button->setBounds (juce::Rectangle<float> (x - w, 0.0f, w, (float) h).toNearestInt());
        x -= w + 7.0f; // 15 px between the labels, 8 of which is padding inside the buttons
    }
}
} // namespace vox
