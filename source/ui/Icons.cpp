#include "Icons.h"

namespace vox::icons
{
juce::Path power()
{
    juce::Path p;
    p.addCentredArc (0.5f, 0.56f, 0.36f, 0.36f, 0.0f, juce::degreesToRadians (35.0f), juce::degreesToRadians (325.0f), true);
    p.startNewSubPath (0.5f, 0.08f);
    p.lineTo (0.5f, 0.5f);
    return p;
}

juce::Path keyListen()
{
    // a small zigzag, like the design's key-listen glyph
    juce::Path p;
    p.startNewSubPath (0.12f, 0.5f);
    p.lineTo (0.32f, 0.5f);
    p.lineTo (0.42f, 0.18f);
    p.lineTo (0.58f, 0.82f);
    p.lineTo (0.68f, 0.5f);
    p.lineTo (0.88f, 0.5f);
    return p;
}

juce::Path lock (bool locked)
{
    juce::Path p;
    p.addRoundedRectangle (0.2f, 0.45f, 0.6f, 0.45f, 0.08f);
    if (locked)
    {
        p.startNewSubPath (0.3f, 0.45f);
        p.lineTo (0.3f, 0.3f);
        p.addCentredArc (0.5f, 0.3f, 0.2f, 0.2f, 0.0f, juce::degreesToRadians (-90.0f), juce::degreesToRadians (90.0f), false);
        p.lineTo (0.7f, 0.45f);
    }
    else
    {
        p.startNewSubPath (0.3f, 0.45f);
        p.lineTo (0.3f, 0.3f);
        p.addCentredArc (0.5f, 0.3f, 0.2f, 0.2f, 0.0f, juce::degreesToRadians (-90.0f), juce::degreesToRadians (90.0f), false);
    }
    return p;
}

juce::Path chevron (bool pointsRight)
{
    juce::Path p;
    if (pointsRight)
    {
        p.startNewSubPath (0.35f, 0.15f);
        p.lineTo (0.68f, 0.5f);
        p.lineTo (0.35f, 0.85f);
    }
    else
    {
        p.startNewSubPath (0.65f, 0.15f);
        p.lineTo (0.32f, 0.5f);
        p.lineTo (0.65f, 0.85f);
    }
    return p;
}

juce::Path plus()
{
    juce::Path p;
    p.startNewSubPath (0.5f, 0.15f);
    p.lineTo (0.5f, 0.85f);
    p.startNewSubPath (0.15f, 0.5f);
    p.lineTo (0.85f, 0.5f);
    return p;
}

juce::Path gear()
{
    juce::Path p;
    p.addStar ({ 0.5f, 0.5f }, 8, 0.36f, 0.48f, 0.0f);
    p.addEllipse (0.5f - 0.15f, 0.5f - 0.15f, 0.3f, 0.3f);
    p.setUsingNonZeroWinding (false); // even-odd: the centre is a hole
    return p;
}

juce::Path bandShape (vs::BandShape shape)
{
    using vs::BandShape;
    juce::Path p;
    switch (shape)
    {
        case BandShape::lowCut:
            p.startNewSubPath (0.0f, 0.85f);
            p.cubicTo (0.25f, 0.85f, 0.3f, 0.25f, 0.5f, 0.25f);
            p.lineTo (1.0f, 0.25f);
            break;
        case BandShape::lowShelf:
            p.startNewSubPath (0.0f, 0.22f);
            p.lineTo (0.25f, 0.22f);
            p.cubicTo (0.45f, 0.22f, 0.55f, 0.78f, 0.75f, 0.78f);
            p.lineTo (1.0f, 0.78f);
            break;
        case BandShape::highShelf:
            p.startNewSubPath (0.0f, 0.78f);
            p.lineTo (0.25f, 0.78f);
            p.cubicTo (0.45f, 0.78f, 0.55f, 0.22f, 0.75f, 0.22f);
            p.lineTo (1.0f, 0.22f);
            break;
        case BandShape::highCut:
            p.startNewSubPath (0.0f, 0.25f);
            p.lineTo (0.5f, 0.25f);
            p.cubicTo (0.7f, 0.25f, 0.75f, 0.85f, 1.0f, 0.85f);
            break;
        case BandShape::bell:
            p.startNewSubPath (0.0f, 0.75f);
            p.cubicTo (0.25f, 0.75f, 0.3f, 0.2f, 0.5f, 0.2f);
            p.cubicTo (0.7f, 0.2f, 0.75f, 0.75f, 1.0f, 0.75f);
            break;
        case BandShape::bandpass:
            p.startNewSubPath (0.0f, 0.85f);
            p.lineTo (0.2f, 0.85f);
            p.cubicTo (0.36f, 0.85f, 0.38f, 0.2f, 0.5f, 0.2f);
            p.cubicTo (0.62f, 0.2f, 0.64f, 0.85f, 0.8f, 0.85f);
            p.lineTo (1.0f, 0.85f);
            break;
        case BandShape::bandReject:
            p.startNewSubPath (0.0f, 0.2f);
            p.lineTo (0.2f, 0.2f);
            p.cubicTo (0.36f, 0.2f, 0.38f, 0.8f, 0.5f, 0.8f);
            p.cubicTo (0.62f, 0.8f, 0.64f, 0.2f, 0.8f, 0.2f);
            p.lineTo (1.0f, 0.2f);
            break;
        case BandShape::tilt:
            p.startNewSubPath (0.1f, 0.85f);
            p.lineTo (0.9f, 0.15f);
            break;
    }
    return p;
}

juce::Path wave()
{
    // the logo: M1 11 c5 0 5-8 10-8 s5 16 10 16 S26 3 31 3 s5 8 10 8
    juce::Path p;
    p.startNewSubPath (1.0f, 11.0f);
    p.cubicTo (6.0f, 11.0f, 6.0f, 3.0f, 11.0f, 3.0f);
    p.cubicTo (16.0f, 3.0f, 16.0f, 19.0f, 21.0f, 19.0f);
    p.cubicTo (26.0f, 19.0f, 26.0f, 3.0f, 31.0f, 3.0f);
    p.cubicTo (36.0f, 3.0f, 36.0f, 11.0f, 41.0f, 11.0f);
    p.applyTransform (juce::AffineTransform::scale (1.0f / 42.0f, 1.0f / 22.0f)); // into the unit box
    return p;
}

void stroke (juce::Graphics& g, const juce::Path& unitPath, juce::Rectangle<float> area, juce::Colour colour, float thickness)
{
    auto transform = juce::AffineTransform::fromTargetPoints (0.0f, 0.0f, area.getX(), area.getY(),
        1.0f, 0.0f, area.getRight(), area.getY(),
        0.0f, 1.0f, area.getX(), area.getBottom());
    g.setColour (colour);
    g.strokePath (unitPath, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), transform);
}
} // namespace vox::icons
