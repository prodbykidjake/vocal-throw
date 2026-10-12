#include "ReductionGraph.h"

namespace
{
constexpr float kMaxDisplayDb = 24.0f;
const auto kBackground = juce::Colour (0xff14161a);
const auto kGrid = juce::Colour (0xff2a2e35);
const auto kAccent = juce::Colour (0xff4fd1c5);
} // namespace

ReductionGraph::ReductionGraph (PluginProcessor& p) : processor (p)
{
    setOpaque (true);
    startTimerHz (30);
}

void ReductionGraph::timerCallback()
{
    processor.getReductionDisplay (reduction.data());

    // fast rise, slower fall so short cuts remain visible
    for (size_t i = 0; i < shown.size(); ++i)
        shown[i] = reduction[i] > shown[i] ? reduction[i] : shown[i] + 0.3f * (reduction[i] - shown[i]);

    repaint();
}

void ReductionGraph::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);
    const auto area = getLocalBounds().toFloat().reduced (1.0f);

    auto xForHz = [&] (float hz) {
        return area.getX() + area.getWidth() * std::log (hz / 20.0f) / std::log (1000.0f);
    };
    auto yForDb = [&] (float db) { return area.getY() + area.getHeight() * juce::jmin (db, kMaxDisplayDb) / kMaxDisplayDb; };

    g.setFont (11.0f);
    g.setColour (kGrid);
    for (const float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        g.drawVerticalLine ((int) xForHz (hz), area.getY(), area.getBottom());
    for (float db = 6.0f; db < kMaxDisplayDb; db += 6.0f)
        g.drawHorizontalLine ((int) yForDb (db), area.getX(), area.getRight());

    g.setColour (juce::Colours::grey);
    for (const float hz : { 100.0f, 1000.0f, 10000.0f })
        g.drawText (hz >= 1000.0f ? juce::String ((int) (hz / 1000)) + "k" : juce::String ((int) hz),
            juce::Rectangle<float> (xForHz (hz) + 2.0f, area.getBottom() - 14.0f, 40.0f, 12.0f).toNearestInt(),
            juce::Justification::left, false);

    juce::Path curve;
    curve.startNewSubPath (area.getX(), area.getY());
    for (size_t i = 0; i < shown.size(); ++i)
        curve.lineTo (xForHz (vs::ResonanceSuppressor::displayPointFrequency ((int) i)), yForDb (shown[i]));
    curve.lineTo (area.getRight(), area.getY());
    curve.closeSubPath();

    g.setColour (kAccent.withAlpha (0.35f));
    g.fillPath (curve);
    g.setColour (kAccent);
    g.strokePath (curve, juce::PathStrokeType (1.5f));

    g.setColour (kGrid);
    g.drawRect (getLocalBounds(), 1);
}
