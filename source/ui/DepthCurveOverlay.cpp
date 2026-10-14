#include "DepthCurveOverlay.h"

namespace
{
constexpr float kNodeRadius = 7.0f;
constexpr float kHitRadius = 11.0f;
const auto kCurveColour = juce::Colour (0xffe8edf2);
const auto kAccent = juce::Colour (0xff4fd1c5);
const char* const kShapeNames[] = { "Low Cut", "Low Shelf", "High Shelf", "High Cut", "Bell", "Bandpass", "Band Reject", "Tilt" };

juce::String formatHz (float hz)
{
    return hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + "k" : juce::String (juce::roundToInt (hz));
}
} // namespace

DepthCurveOverlay::DepthCurveOverlay (juce::AudioProcessorValueTreeState& state) : apvts (state)
{
    for (int b = 0; b < ParamIDs::numBands; ++b)
        for (int p = 0; p < ParamIDs::numBandParams; ++p)
            params[(size_t) b][(size_t) p] = apvts.getParameter (ParamIDs::bandParamID (b + 1, (BandParam) p));

    setOpaque (false);
    startTimerHz (30);
}

//==============================================================================
float DepthCurveOverlay::xForHz (float hz) const noexcept
{
    const auto a = plotArea();
    return a.getX() + a.getWidth() * std::log (juce::jmax (hz, 1.0f) / 20.0f) / std::log (1000.0f);
}

float DepthCurveOverlay::hzForX (float x) const noexcept
{
    const auto a = plotArea();
    return 20.0f * std::pow (1000.0f, (x - a.getX()) / a.getWidth());
}

float DepthCurveOverlay::yForDb (float db) const noexcept
{
    const auto a = plotArea();
    return a.getCentreY() - a.getHeight() * 0.5f * juce::jlimit (-maxDb, maxDb, db) / maxDb;
}

float DepthCurveOverlay::dbForY (float y) const noexcept
{
    const auto a = plotArea();
    return juce::jlimit (-maxDb, maxDb, (a.getCentreY() - y) / (a.getHeight() * 0.5f) * maxDb);
}

//==============================================================================
float DepthCurveOverlay::getValue (int band, BandParam p) const
{
    auto* param = params[(size_t) band][(size_t) p];
    return param->convertFrom0to1 (param->getValue());
}

void DepthCurveOverlay::setValue (int band, BandParam p, float plainValue)
{
    auto* param = params[(size_t) band][(size_t) p];
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

vs::BandSettings DepthCurveOverlay::readBand (int band) const
{
    vs::BandSettings s;
    s.enabled = isActive (band) && getValue (band, BandParam::enabled) >= 0.5f;
    s.shape = (vs::BandShape) juce::jlimit (0, vs::numBandShapes - 1, juce::roundToInt (getValue (band, BandParam::shape)));
    s.freqHz = getValue (band, BandParam::freq);
    s.gainDb = getValue (band, BandParam::gain);
    s.q = getValue (band, BandParam::q);
    return s;
}

vs::BandArray DepthCurveOverlay::readAll() const
{
    vs::BandArray all;
    for (int b = 0; b < ParamIDs::numBands; ++b)
        all[(size_t) b] = readBand (b);
    return all;
}

juce::Point<float> DepthCurveOverlay::nodePosition (int band) const
{
    const auto s = readBand (band);
    return { xForHz (s.freqHz), yForDb (vs::shapeUsesGain (s.shape) ? s.gainDb : 0.0f) };
}

int DepthCurveOverlay::nodeAt (juce::Point<float> p) const
{
    int best = -1;
    auto bestDistance = kHitRadius;
    for (int b = 0; b < ParamIDs::numBands; ++b)
    {
        if (! isActive (b))
            continue;

        const auto d = nodePosition (b).getDistanceFrom (p);
        if (d <= bestDistance)
        {
            best = b;
            bestDistance = d;
        }
    }
    return best;
}

//==============================================================================
void DepthCurveOverlay::paint (juce::Graphics& g)
{
    const auto a = plotArea();
    const auto bands = readAll();

    // neutral line and the scale on the right edge
    g.setColour (juce::Colours::white.withAlpha (0.18f));
    const float dashes[] = { 4.0f, 4.0f };
    g.drawDashedLine ({ a.getX(), yForDb (0.0f), a.getRight(), yForDb (0.0f) }, dashes, 2);

    g.setFont (10.0f);
    g.setColour (juce::Colours::grey);
    for (const float db : { maxDb, 12.0f, 0.0f, -12.0f, -maxDb })
    {
        const auto label = (db > 0.0f ? "+" : "") + juce::String ((int) db);
        const auto y = juce::jlimit (a.getY() + 6.0f, a.getBottom() - 6.0f, yForDb (db));
        g.drawText (label, juce::Rectangle<float> (a.getRight() - 30.0f, y - 6.0f, 28.0f, 12.0f).toNearestInt(), juce::Justification::right, false);
    }

    // the weighting curve
    juce::Path curve;
    const int steps = juce::jmax (2, (int) a.getWidth() / 2);
    for (int i = 0; i <= steps; ++i)
    {
        const auto x = a.getX() + a.getWidth() * (float) i / (float) steps;
        const auto w = vs::DepthCurve::weightAt (bands, hzForX (x));
        const auto db = 20.0f * std::log10 (juce::jmax (w, 1.0e-3f));
        const auto y = yForDb (db);
        if (i == 0)
            curve.startNewSubPath (x, y);
        else
            curve.lineTo (x, y);
    }
    g.setColour (kCurveColour.withAlpha (0.75f));
    g.strokePath (curve, juce::PathStrokeType (1.5f));

    // nodes
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    for (int b = 0; b < ParamIDs::numBands; ++b)
    {
        if (! isActive (b))
            continue;

        const auto s = readBand (b);
        const auto c = nodePosition (b);
        const auto bypassed = ! s.enabled;
        const auto circle = juce::Rectangle<float> (kNodeRadius * 2.0f, kNodeRadius * 2.0f).withCentre (c);

        if (b == selected || b == hovered)
        {
            g.setColour (kAccent.withAlpha (b == selected ? 0.55f : 0.3f));
            g.fillEllipse (circle.expanded (4.0f));
        }

        if (bypassed)
        {
            g.setColour (juce::Colour (0xff14161a));
            g.fillEllipse (circle);
            g.setColour (kCurveColour.withAlpha (0.45f));
            g.drawEllipse (circle, 1.5f);
        }
        else
        {
            g.setColour (kAccent);
            g.fillEllipse (circle);
        }

        g.setColour (bypassed ? kCurveColour.withAlpha (0.6f) : juce::Colour (0xff0e1013));
        g.drawText (juce::String (b + 1), circle.toNearestInt(), juce::Justification::centred, false);
    }

    // read-out for the node being dragged or hovered
    const auto info = dragging ? selected : hovered;
    if (info >= 0 && isActive (info))
    {
        const auto s = readBand (info);
        auto text = juce::String (kShapeNames[(int) s.shape]) + "  " + formatHz (s.freqHz) + " Hz";
        if (vs::shapeUsesGain (s.shape))
            text << "  " << (s.gainDb > 0.0f ? "+" : "") << juce::String (s.gainDb, 1) << " dB";

        const auto pos = nodePosition (info);
        auto box = juce::Rectangle<float> (150.0f, 18.0f).withCentre ({ pos.x, pos.y - 24.0f });
        box = box.withX (juce::jlimit (a.getX(), a.getRight() - box.getWidth(), box.getX()))
                  .withY (juce::jmax (a.getY() + 2.0f, box.getY()));
        g.setColour (juce::Colour (0xe61b1e24));
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (juce::Colours::white);
        g.setFont (11.0f);
        g.drawText (text, box.toNearestInt(), juce::Justification::centred, false);
    }
}

//==============================================================================
void DepthCurveOverlay::mouseMove (const juce::MouseEvent& e)
{
    const auto hit = nodeAt (e.position);
    if (hit != hovered)
    {
        hovered = hit;
        setMouseCursor (hit >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void DepthCurveOverlay::beginDrag (int band)
{
    dragging = true;
    for (auto p : { BandParam::freq, BandParam::gain })
        params[(size_t) band][(size_t) p]->beginChangeGesture();
}

void DepthCurveOverlay::endDrag()
{
    if (! dragging)
        return;

    dragging = false;
    for (auto p : { BandParam::freq, BandParam::gain })
        params[(size_t) selected][(size_t) p]->endChangeGesture();
}

void DepthCurveOverlay::mouseDown (const juce::MouseEvent& e)
{
    const auto hit = nodeAt (e.position);
    selected = hit;
    repaint();

    if (hit < 0)
        return;

    if (e.mods.isPopupMenu())
        showMenu (hit);
    else
        beginDrag (hit);
}

void DepthCurveOverlay::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging || selected < 0)
        return;

    const auto hz = juce::jlimit (20.0f, 20000.0f, hzForX (e.position.x));
    setValue (selected, BandParam::freq, hz);

    if (vs::shapeUsesGain ((vs::BandShape) juce::roundToInt (getValue (selected, BandParam::shape))))
        setValue (selected, BandParam::gain, dbForY (e.position.y));
}

void DepthCurveOverlay::mouseUp (const juce::MouseEvent&)
{
    endDrag();
}

void DepthCurveOverlay::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto hit = nodeAt (e.position);
    if (hit >= 0)
    {
        setValue (hit, BandParam::enabled, getValue (hit, BandParam::enabled) >= 0.5f ? 0.0f : 1.0f);
        return;
    }

    for (int b = 0; b < ParamIDs::numBands; ++b)
    {
        if (isActive (b))
            continue;

        setValue (b, BandParam::shape, (float) (int) vs::BandShape::bell);
        setValue (b, BandParam::freq, juce::jlimit (20.0f, 20000.0f, hzForX (e.position.x)));
        setValue (b, BandParam::gain, dbForY (e.position.y));
        setValue (b, BandParam::q, 1.0f);
        setValue (b, BandParam::enabled, 1.0f);
        setValue (b, BandParam::active, 1.0f);
        selected = b;
        return;
    }
}

void DepthCurveOverlay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto hit = nodeAt (e.position);
    if (hit < 0)
        return;

    setValue (hit, BandParam::q, getValue (hit, BandParam::q) * std::exp (wheel.deltaY * 0.8f));
    selected = hit;
}

void DepthCurveOverlay::showMenu (int band)
{
    juce::PopupMenu menu;
    const auto current = juce::roundToInt (getValue (band, BandParam::shape));
    for (int i = 0; i < vs::numBandShapes; ++i)
        menu.addItem (i + 1, kShapeNames[i], true, i == current);

    menu.addSeparator();
    const auto enabled = getValue (band, BandParam::enabled) >= 0.5f;
    menu.addItem (100, enabled ? "Bypass" : "Enable");
    menu.addItem (101, "Delete");

    juce::Component::SafePointer<DepthCurveOverlay> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options(), [safeThis, band] (int result) {
        if (safeThis == nullptr || result == 0)
            return;

        if (result == 101)
        {
            safeThis->setValue (band, BandParam::active, 0.0f);
            safeThis->selected = -1;
        }
        else if (result == 100)
        {
            safeThis->setValue (band, BandParam::enabled, safeThis->getValue (band, BandParam::enabled) >= 0.5f ? 0.0f : 1.0f);
        }
        else
        {
            const auto shape = (vs::BandShape) (result - 1);
            safeThis->setValue (band, BandParam::shape, (float) (int) shape);
            // cuts want a steeper slope than the bell Q default
            if (shape == vs::BandShape::lowCut || shape == vs::BandShape::highCut)
                safeThis->setValue (band, BandParam::q, 2.0f);
        }
    });
}
