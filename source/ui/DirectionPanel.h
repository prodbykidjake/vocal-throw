#pragma once

#include "../advisor/Advisor.h"
#include "Widgets.h"

inline void drawGoalGlyph (juce::Graphics& g, Goal goal, juce::Rectangle<float> r, juce::Colour c)
{
    const auto cx = r.getCentreX(), cy = r.getCentreY(), s = std::min (r.getWidth(), r.getHeight()) * 0.5f;
    g.setColour (c);
    const juce::PathStrokeType stroke (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    juce::Path p;
    switch (goal)
    {
        case Goal::clarity: // half-filled circle
            g.drawEllipse (cx - s, cy - s, 2 * s, 2 * s, 1.6f);
            p.addCentredArc (cx, cy, s, s, 0, juce::MathConstants<float>::pi, juce::MathConstants<float>::twoPi, true);
            p.closeSubPath();
            g.fillPath (p);
            break;
        case Goal::warmth: // two waves
            for (int k = -1; k <= 1; k += 2)
            {
                juce::Path w;
                w.startNewSubPath (cx - s, cy + (float) k * s * 0.35f);
                w.cubicTo (cx - s * 0.5f, cy + (float) k * s * 0.35f - s * 0.5f, cx - s * 0.1f, cy + (float) k * s * 0.35f + s * 0.5f, cx + s * 0.4f, cy + (float) k * s * 0.35f - s * 0.1f);
                w.lineTo (cx + s, cy + (float) k * s * 0.35f);
                g.strokePath (w, stroke);
            }
            break;
        case Goal::punch: // bolt
            p.startNewSubPath (cx - s * 0.6f, cy - s * 0.2f);
            p.lineTo (cx + s * 0.1f, cy - s * 0.2f);
            p.lineTo (cx - s * 0.1f, cy + s * 0.25f);
            p.lineTo (cx + s * 0.7f, cy + s * 0.25f);
            g.strokePath (p, stroke);
            break;
        case Goal::presence: // diamond
            p.addQuadrilateral (cx, cy - s, cx + s, cy, cx, cy + s, cx - s, cy);
            g.strokePath (p, stroke);
            break;
        case Goal::density: // stacked bars
            for (int i = 0; i < 3; ++i)
                g.fillRoundedRectangle (cx - s, cy - s * 0.8f + (float) i * s * 0.8f, 2 * s, s * 0.35f, 1.5f);
            break;
        case Goal::smoothness: // sine
            p.startNewSubPath (cx - s, cy);
            p.cubicTo (cx - s * 0.5f, cy - s * 1.2f, cx - s * 0.25f, cy - s * 1.2f, cx, cy);
            p.cubicTo (cx + s * 0.25f, cy + s * 1.2f, cx + s * 0.5f, cy + s * 1.2f, cx + s, cy);
            g.strokePath (p, stroke);
            break;
        case Goal::brightness: // sun
            g.drawEllipse (cx - s * 0.4f, cy - s * 0.4f, s * 0.8f, s * 0.8f, 1.6f);
            for (int i = 0; i < 8; ++i)
            {
                const auto a = juce::MathConstants<float>::twoPi * (float) i / 8.0f;
                g.drawLine (cx + std::cos (a) * s * 0.65f, cy + std::sin (a) * s * 0.65f, cx + std::cos (a) * s, cy + std::sin (a) * s, 1.4f);
            }
            break;
        case Goal::body: // bowl
            p.addCentredArc (cx, cy - s * 0.2f, s, s, 0, juce::MathConstants<float>::halfPi, juce::MathConstants<float>::pi * 1.5f, true);
            p.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::pi, cx, cy).scaled (1.0f, -1.0f, cx, cy));
            g.strokePath (p, stroke);
            g.drawLine (cx - s, cy + s * 0.55f, cx + s, cy + s * 0.55f, 1.6f);
            break;
        case Goal::dynamicControl: // converging lines
            g.drawLine (cx - s, cy - s * 0.8f, cx, cy - s * 0.1f, 1.6f);
            g.drawLine (cx, cy - s * 0.1f, cx + s, cy - s * 0.1f, 1.6f);
            g.drawLine (cx - s, cy + s * 0.8f, cx, cy + s * 0.1f, 1.6f);
            g.drawLine (cx, cy + s * 0.1f, cx + s, cy + s * 0.1f, 1.6f);
            break;
        case Goal::reducedHarshness: // spike with a notch
            p.startNewSubPath (cx - s, cy);
            p.lineTo (cx - s * 0.3f, cy);
            p.lineTo (cx - s * 0.1f, cy + s * 0.9f);
            p.lineTo (cx + s * 0.1f, cy);
            p.lineTo (cx + s, cy);
            g.strokePath (p, stroke);
            break;
    }
}

inline void drawSparkle (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    const auto cx = r.getCentreX(), cy = r.getCentreY(), s = std::min (r.getWidth(), r.getHeight()) * 0.5f;
    juce::Path p;
    p.startNewSubPath (cx, cy - s);
    p.quadraticTo (cx, cy, cx + s, cy);
    p.quadraticTo (cx, cy, cx, cy + s);
    p.quadraticTo (cx, cy, cx - s, cy);
    p.quadraticTo (cx, cy, cx, cy - s);
    g.setColour (c);
    g.fillPath (p);
}

class GoalCard : public juce::Button
{
public:
    explicit GoalCard (Goal gl) : juce::Button (goalName (gl)), goal (gl) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto r = getLocalBounds().toFloat();
        const bool active = getToggleState();
        if (active)
        {
            g.setGradientFill (juce::ColourGradient (Theme::accent.withAlpha (0.10f), r.getX(), r.getY(), Theme::accent.withAlpha (0.025f), r.getRight(), r.getBottom(), false));
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (Theme::accent.withAlpha (0.6f));
        }
        else
        {
            g.setColour (Theme::card);
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (over ? juce::Colour (0xff3d4a4d) : Theme::borderCard);
        }
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
        drawGoalGlyph (g, goal, { r.getX() + 12.0f, r.getCentreY() - 9.0f, 18.0f, 18.0f }, Theme::accent);
        g.setColour (active ? juce::Colour (0xffe2eeec) : juce::Colour (0xff899598));
        g.setFont (Theme::sans (11.5f, true));
        g.drawText (goalName (goal), (int) r.getX() + 42, (int) r.getY() + 6, getWidth() - 48, 16, juce::Justification::centredLeft, true);
        g.setColour (juce::Colour (0xff667376));
        g.setFont (Theme::sans (9.5f));
        g.drawText (goalTagline (goal), (int) r.getX() + 42, (int) r.getY() + 22, getWidth() - 48, 14, juce::Justification::centredLeft, true);
    }

    Goal goal;
};

class IntensityLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle, juce::Slider&) override
    {
        const auto cy = (float) y + (float) h * 0.5f;
        g.setColour (juce::Colour (0xff273033));
        g.fillRoundedRectangle ((float) x, cy - 1.5f, (float) w, 3.0f, 1.5f);
        g.setColour (Theme::accent);
        g.fillRoundedRectangle ((float) x, cy - 1.5f, pos - (float) x, 3.0f, 1.5f);
        g.setColour (Theme::accent.withAlpha (0.25f));
        g.fillEllipse (pos - 9.0f, cy - 9.0f, 18.0f, 18.0f);
        g.setColour (Theme::accent);
        g.fillEllipse (pos - 6.0f, cy - 6.0f, 12.0f, 12.0f);
    }
};

// "What do you want to hear?": source, ten goals, intensity and the Analyze action.
class DirectionPanel : public juce::Component
{
public:
    DirectionPanel (juce::ValueTree stateTree, std::function<void()> onSelectionChanged, std::function<void()> onAnalyse)
        : state (std::move (stateTree)), selectionChanged (std::move (onSelectionChanged))
    {
        for (int i = 0; i < numGoals; ++i)
        {
            cards.push_back (std::make_unique<GoalCard> ((Goal) i));
            auto& c = *cards.back();
            c.setRadioGroupId (9100);
            c.setClickingTogglesState (true);
            c.onClick = [this, i] {
                state.setProperty (PluginProcessor::goal_, i, nullptr);
                selectionChanged();
            };
            addAndMakeVisible (c);
        }

        sourceButton.setStyle (DesignButton::Style::outline);
        sourceButton.setTextFont (Theme::sans (10.5f));
        sourceButton.iconOnRight = true;
        sourceButton.icon = [] (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c) { drawChevron (g, r, c); };
        sourceButton.onClick = [this] { showSourceMenu(); };
        addAndMakeVisible (sourceButton);

        intensitySlider.setSliderStyle (juce::Slider::LinearHorizontal);
        intensitySlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        intensitySlider.setRange (0.0, 100.0, 1.0);
        intensitySlider.setLookAndFeel (&sliderLnf);
        intensitySlider.setDoubleClickReturnValue (true, 50.0);
        intensitySlider.onValueChange = [this] {
            state.setProperty (PluginProcessor::intensity_, intensitySlider.getValue() / 100.0, nullptr);
            repaint();
            selectionChanged();
        };
        addAndMakeVisible (intensitySlider);

        analyseButton.setStyle (DesignButton::Style::primary);
        analyseButton.setTextFont (Theme::sans (11.5f, true));
        analyseButton.icon = drawSparkle;
        analyseButton.onClick = std::move (onAnalyse);
        addAndMakeVisible (analyseButton);
        refresh();
    }

    ~DirectionPanel() override { intensitySlider.setLookAndFeel (nullptr); }

    void refresh()
    {
        const int goal = juce::jlimit (0, numGoals - 1, (int) state.getProperty (PluginProcessor::goal_, 0));
        const int src = juce::jlimit (0, numSources - 1, (int) state.getProperty (PluginProcessor::source_, 0));
        for (int i = 0; i < numGoals; ++i)
            cards[(size_t) i]->setToggleState (i == goal, juce::dontSendNotification);
        sourceButton.setButtonText (sourceName ((Source) src));
        intensitySlider.setValue (100.0 * (double) (float) state.getProperty (PluginProcessor::intensity_, 0.5), juce::dontSendNotification);
        repaint();
    }

    void setAnalysing (bool busy)
    {
        analyseButton.setButtonText (busy ? "Listening to your signal..." : "Analyze & suggest");
        analyseButton.setEnabled (! busy);
    }

    void paint (juce::Graphics& g) override
    {
        const int pad = 24;
        Theme::eyebrow (g, "DIRECTION", { pad, 24, 120, 12 });
        g.setColour (Theme::textBright);
        g.setFont (Theme::sans (15.0f, true));
        g.drawText ("What do you want to hear?", pad, 38, 330, 22, juce::Justification::centredLeft, false);

        auto box = intensityBox.toFloat();
        Theme::card_ (g, box, Theme::cardDark, juce::Colour (0xff293234), 6.0f);
        const float v = (float) intensitySlider.getValue() / 100.0f;
        g.setColour (Theme::textMuted.brighter (0.1f));
        g.setFont (Theme::mono (8.5f, 0.11f));
        g.drawText ("INTENSITY", intensityBox.getX() + 12, intensityBox.getY() + 11, 90, 12, juce::Justification::centredLeft, false);
        g.setColour (Theme::text);
        g.setFont (Theme::sans (11.0f, true));
        g.drawText (intensityName (v), intensityBox.getX() + 12, intensityBox.getY() + 26, 90, 14, juce::Justification::centredLeft, false);
        g.setColour (Theme::accent);
        g.setFont (Theme::mono (11.0f));
        g.drawText (juce::String ((int) std::lround (v * 100.0f)) + "%", intensityBox.getRight() - 52, intensityBox.getY(), 40, intensityBox.getHeight(), juce::Justification::centredRight, false);

        g.setColour (juce::Colour (0xff5f6c6f));
        g.setFont (Theme::sans (10.0f));
        g.drawText ("Play the loudest section of your track for the most useful result.", 0, analyseButton.getBottom() + 8, getWidth(), 14, juce::Justification::centred, false);
    }

    void resized() override
    {
        const int pad = 24;
        sourceButton.setBounds (getWidth() - pad - 108, 24, 108, 28);
        const int gridTop = 79, gap = 8, cardH = 42, vgap = 5;
        const int cardW = (getWidth() - 2 * pad - gap) / 2;
        for (int i = 0; i < numGoals; ++i)
            cards[(size_t) i]->setBounds (pad + (i % 2) * (cardW + gap), gridTop + (i / 2) * (cardH + vgap), cardW, cardH);
        const int gridBottom = gridTop + 5 * cardH + 4 * vgap;
        intensityBox = { pad, gridBottom + 14, getWidth() - 2 * pad, 50 };
        intensitySlider.setBounds (intensityBox.getX() + 104, intensityBox.getY() + 7, intensityBox.getWidth() - 104 - 58, 36);
        analyseButton.setBounds (pad, intensityBox.getBottom() + 14, getWidth() - 2 * pad, 42);
    }

private:
    void showSourceMenu()
    {
        juce::PopupMenu m;
        const int current = (int) state.getProperty (PluginProcessor::source_, 0);
        for (int i = 0; i < numSources; ++i)
            m.addItem (i + 1, sourceName ((Source) i), true, i == current);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&sourceButton), [this, safe = juce::Component::SafePointer<DirectionPanel> (this)] (int r) {
            if (safe == nullptr || r == 0)
                return;
            state.setProperty (PluginProcessor::source_, r - 1, nullptr);
            refresh();
            selectionChanged();
        });
    }

    juce::ValueTree state;
    std::function<void()> selectionChanged;
    std::vector<std::unique_ptr<GoalCard>> cards;
    DesignButton sourceButton { "Lead vocal", DesignButton::Style::outline };
    juce::Slider intensitySlider;
    IntensityLookAndFeel sliderLnf;
    DesignButton analyseButton { "Analyze & suggest", DesignButton::Style::primary };
    juce::Rectangle<int> intensityBox;
};
