#pragma once

#include "../PluginProcessor.h"

/** Live gain-reduction display: frequency (log) on x, reduction in dB (downwards) on y. */
class ReductionGraph : public juce::Component, private juce::Timer
{
public:
    explicit ReductionGraph (PluginProcessor& p);
    ~ReductionGraph() override = default;

    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override;

    PluginProcessor& processor;
    std::array<float, vs::ResonanceSuppressor::numDisplayPoints> reduction {};
    std::array<float, vs::ResonanceSuppressor::numDisplayPoints> shown {};
};
