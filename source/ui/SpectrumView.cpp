#include "SpectrumView.h"
#include "Fonts.h"
#include "Icons.h"

namespace vox
{
namespace
{
    constexpr float sidePad = 33.0f;           // the frequency axis starts and ends this far from the edges
    constexpr float zeroFraction = 0.4875f;    // the zero line sits at 312 of the design's 640-unit viewBox
    constexpr float gridStepPx = 93.4f;        // the design's horizontal grid spacing (6 dB)
    constexpr float dbPerGrid = 6.0f;
    constexpr float freqLabelHeight = 32.0f;

    const char* const shapeNames[] = { "Low Cut", "Low Shelf", "High Shelf", "High Cut", "Bell", "Bandpass", "Band Reject", "Tilt" };

    juce::Colour rgba (int r, int g, int b, float a) { return juce::Colour::fromFloatRGBA ((float) r / 255.0f, (float) g / 255.0f, (float) b / 255.0f, a); }

    juce::String frequencyLabel (float hz) { return hz >= 1000.0f ? juce::String (juce::roundToInt (hz / 1000.0f)) + "k" : juce::String (juce::roundToInt (hz)); }
}

SpectrumView::SpectrumView (Theme& t, PluginProcessor& p) : theme (t), processor (p), apvts (p.apvts)
{
    for (int b = 0; b < ParamIDs::numBands; ++b)
        for (int q = 0; q < ParamIDs::numBandParams; ++q)
            params[(size_t) b][(size_t) q] = apvts.getParameter (ParamIDs::bandParamID (b + 1, (BandParam) q));

    setOpaque (true);
    startTimerHz (30);
}

//==============================================================================
float SpectrumView::pxPerDb() const noexcept { return gridStepPx / dbPerGrid * ((float) getHeight() / 766.0f); }
float SpectrumView::zeroLineY() const noexcept { return (float) getHeight() * zeroFraction; }

float SpectrumView::xForHz (float hz) const noexcept
{
    return sidePad + ((float) getWidth() - 2.0f * sidePad) * std::log (juce::jmax (hz, 1.0f) / 20.0f) / std::log (1000.0f);
}

float SpectrumView::hzForX (float x) const noexcept
{
    return 20.0f * std::pow (1000.0f, (x - sidePad) / ((float) getWidth() - 2.0f * sidePad));
}

float SpectrumView::yForDb (float db) const noexcept { return zeroLineY() - juce::jlimit (-maxDb, maxDb, db) * pxPerDb(); }
float SpectrumView::dbForY (float y) const noexcept { return juce::jlimit (-maxDb, maxDb, (zeroLineY() - y) / pxPerDb()); }

juce::Rectangle<float> SpectrumView::addButtonArea() const noexcept
{
    return { (float) getWidth() - 27.0f - 30.0f, (float) getHeight() - 22.0f - 30.0f, 30.0f, 30.0f };
}

//==============================================================================
float SpectrumView::getValue (int band, BandParam p) const
{
    auto* param = params[(size_t) band][(size_t) p];
    return param->convertFrom0to1 (param->getValue());
}

void SpectrumView::setValue (int band, BandParam p, float plainValue)
{
    auto* param = params[(size_t) band][(size_t) p];
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

vs::BandSettings SpectrumView::readBand (int band) const
{
    vs::BandSettings s;
    s.enabled = isActive (band) && getValue (band, BandParam::enabled) >= 0.5f;
    s.shape = (vs::BandShape) juce::jlimit (0, vs::numBandShapes - 1, juce::roundToInt (getValue (band, BandParam::shape)));
    s.freqHz = getValue (band, BandParam::freq);
    s.gainDb = getValue (band, BandParam::gain);
    s.q = getValue (band, BandParam::q);
    s.balance = getValue (band, BandParam::balance) / 50.0f;
    return s;
}

vs::BandArray SpectrumView::readAll() const
{
    vs::BandArray all;
    for (int b = 0; b < ParamIDs::numBands; ++b)
        all[(size_t) b] = readBand (b);
    return all;
}

juce::Point<float> SpectrumView::nodePosition (int band) const
{
    const auto s = readBand (band);
    return { xForHz (s.freqHz), yForDb (vs::shapeUsesGain (s.shape) ? s.gainDb : 0.0f) };
}

int SpectrumView::nodeAt (juce::Point<float> p) const
{
    int best = -1;
    auto bestDistance = nodeRadius + 4.0f;
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

void SpectrumView::setSelectedBand (int band)
{
    select (band);
}

void SpectrumView::select (int band)
{
    if (band == selected)
        return;

    selected = band;
    if (onSelectionChanged)
        onSelectionChanged (selected);
    repaint();
}

int SpectrumView::addBand (float hz, float gainDb)
{
    for (int b = 0; b < ParamIDs::numBands; ++b)
    {
        if (isActive (b))
            continue;

        apvts.undoManager->beginNewTransaction ("Add band");
        setValue (b, BandParam::shape, (float) (int) vs::BandShape::bell);
        setValue (b, BandParam::freq, juce::jlimit (20.0f, 20000.0f, hz));
        setValue (b, BandParam::gain, juce::jlimit (-18.0f, 18.0f, gainDb));
        setValue (b, BandParam::q, 1.0f);
        setValue (b, BandParam::balance, 0.0f);
        setValue (b, BandParam::enabled, 1.0f);
        setValue (b, BandParam::active, 1.0f);
        select (b);
        return b;
    }
    return -1;
}

void SpectrumView::timerCallback()
{
    processor.getReductionDisplay (reduction.data());

    // quick to rise, slower to fall, so short cuts stay visible
    for (size_t i = 0; i < shown.size(); ++i)
        shown[i] = reduction[i] > shown[i] ? reduction[i] : shown[i] + 0.3f * (reduction[i] - shown[i]);

    // a deleted band can't stay selected
    if (selected >= 0 && ! isActive (selected))
    {
        int next = -1;
        for (int b = 0; b < ParamIDs::numBands && next < 0; ++b)
            if (isActive (b))
                next = b;
        select (next);
    }

    repaint();
}

//==============================================================================
void SpectrumView::resized() {}

void SpectrumView::paint (juce::Graphics& g)
{
    const auto& pal = theme.p();
    const auto bypassed = theme.isBypassed();
    const auto w = (float) getWidth();
    const auto h = (float) getHeight();
    const auto zeroY = zeroLineY();
    const auto ppd = pxPerDb();

    // CSS: the whole graph goes grey and 45 % opaque when bypassed
    if (bypassed)
        g.beginTransparencyLayer (0.45f);
    auto tone = [&] (juce::Colour c) { return bypassed ? c.withSaturation (0.0f) : c; };

    g.fillAll (tone (pal.graph));
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.14f), 0.0f, 0.0f, juce::Colours::white.withAlpha (0.0f), 0.0f, h * 0.38f, false));
    g.fillRect (0.0f, 0.0f, w, h * 0.38f);

    // grid: 6 dB steps, and the standard frequencies
    g.setColour (rgba (245, 251, 252, 0.22f));
    for (int i = -3; i <= 3; ++i)
        if (i != 0)
            g.fillRect (0.0f, zeroY - (float) i * dbPerGrid * ppd - 0.5f, w, 1.0f);
    for (const float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        g.fillRect (xForHz (hz) - 0.5f, 0.0f, 1.0f, h);

    // frequency labels along the top
    const auto labelColour = tone (rgba (252, 255, 255, 0.88f));
    for (const float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
        fonts::draw (g, frequencyLabel (hz), juce::Rectangle<float> (40.0f, freqLabelHeight).withCentre ({ xForHz (hz), freqLabelHeight * 0.5f }),
            fonts::make (400, 12.0f), labelColour);

    // dB labels on the right
    const auto minus = juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"));
    for (int i = 3; i >= -3; --i)
    {
        const auto db = i * (int) dbPerGrid;
        const auto text = db > 0 ? "+" + juce::String (db) : (db < 0 ? minus + juce::String (-db) : juce::String ("0"));
        fonts::draw (g, text, juce::Rectangle<float> (w - 11.0f - 22.0f, zeroY - (float) i * dbPerGrid * ppd - 8.0f, 22.0f, 16.0f),
            fonts::make (400, 11.0f), tone (rgba (247, 252, 253, 0.86f)), juce::Justification::centredLeft);
    }

    // zero line
    g.setColour (tone (pal.coral));
    g.fillRect (0.0f, zeroY - 0.75f, w, 1.5f);

    // the gain reduction: a wash and a glowing white line hanging below the zero line
    {
        juce::Path line;
        float deepest = zeroY;
        line.startNewSubPath (0.0f, zeroY);
        for (int i = 0; i < (int) shown.size(); ++i)
        {
            const auto x = xForHz (vs::ResonanceSuppressor::displayPointFrequency (i));
            const auto y = juce::jmin (zeroY + shown[(size_t) i] * ppd, h - 4.0f);
            deepest = juce::jmax (deepest, y);
            line.lineTo (x, y);
        }
        line.lineTo (w, zeroY);

        juce::Path wash (line);
        wash.lineTo (w, zeroY);
        wash.closeSubPath();

        const auto top = theme.isDelta() ? juce::Colour (0xfff3ad8a) : juce::Colour (0xffc6e8eb);
        const auto bottom = theme.isDelta() ? juce::Colour (0xffeb8663) : juce::Colour (0xff77b3bf);
        g.setGradientFill (juce::ColourGradient (tone (top).withAlpha (0.75f), 0.0f, zeroY, tone (bottom).withAlpha (0.08f), 0.0f, juce::jmax (zeroY + 20.0f, deepest), false));
        g.fillPath (wash);

        // soft glow, then the line itself. With nothing being reduced it would just hide the coral zero line.
        if (deepest - zeroY > 0.75f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.14f));
            g.strokePath (line, juce::PathStrokeType (9.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (juce::Colours::white.withAlpha (0.2f));
            g.strokePath (line, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (rgba (251, 255, 255, 0.95f));
            g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    // the depth curve
    {
        const auto bands = readAll();
        juce::Path curve;
        const int steps = juce::jmax (2, (int) w / 3);
        for (int i = 0; i <= steps; ++i)
        {
            const auto x = (float) i / (float) steps * w;
            const auto weight = vs::DepthCurve::weightAt (bands, hzForX (x));
            const auto y = yForDb (20.0f * std::log10 (juce::jmax (weight, 1.0e-3f)));
            if (i == 0)
                curve.startNewSubPath (x, y);
            else
                curve.lineTo (x, y);
        }

        juce::Path dashed;
        const float dashes[] = { 5.0f, 4.0f };
        juce::PathStrokeType (1.5f).createDashedStroke (dashed, curve, dashes, 2);
        g.setColour (tone (rgba (60, 85, 97, 0.62f)));
        g.fillPath (dashed);
    }

    // nodes: the selected one is drawn last, on top
    std::vector<int> order;
    for (int b = 0; b < ParamIDs::numBands; ++b)
        if (isActive (b) && b != selected)
            order.push_back (b);
    if (selected >= 0 && isActive (selected))
        order.push_back (selected);

    for (const auto b : order)
    {
        const auto s = readBand (b);
        const auto centre = nodePosition (b);
        const auto emphasised = b == selected || b == hovered;
        const auto radius = nodeRadius * (emphasised ? 1.16f : 1.0f);
        const auto circle = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);
        const auto colour = tone (Theme::bandColour (b));

        if (b == selected)
        {
            g.setColour (juce::Colours::white.withAlpha (0.23f));
            g.fillEllipse (circle.expanded (7.0f));
        }

        juce::Path shadowShape;
        shadowShape.addEllipse (circle);
        juce::DropShadow (rgba (53, 75, 84, 0.25f), 5, { 0, 2 }).drawForPath (g, shadowShape);

        if (s.enabled)
        {
            g.setColour (colour);
            g.fillEllipse (circle);
            g.setColour (rgba (255, 255, 255, 0.86f));
            g.drawEllipse (circle.reduced (1.0f), 2.0f);
        }
        else
        {
            // bypassed: hollow and dim
            g.setColour (colour.withAlpha (0.28f));
            g.fillEllipse (circle);
            g.setColour (rgba (255, 255, 255, 0.6f));
            juce::Path ring;
            ring.addEllipse (circle.reduced (1.0f));
            juce::Path dashedRing;
            const float ringDashes[] = { 3.0f, 3.0f };
            juce::PathStrokeType (2.0f).createDashedStroke (dashedRing, ring, ringDashes, 2);
            g.fillPath (dashedRing);
        }
    }

    // "+" add band
    {
        const auto area = addButtonArea();
        g.setColour (rgba (81, 112, 126, overAdd ? 0.3f : 0.17f));
        g.fillEllipse (area);
        g.setColour (rgba (255, 255, 255, 0.65f));
        g.drawEllipse (area.reduced (0.5f), 1.0f);
        icons::stroke (g, icons::plus(), area.withSizeKeepingCentre (14.0f, 14.0f), juce::Colours::white, 1.6f);
    }

    // read-out for the node being dragged or hovered
    const auto info = dragging ? selected : hovered;
    if (info >= 0 && isActive (info))
    {
        const auto s = readBand (info);
        auto text = juce::String (shapeNames[(int) s.shape]) + "  " + (s.freqHz >= 1000.0f ? juce::String (s.freqHz / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (s.freqHz)) + " Hz");
        if (vs::shapeUsesGain (s.shape))
            text << "  " << (s.gainDb > 0.0f ? "+" : "") << juce::String (s.gainDb, 1) << " dB";

        const auto pos = nodePosition (info);
        auto box = juce::Rectangle<float> (176.0f, 22.0f).withCentre ({ pos.x, pos.y - 34.0f });
        box = box.withX (juce::jlimit (4.0f, w - 4.0f - box.getWidth(), box.getX())).withY (juce::jmax (freqLabelHeight, box.getY()));
        g.setColour (rgba (52, 67, 75, 0.94f));
        g.fillRoundedRectangle (box, 5.0f);
        fonts::draw (g, text, box, fonts::make (500, 11.0f), juce::Colours::white);
    }

    if (bypassed)
        g.endTransparencyLayer();
}

//==============================================================================
void SpectrumView::mouseMove (const juce::MouseEvent& e)
{
    const auto hit = nodeAt (e.position);
    const auto add = addButtonArea().contains (e.position);
    if (hit != hovered || add != overAdd)
    {
        hovered = hit;
        overAdd = add;
        setMouseCursor (hit >= 0 ? juce::MouseCursor::DraggingHandCursor : (add ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor));
        repaint();
    }
}

void SpectrumView::mouseExit (const juce::MouseEvent&)
{
    hovered = -1;
    overAdd = false;
    repaint();
}

void SpectrumView::beginDrag (int band)
{
    dragging = true;
    apvts.undoManager->beginNewTransaction ("Move band");
    for (auto p : { BandParam::freq, BandParam::gain })
        params[(size_t) band][(size_t) p]->beginChangeGesture();
}

void SpectrumView::endDrag()
{
    if (! dragging)
        return;

    dragging = false;
    for (auto p : { BandParam::freq, BandParam::gain })
        params[(size_t) selected][(size_t) p]->endChangeGesture();
    repaint();
}

void SpectrumView::mouseDown (const juce::MouseEvent& e)
{
    if (addButtonArea().contains (e.position))
    {
        // the first of a few spread-out frequencies that has no band close to it
        for (const float hz : { 1000.0f, 3000.0f, 300.0f, 6000.0f, 150.0f, 10000.0f, 600.0f, 2000.0f })
        {
            bool free = true;
            for (int b = 0; b < ParamIDs::numBands; ++b)
                free = free && (! isActive (b) || std::abs (std::log2 (getValue (b, BandParam::freq) / hz)) > 0.4f);
            if (free)
            {
                addBand (hz, 0.0f);
                return;
            }
        }
        addBand (1000.0f, 0.0f);
        return;
    }

    const auto hit = nodeAt (e.position);
    select (hit);
    repaint();

    if (hit < 0)
        return;

    if (e.mods.isPopupMenu())
        showMenu (hit);
    else
        beginDrag (hit);
}

void SpectrumView::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging || selected < 0)
        return;

    setValue (selected, BandParam::freq, juce::jlimit (20.0f, 20000.0f, hzForX (e.position.x)));
    if (vs::shapeUsesGain ((vs::BandShape) juce::roundToInt (getValue (selected, BandParam::shape))))
        setValue (selected, BandParam::gain, dbForY (e.position.y));
}

void SpectrumView::mouseUp (const juce::MouseEvent&)
{
    endDrag();
}

void SpectrumView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (addButtonArea().contains (e.position))
        return;

    const auto hit = nodeAt (e.position);
    if (hit >= 0)
    {
        apvts.undoManager->beginNewTransaction ("Bypass band");
        setValue (hit, BandParam::enabled, getValue (hit, BandParam::enabled) >= 0.5f ? 0.0f : 1.0f);
        return;
    }

    addBand (hzForX (e.position.x), dbForY (e.position.y));
}

void SpectrumView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto hit = nodeAt (e.position);
    if (hit < 0)
        return;

    setValue (hit, BandParam::q, getValue (hit, BandParam::q) * std::exp (wheel.deltaY * 0.8f));
    select (hit);
}

void SpectrumView::showMenu (int band)
{
    juce::PopupMenu menu;
    const auto current = juce::roundToInt (getValue (band, BandParam::shape));
    for (int i = 0; i < vs::numBandShapes; ++i)
        menu.addItem (i + 1, shapeNames[i], true, i == current);

    menu.addSeparator();
    const auto enabled = getValue (band, BandParam::enabled) >= 0.5f;
    menu.addItem (100, enabled ? "Bypass" : "Enable");
    menu.addItem (101, "Delete");

    juce::Component::SafePointer<SpectrumView> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options(), [safeThis, band] (int result) {
        if (safeThis == nullptr || result == 0)
            return;

        safeThis->apvts.undoManager->beginNewTransaction ("Edit band");
        if (result == 101)
        {
            safeThis->setValue (band, BandParam::active, 0.0f);
        }
        else if (result == 100)
        {
            safeThis->setValue (band, BandParam::enabled, safeThis->getValue (band, BandParam::enabled) >= 0.5f ? 0.0f : 1.0f);
        }
        else
        {
            const auto shape = (vs::BandShape) (result - 1);
            safeThis->setValue (band, BandParam::shape, (float) (int) shape);
            if (shape == vs::BandShape::lowCut || shape == vs::BandShape::highCut)
                safeThis->setValue (band, BandParam::q, 2.0f); // cuts want a steeper slope than a bell's Q
        }
    });
}
} // namespace vox
