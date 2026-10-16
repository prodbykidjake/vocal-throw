#include "BandEditor.h"
#include "Fonts.h"
#include "Icons.h"

namespace vox
{
namespace
{
    constexpr int padX = 18;
    constexpr int colGap = 13;
    constexpr int headingW = 95;
    constexpr int sliderW = 104;
    constexpr int shapesW = 236;
}

class BandEditor::ShapeButton : public juce::Button
{
public:
    ShapeButton (Theme& t, vs::BandShape s) : juce::Button (juce::String ((int) s)), theme (t), shape (s)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    bool selected = false;
    juce::Colour accent;

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        const auto area = getLocalBounds().toFloat();
        if (selected)
        {
            g.setColour (theme.adj (accent));
            g.fillRoundedRectangle (area, 4.0f);
        }
        else if (over)
        {
            g.setColour (theme.p().tint.withAlpha (0.15f));
            g.fillRoundedRectangle (area, 4.0f);
        }

        icons::stroke (g, icons::bandShape (shape), area.withSizeKeepingCentre (18.0f, 17.0f),
            selected ? theme.p().onAccent : theme.p().muted, 1.8f);
    }

private:
    Theme& theme;
    vs::BandShape shape;
};

class BandEditor::PowerButton : public juce::Button
{
public:
    explicit PowerButton (Theme& t) : juce::Button ("power"), theme (t)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("Bypass or enable this band");
    }

    juce::Colour accent;
    bool bandEnabled = true;

    void paintButton (juce::Graphics& g, bool, bool) override
    {
        const auto colour = theme.adj (accent);
        const auto circle = getLocalBounds().toFloat().withSizeKeepingCentre (22.0f, 22.0f).reduced (1.0f);
        g.setColour (colour.withAlpha (bandEnabled ? 1.0f : 0.6f));
        if (bandEnabled)
        {
            g.drawEllipse (circle, 2.0f);
            g.fillEllipse (circle.withSizeKeepingCentre (7.0f, 7.0f));
        }
        else
        {
            juce::Path ring;
            ring.addEllipse (circle);
            juce::Path dashed;
            const float dashes[] = { 3.0f, 2.5f };
            juce::PathStrokeType (2.0f).createDashedStroke (dashed, ring, dashes, 2);
            g.fillPath (dashed);
        }
    }

private:
    Theme& theme;
};

BandEditor::BandEditor (Theme& t, juce::AudioProcessorValueTreeState& s)
    : theme (t),
      apvts (s),
      frequency (t, s, ParamIDs::bandParamID (1, ParamIDs::BandParam::freq), "FREQUENCY", fmt::hertz),
      sensitivity (t, s, ParamIDs::bandParamID (1, ParamIDs::BandParam::gain), "SENSITIVITY", fmt::decibels),
      q (t, s, ParamIDs::bandParamID (1, ParamIDs::BandParam::q), "Q", fmt::number),
      balance (t, s, ParamIDs::bandParamID (1, ParamIDs::BandParam::balance), "BALANCE", fmt::balance)
{
    for (auto* c : { &frequency, &sensitivity, &q, &balance })
        addAndMakeVisible (c);

    for (int i = 0; i < vs::numBandShapes; ++i)
    {
        shapes[(size_t) i] = std::make_unique<ShapeButton> (t, (vs::BandShape) i);
        shapes[(size_t) i]->onClick = [this, i] {
            if (band >= 0)
            {
                apvts.undoManager->beginNewTransaction ("Band shape");
                setParam (ParamIDs::BandParam::shape, (float) i);
                // cuts want a steeper slope than a bell's Q
                const auto shape = (vs::BandShape) i;
                if (shape == vs::BandShape::lowCut || shape == vs::BandShape::highCut)
                    setParam (ParamIDs::BandParam::q, 2.0f);
            }
        };
        static const char* const names[] = { "Low cut", "Low shelf", "High shelf", "High cut", "Bell", "Bandpass", "Band reject", "Tilt" };
        shapes[(size_t) i]->setTooltip (names[i]);
        addAndMakeVisible (*shapes[(size_t) i]);
    }

    power = std::make_unique<PowerButton> (t);
    power->onClick = [this] {
        if (band >= 0)
        {
            apvts.undoManager->beginNewTransaction ("Bypass band");
            const auto* enabled = param (ParamIDs::BandParam::enabled);
            setParam (ParamIDs::BandParam::enabled, enabled->getValue() >= 0.5f ? 0.0f : 1.0f);
        }
    };
    addAndMakeVisible (*power);

    startTimerHz (15);
    setBand (-1);
}

BandEditor::~BandEditor() = default;

juce::RangedAudioParameter* BandEditor::param (ParamIDs::BandParam which) const
{
    return apvts.getParameter (ParamIDs::bandParamID (band + 1, which));
}

void BandEditor::setParam (ParamIDs::BandParam which, float plainValue)
{
    if (auto* p = param (which))
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
}

void BandEditor::setBand (int newBand)
{
    band = juce::jlimit (-1, ParamIDs::numBands - 1, newBand);
    const auto id = [&] (ParamIDs::BandParam which) { return ParamIDs::bandParamID (juce::jmax (band, 0) + 1, which); };
    using ParamIDs::BandParam;
    frequency.bindTo (apvts, id (BandParam::freq));
    sensitivity.bindTo (apvts, id (BandParam::gain));
    q.bindTo (apvts, id (BandParam::q));
    balance.bindTo (apvts, id (BandParam::balance));

    const auto colour = Theme::bandColour (juce::jmax (band, 0));
    for (auto* c : { &frequency, &sensitivity, &q, &balance })
        c->setAccent (colour);
    for (auto& s : shapes)
        s->accent = colour;
    power->accent = colour;

    shownShape = -1;
    refresh();
}

void BandEditor::refresh()
{
    const auto active = band >= 0 && param (ParamIDs::BandParam::active)->getValue() >= 0.5f;
    const auto enabled = active && param (ParamIDs::BandParam::enabled)->getValue() >= 0.5f;
    const auto shape = active ? juce::roundToInt (param (ParamIDs::BandParam::shape)->convertFrom0to1 (param (ParamIDs::BandParam::shape)->getValue())) : -1;

    if (active != shownActive || enabled != shownEnabled || shape != shownShape)
    {
        shownActive = active;
        shownEnabled = enabled;
        shownShape = shape;

        const auto look = active && enabled;
        for (auto* c : { &frequency, &sensitivity, &q, &balance })
            c->setEnabledLook (look);
        for (int i = 0; i < vs::numBandShapes; ++i)
        {
            shapes[(size_t) i]->selected = active && i == shape;
            shapes[(size_t) i]->setEnabled (active);
            shapes[(size_t) i]->setAlpha (look ? 1.0f : 0.35f);
            shapes[(size_t) i]->repaint();
        }
        power->bandEnabled = enabled;
        power->setEnabled (active);
        power->repaint();
        repaint();
    }
}

void BandEditor::timerCallback()
{
    refresh();
}

void BandEditor::resized()
{
    power->setBounds (padX, 19, 22, 22);

    int x = padX + headingW + colGap;
    const auto y = (height - ValueSlider::height) / 2;
    for (auto* c : { &frequency, &sensitivity, &q, &balance })
    {
        c->setBounds (x, y, sliderW, ValueSlider::height);
        x += sliderW + colGap;
    }

    // eight 26 px buttons, evenly spread across the last column
    const auto gap = (float) (shapesW - 8 * 26) / 7.0f;
    for (int i = 0; i < vs::numBandShapes; ++i)
        shapes[(size_t) i]->setBounds (juce::roundToInt ((float) x + (float) i * (26.0f + gap)), (height - 32) / 2, 26, 32);
}

void BandEditor::paint (juce::Graphics& g)
{
    const auto& pal = theme.p();
    const auto area = getLocalBounds().toFloat().reduced (1.0f);
    juce::Path p;
    p.addRoundedRectangle (area, 10.0f);
    juce::DropShadow (pal.shade.withAlpha (0.17f), 35, { 0, 15 }).drawForPath (g, p);

    g.setColour (pal.panel.withAlpha (0.91f));
    g.fillPath (p);
    g.setColour (theme.sheen (0.25f));
    g.strokePath (p, juce::PathStrokeType (1.0f));

    const auto active = band >= 0 && shownActive;
    const auto text = band >= 0 ? "BAND " + juce::String (band + 1) : juce::String ("NO BAND");
    fonts::draw (g, text, { (float) padX + 30.0f, 19.0f, 80.0f, 22.0f }, fonts::make (700, 10.0f, 0.07f),
        pal.ink.withMultipliedAlpha (active && shownEnabled ? 1.0f : 0.35f), juce::Justification::centredLeft);

    if (! active)
        fonts::draw (g, "Double-click the graph to add a band", { (float) padX + headingW + colGap, 8.0f, 400.0f, 14.0f }, fonts::make (400, 10.0f),
            pal.soft, juce::Justification::centredLeft);
}
} // namespace vox
