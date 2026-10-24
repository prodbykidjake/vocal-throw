#pragma once

#include "../engine/PresetManager.h"
#include "Widgets.h"

// Brand, preset selector, undo/redo, A/B, bypass and settings (design: .topbar, 76 px).
class HeaderBar : public juce::Component, private juce::Timer
{
public:
    HeaderBar (PluginProcessor& p, PresetManager& pm, std::function<void()> stateChanged)
        : processor (p), presets (pm), changed (std::move (stateChanged)),
          bypassAttachment (*p.apvts.getParameter (ParamIDs::bypass), [this] (float) { refreshBypass(); }, &p.undoManager)
    {
        undoButton.icon = [] (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c) { drawUndoGlyph (g, r, c, false); };
        redoButton.icon = [] (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c) { drawUndoGlyph (g, r, c, true); };
        settingsButton.icon = [] (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c) { drawGear (g, r, c); };
        bypassButton.icon = drawPowerGlyph;
        bypassButton.setTextFont (Theme::mono (9.5f, 0.08f));
        bypassButton.setClickingTogglesState (false);
        for (auto* b : { &undoButton, &redoButton, &settingsButton })
            b->setTextFont (Theme::sans (11.0f));
        for (auto* b : { &slotA, &slotB })
        {
            b->setStyle (DesignButton::Style::segment);
            b->setTextFont (Theme::mono (10.0f));
        }

        undoButton.onClick = [this] { processor.undoManager.undo(); changed(); };
        redoButton.onClick = [this] { processor.undoManager.redo(); changed(); };
        slotA.onClick = [this] { selectSlot (0); };
        slotB.onClick = [this] { selectSlot (1); };
        bypassButton.onClick = [this] {
            bypassAttachment.setValueAsCompleteGesture (processor.apvts.getRawParameterValue (ParamIDs::bypass)->load() > 0.5f ? 0.0f : 1.0f);
        };
        settingsButton.onClick = [this] { showSettings(); };

        for (auto* c : std::initializer_list<juce::Component*> { &undoButton, &redoButton, &slotA, &slotB, &bypassButton, &settingsButton })
            addAndMakeVisible (c);
        refresh();
        startTimerHz (10);
    }

    void refresh()
    {
        refreshBypass();
        const int slot = processor.getActiveSlot();
        slotA.setToggleState (slot == 0, juce::dontSendNotification);
        slotB.setToggleState (slot == 1, juce::dontSendNotification);
        undoButton.setEnabled (processor.undoManager.canUndo());
        redoButton.setEnabled (processor.undoManager.canRedo());
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::topbar);
        g.setColour (juce::Colour (0xff272f31));
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

        // brand mark: five mint bars in a rounded tile
        auto mark = juce::Rectangle<float> (22.0f, 21.0f, 34.0f, 34.0f);
        Theme::card_ (g, mark, juce::Colour (0xff0f1314), juce::Colour (0xff344044), 8.0f);
        const float heights[] = { 8, 17, 24, 17, 8 };
        for (int i = 0; i < 5; ++i)
        {
            auto bar = juce::Rectangle<float> (2.0f, heights[i]).withCentre ({ mark.getX() + 8.0f + (float) i * 4.5f + 1.5f, mark.getCentreY() });
            g.setColour (Theme::accent.withAlpha (0.25f));
            g.fillRoundedRectangle (bar.expanded (2.0f), 3.0f);
            g.setColour (Theme::accent);
            g.fillRoundedRectangle (bar, 1.0f);
        }
        g.setFont (Theme::sans (14.0f, true, 0.14f));
        g.setColour (Theme::text);
        const auto signal = juce::String ("SIGNAL");
        const auto w = juce::GlyphArrangement::getStringWidth (Theme::sans (14.0f, true, 0.14f), signal);
        g.drawText (signal, 68, 22, (int) w + 4, 18, juce::Justification::centredLeft, false);
        g.setColour (Theme::accent);
        g.drawText ("PATH", 68 + (int) w + 2, 22, 70, 18, juce::Justification::centredLeft, false);
        g.setColour (juce::Colour (0xff6f7b7e));
        g.setFont (Theme::mono (8.0f, 0.2f));
        g.drawText ("INTELLIGENT AUDIO", 68, 42, 200, 12, juce::Justification::centredLeft, false);

        // preset selector
        auto r = presetBounds.toFloat();
        Theme::card_ (g, r, juce::Colour (0xff101415), Theme::borderStrong, 7.0f);
        g.setColour (juce::Colour (0xff687477));
        g.setFont (Theme::mono (8.0f, 0.14f));
        g.drawText ("PRESET", presetBounds.withTrimmedLeft (14).withTrimmedTop (8).withHeight (12), juce::Justification::topLeft, false);
        g.setColour (Theme::text);
        g.setFont (Theme::sans (12.0f));
        const auto name = juce::String (processor.apvts.state.getProperty (PluginProcessor::presetName_, "Init").toString());
        g.drawText (name.isEmpty() ? juce::String ("Init") : name, presetBounds.withTrimmedLeft (14).withTrimmedRight (34).withTrimmedTop (21), juce::Justification::topLeft, true);
        drawChevron (g, juce::Rectangle<float> ((float) presetBounds.getRight() - 30.0f, (float) presetBounds.getCentreY() - 7.0f, 14.0f, 14.0f), juce::Colour (0xff9ba7aa));
    }

    void resized() override
    {
        presetBounds = { (getWidth() - 340) / 2, 16, 340, 44 };
        auto r = getLocalBounds().withTrimmedRight (22).withSizeKeepingCentre (getWidth() - 22, 34);
        settingsButton.setBounds (r.removeFromRight (34));
        r.removeFromRight (7);
        bypassButton.setBounds (r.removeFromRight (104));
        r.removeFromRight (7);
        auto ab = r.removeFromRight (68);
        slotA.setBounds (ab.removeFromLeft (34).reduced (3));
        slotB.setBounds (ab.reduced (3));
        r.removeFromRight (7);
        redoButton.setBounds (r.removeFromRight (34));
        r.removeFromRight (7);
        undoButton.setBounds (r.removeFromRight (34));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (presetBounds.contains (e.getPosition()))
            showPresetMenu();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (presetBounds.contains (e.getPosition()) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // frame behind the A/B segment
        auto ab = juce::Rectangle<float> ((float) slotA.getX() - 3.0f, (float) slotA.getY() - 3.0f, 68.0f, 34.0f);
        g.setColour (Theme::borderStrong);
        g.drawRoundedRectangle (ab.reduced (0.5f), 6.0f, 1.0f);
    }

    std::function<void()> onSettingsChanged;

private:
    static void drawUndoGlyph (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, bool redo)
    {
        const auto cx = r.getCentreX(), cy = r.getCentreY() + 1.0f, s = std::min (r.getWidth(), r.getHeight()) * 0.42f;
        const auto flip = redo ? juce::AffineTransform::scale (-1.0f, 1.0f, cx, cy) : juce::AffineTransform();
        juce::Path arc;
        arc.addCentredArc (cx, cy, s, s, 0.0f, -1.0f, 2.7f, true); // clockwise from upper-left over the top and right
        arc.applyTransform (flip);
        g.setColour (c);
        g.strokePath (arc, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const auto tx = cx + s * std::sin (-1.0f), ty = cy - s * std::cos (-1.0f); // arrow tip at the start of the arc
        juce::Path head;
        head.addTriangle (tx - 3.2f, ty - 3.8f, tx + 1.0f, ty + 3.8f, tx - 4.4f, ty + 3.0f);
        head.applyTransform (flip);
        g.fillPath (head);
    }

    static void drawGear (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
    {
        const auto cx = r.getCentreX(), cy = r.getCentreY(), s = std::min (r.getWidth(), r.getHeight()) * 0.46f;
        g.setColour (c);
        g.drawEllipse (cx - s * 0.3f, cy - s * 0.3f, s * 0.6f, s * 0.6f, 1.5f);
        for (int i = 0; i < 8; ++i)
        {
            const auto a = juce::MathConstants<float>::twoPi * (float) i / 8.0f;
            g.drawLine (cx + std::cos (a) * s * 0.62f, cy + std::sin (a) * s * 0.62f, cx + std::cos (a) * s, cy + std::sin (a) * s, 2.2f);
        }
        g.drawEllipse (cx - s * 0.65f, cy - s * 0.65f, s * 1.3f, s * 1.3f, 1.5f);
    }

    void timerCallback() override
    {
        undoButton.setEnabled (processor.undoManager.canUndo());
        redoButton.setEnabled (processor.undoManager.canRedo());
        const int slot = processor.getActiveSlot();
        slotA.setToggleState (slot == 0, juce::dontSendNotification);
        slotB.setToggleState (slot == 1, juce::dontSendNotification);
        refreshBypass();
        repaint (presetBounds);
    }

    void refreshBypass()
    {
        const bool bypassed = processor.apvts.getRawParameterValue (ParamIDs::bypass)->load() > 0.5f;
        bypassButton.setButtonText (bypassed ? "BYPASSED" : "ACTIVE");
        bypassButton.setToggleState (bypassed, juce::dontSendNotification);
        bypassButton.setTint (Theme::coral);
        bypassButton.setAccentText (! bypassed);
    }

    void selectSlot (int slot)
    {
        if (processor.getActiveSlot() != slot)
            processor.switchABSlot();
        refresh();
        changed();
    }

    void showPresetMenu()
    {
        juce::PopupMenu menu;
        const auto list = presets.list();
        menu.addSectionHeader ("Factory");
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].factory)
                menu.addItem ((int) (1000 + i), list[i].name);
        bool anyUser = false;
        for (size_t i = 0; i < list.size(); ++i)
            if (! list[i].factory)
            {
                if (! anyUser)
                {
                    menu.addSeparator();
                    menu.addSectionHeader ("User");
                    anyUser = true;
                }
                menu.addItem ((int) (1000 + i), list[i].name);
            }
        menu.addSeparator();
        menu.addItem (1, "Save current settings as...");
        if (anyUser)
        {
            juce::PopupMenu del;
            for (size_t i = 0; i < list.size(); ++i)
                if (! list[i].factory)
                    del.addItem ((int) (5000 + i), list[i].name);
            menu.addSubMenu ("Delete user preset", del);
        }
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (presetBounds)).withMinimumWidth (presetBounds.getWidth()),
                            [this, list, safe = juce::Component::SafePointer<HeaderBar> (this)] (int result) {
                                if (safe == nullptr || result == 0)
                                    return;
                                if (result == 1)
                                    promptSave();
                                else if (result >= 5000)
                                    presets.remove (list[(size_t) (result - 5000)].name);
                                else if (presets.load (list[(size_t) (result - 1000)].name))
                                    changed();
                                refresh();
                            });
    }

    void promptSave()
    {
        auto* w = new juce::AlertWindow ("Save preset", "Name for this preset:", juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("name", {});
        w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, safe = juce::Component::SafePointer<HeaderBar> (this)] (int r) {
                                if (safe != nullptr && r == 1 && ! presets.save (w->getTextEditorContents ("name")))
                                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Could not save", "Choose a different name (factory preset names are reserved).");
                                if (safe != nullptr)
                                    refresh();
                            }),
                            true);
    }

    void showSettings()
    {
        juce::PopupMenu m;
        const bool match = processor.apvts.getRawParameterValue (ParamIDs::loudnessMatch)->load() > 0.5f;
        const bool autoApply = (bool) processor.apvts.state.getProperty (PluginProcessor::autoApply_, false);
        const bool autoGain = processor.apvts.getRawParameterValue (ParamIDs::autoGain)->load() > 0.5f;
        m.addItem (4, "Auto level match (keep loudness constant)", true, autoGain);
        m.addItem (1, "Match loudness when bypassed", true, match);
        m.addItem (2, "Automatic mode: apply suggestions immediately", true, autoApply);
        m.addSeparator();
        m.addItem (3, "Clear undo history");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&settingsButton), [this] (int r) {
            if (r == 1)
            {
                auto* p = processor.apvts.getParameter (ParamIDs::loudnessMatch);
                p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.0f : 1.0f);
            }
            else if (r == 2)
                processor.apvts.state.setProperty (PluginProcessor::autoApply_, ! (bool) processor.apvts.state.getProperty (PluginProcessor::autoApply_, false), nullptr);
            else if (r == 3)
                processor.undoManager.clearUndoHistory();
            else if (r == 4)
            {
                auto* p = processor.apvts.getParameter (ParamIDs::autoGain);
                p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.0f : 1.0f);
            }
            if (r != 0)
            {
                if (onSettingsChanged)
                    onSettingsChanged();
                refresh();
            }
        });
    }

    PluginProcessor& processor;
    PresetManager& presets;
    std::function<void()> changed;
    juce::ParameterAttachment bypassAttachment;
    juce::Rectangle<int> presetBounds;
    DesignButton undoButton { {}, DesignButton::Style::outline }, redoButton { {}, DesignButton::Style::outline },
        slotA { "A", DesignButton::Style::segment }, slotB { "B", DesignButton::Style::segment },
        bypassButton { "ACTIVE", DesignButton::Style::outline }, settingsButton { {}, DesignButton::Style::outline };
};

// Guided / Automatic / Creative tabs, description, live-input indicator (design: .mode-bar, 52 px).
class ModeBar : public juce::Component
{
public:
    static constexpr const char* descriptions[3] = { "Make confident changes with clear, signal-specific guidance.",
                                                     "Get a fast recommendation while keeping every change editable.",
                                                     "Shape the complete processing chain with detailed controls." };

    explicit ModeBar (std::function<void (int)> onMode) : modeChanged (std::move (onMode))
    {
        const char* names[3] = { "Guided", "Automatic", "Creative" };
        for (int i = 0; i < 3; ++i)
        {
            tabs[(size_t) i].setButtonText (names[i]);
            tabs[(size_t) i].setStyle (DesignButton::Style::segment);
            tabs[(size_t) i].setTextFont (Theme::sans (11.0f, true));
            tabs[(size_t) i].setRadioGroupId (4711);
            tabs[(size_t) i].setClickingTogglesState (true);
            tabs[(size_t) i].onClick = [this, i] { setMode (i, true); };
            addAndMakeVisible (tabs[(size_t) i]);
        }
    }

    void setMode (int m, bool notify)
    {
        mode = m;
        for (int i = 0; i < 3; ++i)
            tabs[(size_t) i].setToggleState (i == m, juce::dontSendNotification);
        repaint();
        if (notify)
            modeChanged (m);
    }

    void setLive (bool isLive, const juce::String& text)
    {
        if (isLive != live || text != liveText)
        {
            live = isLive;
            liveText = text;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::modebar);
        g.setColour (Theme::borderSoft);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
        Theme::card_ (g, trackBounds.toFloat(), Theme::tabTrack, Theme::tabTrack, 7.0f);
        g.setColour (Theme::textDim);
        g.setFont (Theme::sans (11.0f));
        g.drawText (Theme::u8 (descriptions[mode]), trackBounds.getRight() + 24, 0, 700, getHeight(), juce::Justification::centredLeft, false);

        g.setFont (Theme::mono (9.0f, 0.12f));
        const auto w = (int) juce::GlyphArrangement::getStringWidth (Theme::mono (9.0f, 0.12f), liveText) + 4;
        g.setColour (juce::Colour (0xff7d898c));
        g.drawText (liveText, getWidth() - 22 - w, 0, w, getHeight(), juce::Justification::centredRight, false);
        const auto dot = juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ (float) (getWidth() - 22 - w - 10), (float) getHeight() * 0.5f });
        if (live)
        {
            g.setColour (Theme::accent.withAlpha (0.3f));
            g.fillEllipse (dot.expanded (3.0f));
        }
        g.setColour (live ? Theme::accent : juce::Colour (0xff3a4547));
        g.fillEllipse (dot);
    }

    void resized() override
    {
        trackBounds = { 22, 9, 3 * 84 + 6, 34 };
        auto r = trackBounds.reduced (3);
        for (auto& t : tabs)
            t.setBounds (r.removeFromLeft (84));
    }

private:
    std::function<void (int)> modeChanged;
    std::array<DesignButton, 3> tabs;
    juce::Rectangle<int> trackBounds;
    int mode = 0;
    bool live = false;
    juce::String liveText = "NO SIGNAL";
};
