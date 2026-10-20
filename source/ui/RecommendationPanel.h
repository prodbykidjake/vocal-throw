#pragma once

#include "../engine/RecommendationSession.h"
#include "Widgets.h"

class RecommendationPanel : public juce::Component
{
public:
    RecommendationPanel (PluginProcessor& p, RecommendationSession& s, juce::ValueTree stateTree, std::function<void()> onUserAction)
        : processor (p), session (s), state (std::move (stateTree)), userAction (std::move (onUserAction)),
          matchAttachment (*p.apvts.getParameter (ParamIDs::loudnessMatch), [this] (float) { repaint(); }, &p.undoManager)
    {
        left.setTextFont (Theme::sans (10.5f, true));
        right.setTextFont (Theme::sans (10.5f, true));
        right.setStyle (DesignButton::Style::light);
        left.onClick = [this] { onLeft(); };
        right.onClick = [this] { onRight(); };
        manualButton.setStyle (DesignButton::Style::ghost);
        manualButton.setTextFont (Theme::sans (10.0f));
        manualButton.onClick = [this] {
            state.setProperty (manualProp, ! (bool) state.getProperty (manualProp, false), nullptr);
            refresh();
        };
        matchToggle.setTextFont (Theme::mono (8.5f));
        matchToggle.onClick = [this] { matchAttachment.setValueAsCompleteGesture (processor.apvts.getRawParameterValue (ParamIDs::loudnessMatch)->load() > 0.5f ? 0.0f : 1.0f); };
        for (auto* c : std::initializer_list<juce::Component*> { &left, &right, &manualButton, &matchToggle })
            addAndMakeVisible (c);
        refresh();
    }

    void setMode (int m)
    {
        mode = m;
        refresh();
    }

    void refresh()
    {
        const auto st = session.getState();
        const auto& rec = session.getRecommendation();
        const bool actionable = st != RecommendationSession::State::idle && rec.outcome == Outcome::suggested;
        left.setVisible (actionable);
        right.setVisible (actionable);
        manualButton.setVisible (actionable && mode == 0);
        switch (st)
        {
            case RecommendationSession::State::previewing:
                left.setButtonText ("Reject");
                right.setButtonText ("Apply changes");
                break;
            case RecommendationSession::State::applied:
                left.setButtonText ("Undo");
                right.setButtonText (Theme::u8 ("✓ Applied"));
                break;
            case RecommendationSession::State::idle:
            case RecommendationSession::State::ready:
                left.setButtonText ("Preview");
                right.setButtonText ("Apply changes");
                break;
        }
        right.setToggleState (st == RecommendationSession::State::applied, juce::dontSendNotification);
        right.setEnabled (st != RecommendationSession::State::applied);
        manualButton.setButtonText ((bool) state.getProperty (manualProp, false) ? "Hide manual steps" : "Show me how to do this by hand");
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const int pad = 24, w = getWidth(), inner = w - 2 * pad;
        const auto st = session.getState();
        const auto& rec = session.getRecommendation();
        using S = RecommendationSession::State;

        juce::String heading = "Ready when you are", pill = "WAITING";
        juce::Colour pillColour = Theme::amber;
        if (st != S::idle)
        {
            switch (rec.outcome)
            {
                case Outcome::suggested:
                    heading = st == S::applied ? "Changes applied" : "A focused starting point";
                    pill = st == S::applied ? "APPLIED" : (st == S::previewing ? "PREVIEWING" : "READY");
                    pillColour = Theme::accent;
                    break;
                case Outcome::noChangeNeeded: heading = "No change suggested"; pill = "NO CHANGE"; pillColour = Theme::textDim; break;
                case Outcome::insufficientSignal: heading = "Not enough signal"; pill = "NEEDS AUDIO"; break;
            }
        }

        Theme::eyebrow (g, "RECOMMENDATION", { pad, 24, 160, 12 });
        g.setColour (Theme::textBright);
        g.setFont (Theme::sans (15.0f, true));
        g.drawText (heading, pad, 38, inner - 110, 22, juce::Justification::centredLeft, false);
        g.setFont (Theme::mono (8.5f, 0.08f));
        const int pw = (int) juce::GlyphArrangement::getStringWidth (Theme::mono (8.5f, 0.08f), pill) + 16;
        auto pillBox = juce::Rectangle<float> ((float) (w - pad - pw), 38.0f, (float) pw, 20.0f);
        Theme::card_ (g, pillBox, pillColour.withAlpha (0.06f), pillColour.withAlpha (0.25f), 3.0f);
        g.setColour (pillColour);
        g.drawText (pill, pillBox.toNearestInt(), juce::Justification::centred, false);

        auto content = juce::Rectangle<int> (pad, 79, inner, std::max (0, actionsTop - 79 - 28));
        {
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (content);
            drawContent (g, content, st, rec);
        }

        // loudness-match row
        auto mb = matchBounds.toFloat();
        Theme::card_ (g, mb, juce::Colour (0xff101617), juce::Colour (0xff293335), 5.0f);
        g.setColour (Theme::accent);
        g.drawEllipse ({ mb.getX() + 11.0f, mb.getCentreY() - 7.0f, 14.0f, 14.0f }, 1.4f);
        g.fillEllipse ({ mb.getX() + 14.5f, mb.getCentreY() - 3.5f, 7.0f, 7.0f });
        g.setColour (juce::Colour (0xffaebabc));
        g.setFont (Theme::sans (10.0f, true));
        g.drawText ("Match loudness when bypassed", matchBounds.getX() + 36, matchBounds.getY() + 6, 300, 14, juce::Justification::centredLeft, false);
        g.setColour (juce::Colour (0xff5f6d6f));
        g.setFont (Theme::mono (8.5f));
        const auto off = processor.getLoudnessOffsetDb();
        g.drawText ("Processing changes loudness by " + (off < 0 ? Theme::u8 ("−") : juce::String ("+")) + juce::String (std::abs (off), 1) + " dB (K-weighted)",
                    matchBounds.getX() + 36, matchBounds.getY() + 22, 380, 12, juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        const int pad = 24;
        matchBounds = { pad, getHeight() - pad - 42, getWidth() - 2 * pad, 42 };
        actionsTop = matchBounds.getY() - 12 - 38;
        auto row = juce::Rectangle<int> (pad, actionsTop, getWidth() - 2 * pad, 38);
        const int leftW = (int) ((float) row.getWidth() / 2.35f);
        left.setBounds (row.removeFromLeft (leftW));
        row.removeFromLeft (8);
        right.setBounds (row);
        manualButton.setBounds (pad, actionsTop - 24, 260, 20);
        matchToggle.setBounds (matchBounds.getRight() - 52, matchBounds.getCentreY() - 10, 42, 20);
        refreshLive();
    }

    void refreshLive()
    {
        const bool on = processor.apvts.getRawParameterValue (ParamIDs::loudnessMatch)->load() > 0.5f;
        matchToggle.setButtonText (on ? "ON" : "OFF");
        matchToggle.setToggleState (on, juce::dontSendNotification);
        matchToggle.setAccentText (on);
        repaint (matchBounds);
    }

private:
    static constexpr const char* manualProp = "manualOpen";

    void drawContent (juce::Graphics& g, juce::Rectangle<int> area, RecommendationSession::State st, const Recommendation& rec)
    {
        using S = RecommendationSession::State;
        const int pad = area.getX(), w = getWidth(), inner = area.getWidth();
        int y = area.getY();

        if (st == S::idle)
        {
            paragraph (g, "Choose what you want to hear, then press Analyze & suggest while your track is playing. "
                          "SignalPath measures the signal, proposes small reversible changes and explains them. Nothing changes until you preview or apply it.",
                       Theme::sans (11.0f), Theme::textDim, { pad, y, inner, 100 });
            return;
        }

        if (rec.outcome == Outcome::suggested)
        {
            Theme::card_ (g, { (float) pad, (float) y, 25.0f, 25.0f }, juce::Colours::transparentBlack, juce::Colour (0xff3c494b), 5.0f);
            g.setColour (Theme::accent);
            g.setFont (Theme::mono (9.0f));
            g.drawText ("01", pad, y, 25, 25, juce::Justification::centred, false);
            const int tx = pad + 37;
            y += std::max (25, paragraph (g, Theme::u8 (rec.rationale), Theme::sans (12.0f, true), juce::Colour (0xffdce5e7), { tx, y + 2, inner - 37, 60 }) + 2);
            if (! rec.interpretations.empty())
            {
                juce::String t;
                for (const auto& i : rec.interpretations)
                    t += (t.isEmpty() ? "" : " ") + Theme::u8 (i);
                y += 6 + paragraph (g, t, Theme::sans (10.0f), Theme::textDim.brighter (0.05f), { tx, y + 6, inner - 37, 70 });
            }
            y += 10;
            g.setColour (juce::Colour (0xff283032));
            g.drawHorizontalLine (y, (float) pad, (float) (pad + inner));
            y += 8;

            if (mode == 0 && (bool) state.getProperty (manualProp, false))
            {
                Theme::eyebrow (g, "HOW TO DO THIS BY HAND", { pad, y, inner, 12 });
                y += 16;
                int n = 1;
                for (const auto& step : rec.manualSteps)
                    y += 3 + paragraph (g, juce::String (n++) + ". " + Theme::u8 (step), Theme::sans (10.0f), Theme::textMid, { pad, y, inner, 40 });
                return;
            }

            const char* stateLabel = st == S::applied ? "APPLIED" : (st == S::previewing ? "PREVIEWING" : "SUGGESTED");
            for (const auto& cg : rec.changes)
            {
                const auto [badge, tone] = badgeFor (cg.module);
                auto b = juce::Rectangle<float> ((float) pad, (float) y + 4.0f, 27.0f, 27.0f);
                g.setColour (tone.withAlpha (0.10f));
                g.fillRoundedRectangle (b, 5.0f);
                g.setColour (tone);
                g.setFont (Theme::mono (9.0f));
                g.drawText (badge, b.toNearestInt(), juce::Justification::centred, false);
                g.setColour (Theme::text);
                g.setFont (Theme::sans (10.5f));
                g.drawText (Theme::u8 (cg.title), pad + 39, y + 3, inner - 140, 15, juce::Justification::centredLeft, true);
                g.setColour (juce::Colour (0xff667376));
                g.setFont (Theme::mono (9.0f));
                g.drawText (Theme::u8 (cg.detail), pad + 39, y + 19, inner - 140, 13, juce::Justification::centredLeft, true);
                g.setColour (juce::Colour (0xff6f7c7f));
                g.setFont (Theme::mono (8.5f, 0.1f));
                g.drawText (stateLabel, pad + inner - 100, y + 4, 100, 27, juce::Justification::centredRight, false);
                y += 38;
            }
            if (mode == 0)
            {
                y += 4;
                if (! rec.measurements.empty())
                {
                    Theme::eyebrow (g, "WHAT WAS MEASURED", { pad, y, inner, 12 });
                    y += 15;
                    for (const auto& m : rec.measurements)
                        y += 2 + paragraph (g, Theme::u8 (m), Theme::sans (9.5f), Theme::textDim, { pad, y, inner, 30 });
                }
                if (! rec.conceptTip.empty())
                    paragraph (g, "In short: " + Theme::u8 (rec.conceptTip), Theme::sans (9.5f), Theme::accent.withAlpha (0.8f), { pad, y + 6, inner, 40 });
            }
            juce::ignoreUnused (w);
            return;
        }

        // no change / insufficient signal
        y += paragraph (g, Theme::u8 (rec.rationale), Theme::sans (11.5f, true), juce::Colour (0xffdce5e7), { pad, y, inner, 70 }) + 8;
        if (mode == 0 || rec.outcome == Outcome::insufficientSignal)
            for (const auto& m : rec.measurements)
                y += 3 + paragraph (g, Theme::u8 (m), Theme::sans (10.0f), Theme::textDim, { pad, y, inner, 40 });
        if (! rec.interpretations.empty() && mode == 0)
        {
            y += 6;
            for (const auto& m : rec.interpretations)
                y += 3 + paragraph (g, Theme::u8 (m), Theme::sans (10.0f), Theme::textDim.brighter (0.1f), { pad, y, inner, 40 });
        }
        if (! rec.alternatives.empty())
        {
            y += 10;
            Theme::eyebrow (g, "YOU COULD TRY", { pad, y, inner, 12 });
            y += 16;
            for (const auto& a : rec.alternatives)
                y += 3 + paragraph (g, Theme::u8 (std::string ("- ") + a), Theme::sans (10.0f), Theme::textMid, { pad, y, inner, 40 });
        }
    }

    static std::pair<juce::String, juce::Colour> badgeFor (Module m)
    {
        switch (m)
        {
            case Module::eq: return { "EQ", Theme::accent };
            case Module::compressor: return { "CP", Theme::purple };
            case Module::saturation: return { "SAT", Theme::orange };
        }
        return { "", Theme::accent };
    }

    static int paragraph (juce::Graphics& g, const juce::String& text, const juce::Font& font, juce::Colour colour, juce::Rectangle<int> area)
    {
        juce::AttributedString a;
        a.append (text, font, colour);
        a.setWordWrap (juce::AttributedString::byWord);
        juce::TextLayout layout;
        layout.createLayout (a, (float) area.getWidth());
        layout.draw (g, area.toFloat());
        return (int) std::ceil (layout.getHeight());
    }

    void onLeft()
    {
        using S = RecommendationSession::State;
        switch (session.getState())
        {
            case S::ready: session.preview(); break;
            case S::previewing: session.reject(); break;
            case S::applied:
                processor.undoManager.undo();
                session.markUndone();
                break;
            case S::idle: break;
        }
        refresh();
        userAction();
    }

    void onRight()
    {
        session.apply();
        refresh();
        userAction();
    }

    PluginProcessor& processor;
    RecommendationSession& session;
    juce::ValueTree state;
    std::function<void()> userAction;
    juce::ParameterAttachment matchAttachment;
    int mode = 0;
    int actionsTop = 0;
    juce::Rectangle<int> matchBounds;
    DesignButton left { "Preview", DesignButton::Style::outline }, right { "Apply changes", DesignButton::Style::light },
        manualButton { {}, DesignButton::Style::ghost }, matchToggle { "OFF", DesignButton::Style::outline };
};
