#pragma once

#include "Widgets.h"
#include <array>

// Input analysis column: scope, L/R meters, peak/RMS/crest/width and a plain-language level note.
// Every number comes from the live meters; nothing is decorative.
class AnalysisPanel : public juce::Component
{
public:
    static constexpr int numBars = 62;

    struct Live
    {
        double sampleRate = 48000.0;
        std::array<float, numBars> wave {}; // 0..1 per bar (dB-mapped peak)
        float fillL = 0, fillR = 0, tickL = 0, tickR = 0;
        juce::String textL = "--", textR = "--";
        juce::String peak = "--", rms = "--", crest = "--", width = "--";
        juce::String noteTitle = "No signal", noteBody = "Play audio through the plugin to see its level and balance.";
        bool warning = false;
    };

    void setLive (Live l)
    {
        live = std::move (l);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const int w = getWidth(), pad = 24;
        const int inner = w - 2 * pad;
        Theme::eyebrow (g, "INPUT", { pad, 24, 100, 12 });
        g.setColour (Theme::textBright);
        g.setFont (Theme::sans (15.0f, true));
        g.drawText ("Signal analysis", pad, 38, 240, 22, juce::Justification::centredLeft, false);
        g.setColour (juce::Colour (0xff718083));
        g.setFont (Theme::mono (9.0f, 0.08f));
        g.drawText (juce::String (live.sampleRate / 1000.0, juce::approximatelyEqual (live.sampleRate, 44100.0) ? 1 : 0) + " kHz", w - pad - 90, 41, 90, 14, juce::Justification::centredRight, false);

        // scope
        auto scope = juce::Rectangle<float> ((float) pad, 79.0f, (float) inner, 105.0f);
        Theme::card_ (g, scope, Theme::well, juce::Colour (0xff293234), 6.0f);
        g.setColour (juce::Colours::white.withAlpha (0.018f));
        for (int i = 1; i < 4; ++i)
            g.drawHorizontalLine ((int) (scope.getY() + scope.getHeight() * (float) i / 4.0f), scope.getX() + 1, scope.getRight() - 1);
        const auto bars = scope.reduced (8.0f, 10.0f);
        const float gap = 2.0f, bw = (bars.getWidth() - gap * (numBars - 1)) / (float) numBars;
        for (int i = 0; i < numBars; ++i)
        {
            const auto h = bars.getHeight() * (0.10f + 0.70f * live.wave[(size_t) i]);
            g.setColour (juce::Colour (0xff4bd3b0).withAlpha (0.68f));
            g.fillRoundedRectangle (bars.getX() + (float) i * (bw + gap), bars.getCentreY() - h * 0.5f, std::max (1.0f, bw), h, 1.5f);
        }
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawHorizontalLine ((int) scope.getCentreY(), scope.getX() + 1, scope.getRight() - 1);

        // L / R meters
        drawMeter (g, "L", live.fillL, live.tickL, live.textL, pad, 200, inner);
        drawMeter (g, "R", live.fillR, live.tickR, live.textR, pad, 219, inner);
        g.setColour (juce::Colour (0xff242c2e));
        g.drawHorizontalLine (248, (float) pad, (float) (w - pad));

        // stats
        const char* labels[4] = { "PEAK", "RMS", "CREST", "WIDTH" };
        const char* units[4] = { "dBFS", "dBFS", "dB", "%" };
        const juce::String values[4] = { live.peak, live.rms, live.crest, live.width };
        const auto colW = (float) inner / 4.0f;
        for (int i = 0; i < 4; ++i)
        {
            const auto x = (float) pad + colW * (float) i;
            g.setColour (juce::Colour (0xff637074));
            g.setFont (Theme::mono (8.5f, 0.08f));
            g.drawText (labels[i], (int) x, 266, (int) colW, 12, juce::Justification::centred, false);
            g.setColour (Theme::text.withAlpha (0.95f));
            g.setFont (Theme::mono (14.0f));
            g.drawText (values[i], (int) x, 281, (int) colW, 20, juce::Justification::centred, false);
            g.setColour (juce::Colour (0xff637074));
            g.setFont (Theme::mono (8.5f));
            g.drawText (units[i], (int) x, 303, (int) colW, 12, juce::Justification::centred, false);
            if (i < 3)
            {
                g.setColour (juce::Colour (0xff273033));
                g.drawVerticalLine ((int) (x + colW), 266.0f, 314.0f);
            }
        }

        // note
        auto note = juce::Rectangle<float> ((float) pad, 336.0f, (float) inner, 68.0f);
        Theme::card_ (g, note, juce::Colour (0xff131a1b), live.warning ? Theme::coral.withAlpha (0.35f) : juce::Colour (0xff273235), 6.0f);
        const auto tone = live.warning ? Theme::coral : Theme::accent;
        const auto icon = juce::Rectangle<float> (17.0f, 17.0f).withPosition (note.getX() + 11.0f, note.getY() + 12.0f);
        g.setColour (tone.withAlpha (0.55f));
        g.drawEllipse (icon, 1.0f);
        g.setColour (tone);
        g.setFont (Theme::mono (9.5f));
        g.drawText (live.warning ? "!" : "i", icon.toNearestInt(), juce::Justification::centred, false);
        auto textArea = note.toNearestInt().withTrimmedLeft (39).withTrimmedRight (11).withTrimmedTop (10).withTrimmedBottom (8);
        g.setColour (juce::Colour (0xffb9c5c7));
        g.setFont (Theme::sans (11.0f, true));
        g.drawText (live.noteTitle, textArea.removeFromTop (16), juce::Justification::topLeft, false);
        g.setColour (juce::Colour (0xff788588));
        g.setFont (Theme::sans (10.0f));
        g.drawFittedText (live.noteBody, textArea, juce::Justification::topLeft, 3, 1.0f);
    }

private:
    static void drawMeter (juce::Graphics& g, const char* label, float fill, float tick, const juce::String& text, int x, int y, int inner)
    {
        g.setColour (juce::Colour (0xff758184));
        g.setFont (Theme::mono (9.0f));
        g.drawText (label, x, y, 12, 12, juce::Justification::centredLeft, false);
        const float tx = (float) x + 20.0f, tw = (float) inner - 20.0f - 38.0f;
        const auto track = juce::Rectangle<float> (tx, (float) y + 4.0f, tw, 4.0f);
        g.setColour (juce::Colour (0xff273033));
        g.fillRoundedRectangle (track, 2.0f);
        const auto f = juce::jlimit (0.0f, 1.0f, fill);
        if (f > 0.0f)
        {
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff39d3af), track.getX(), 0.0f, juce::Colour (0xfff5c95e), track.getRight(), 0.0f, false));
            g.fillRoundedRectangle (track.withWidth (tw * f), 2.0f);
        }
        g.setColour (juce::Colour (0xfff5d275));
        g.fillRect (track.getX() + tw * juce::jlimit (0.0f, 0.99f, tick), (float) y + 2.0f, 1.0f, 8.0f);
        g.setColour (juce::Colour (0xff758184));
        g.drawText (text, x + inner - 36, y, 36, 12, juce::Justification::centredRight, false);
    }

    Live live;
};
