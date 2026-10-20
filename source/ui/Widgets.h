#pragma once

#include "Theme.h"
#include "../PluginProcessor.h"

// Text/icon button used everywhere in the design: outline, primary (mint fill), light (apply), ghost.
class DesignButton : public juce::Button
{
public:
    enum class Style { outline, primary, light, ghost, segment };

    explicit DesignButton (const juce::String& label = {}, Style s = Style::outline) : juce::Button (label), style (s) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    void setStyle (Style s) { style = s; repaint(); }
    void setTextFont (juce::Font f) { font = std::move (f); }
    void setAccentText (bool b) { accentText = b; repaint(); }
    void setTint (juce::Colour c) { tint = c; hasTint = true; repaint(); }

    std::function<void (juce::Graphics&, juce::Rectangle<float>, juce::Colour)> icon; // optional glyph drawn beside the text
    bool iconOnRight = false;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat();
        const bool on = getToggleState();
        const bool en = isEnabled();
        juce::Colour textColour = Theme::text;

        switch (style)
        {
            case Style::outline:
                Theme::card_ (g, r, over ? juce::Colour (0xff181f20) : Theme::topbar.darker (0.2f), hasTint && on ? tint.withAlpha (0.4f) : Theme::borderStrong, 6.0f);
                textColour = hasTint && on ? tint : (accentText ? Theme::accent : Theme::textMid);
                break;
            case Style::primary:
                g.setColour (Theme::accent.withMultipliedBrightness (down ? 0.9f : 1.0f).withAlpha (en ? 1.0f : 0.45f));
                g.fillRoundedRectangle (r, 6.0f);
                textColour = Theme::accentDark;
                break;
            case Style::light:
                g.setColour ((on ? Theme::accent : juce::Colour (0xffe7efef)).withAlpha (en ? 1.0f : 0.55f));
                g.fillRoundedRectangle (r, 5.0f);
                textColour = juce::Colour (0xff101617);
                break;
            case Style::ghost:
                textColour = over ? Theme::text : Theme::textDim;
                break;
            case Style::segment:
                if (on)
                {
                    g.setColour (Theme::accent);
                    g.fillRoundedRectangle (r, 5.0f);
                    textColour = juce::Colour (0xff0a1513);
                }
                else
                    textColour = over ? Theme::text : juce::Colour (0xff717d80);
                break;
        }
        if (! en)
            textColour = textColour.withAlpha (0.5f);

        g.setColour (textColour);
        g.setFont (font);
        auto textArea = getLocalBounds().toFloat();
        if (icon)
        {
            auto iconArea = iconOnRight ? textArea.removeFromRight (textArea.getHeight() * 0.9f).withTrimmedRight (4.0f).reduced (textArea.getHeight() * 0.22f)
                                        : textArea.removeFromLeft (textArea.getHeight() * 0.9f).withTrimmedLeft (4.0f).reduced (textArea.getHeight() * 0.22f);
            icon (g, iconArea, textColour);
        }
        g.drawText (getButtonText(), textArea, juce::Justification::centred, false);
    }

private:
    Style style;
    juce::Font font = Theme::sans (11.0f, true);
    bool accentText = false, hasTint = false;
    juce::Colour tint;
};

// Power glyph (the design's PowerIcon), drawn into `r`.
inline void drawPowerGlyph (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    const auto s = std::min (r.getWidth(), r.getHeight());
    auto b = juce::Rectangle<float> (s, s).withCentre (r.getCentre());
    juce::Path arc;
    arc.addCentredArc (b.getCentreX(), b.getCentreY() + s * 0.06f, s * 0.36f, s * 0.36f, 0.0f, juce::degreesToRadians (40.0f), juce::degreesToRadians (320.0f), true);
    g.setColour (c);
    g.strokePath (arc, juce::PathStrokeType (std::max (1.4f, s * 0.12f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.drawLine (b.getCentreX(), b.getY() + s * 0.05f, b.getCentreX(), b.getCentreY() - s * 0.02f, std::max (1.4f, s * 0.12f));
}

inline void drawChevron (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, bool up = false)
{
    juce::Path p;
    const auto cx = r.getCentreX(), cy = r.getCentreY();
    const auto dy = up ? 1.5f : -1.5f;
    p.startNewSubPath (cx - 3.5f, cy + dy);
    p.lineTo (cx, cy - dy);
    p.lineTo (cx + 3.5f, cy + dy);
    g.setColour (c);
    g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// Rotary knob with a mint value arc, matching the design's Knob.
class KnobLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float startAngle, float endAngle, juce::Slider& s) override
    {
        auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
        const auto d = std::min (r.getWidth(), r.getHeight());
        r = r.withSizeKeepingCentre (d, d);
        const auto c = r.getCentre();
        const bool en = s.isEnabled();

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff252d2f), c, juce::Colour (0xff111617), c.translated (d * 0.7f, 0), true));
        g.fillEllipse (r.reduced (3.0f));
        g.setColour (juce::Colour (0xff313c3e));
        g.drawEllipse (r.reduced (3.0f), 2.0f);

        juce::Path track, value;
        const auto arcR = d * 0.5f - 0.5f;
        track.addCentredArc (c.x, c.y, arcR, arcR, 0, startAngle, endAngle, true);
        g.setColour (juce::Colour (0xff1d2527));
        g.strokePath (track, juce::PathStrokeType (2.5f));
        const auto angle = startAngle + pos * (endAngle - startAngle);
        value.addCentredArc (c.x, c.y, arcR, arcR, 0, startAngle, angle, true);
        g.setColour (Theme::accent.withAlpha (en ? 1.0f : 0.3f));
        g.strokePath (value, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path tick;
        tick.addRoundedRectangle (-0.7f, -d * 0.5f + 5.0f, 1.4f, d * 0.2f, 0.7f);
        g.setColour (juce::Colour (0xffd8e3e3).withAlpha (en ? 1.0f : 0.4f));
        g.fillPath (tick, juce::AffineTransform::rotation (angle).translated (c));
    }
};

// A knob bound to one parameter, with its value, unit and name under it.
class Knob : public juce::Component
{
public:
    Knob()
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        slider.setLookAndFeel (&lnf);
        slider.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
        slider.setPopupDisplayEnabled (false, false, nullptr);
        slider.onValueChange = [this] { repaint(); };
        addAndMakeVisible (slider);
    }
    ~Knob() override { slider.setLookAndFeel (nullptr); }

    void bind (juce::AudioProcessorValueTreeState& apvts, const juce::String& id, const juce::String& labelText, const juce::String& unitText, int decimals = 1)
    {
        attachment.reset();
        label = labelText;
        unit = unitText;
        places = decimals;
        slider.setNumDecimalPlacesToDisplay (decimals);
        if (auto* p = apvts.getParameter (id))
            slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, id, slider);
        setVisible (true);
        repaint();
    }

    void unbind() { attachment.reset(); setVisible (false); }

    void resized() override { slider.setBounds (getLocalBounds().removeFromTop (46).withSizeKeepingCentre (46, 46)); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        r.removeFromTop (46);
        g.setColour (Theme::text);
        g.setFont (Theme::mono (10.5f));
        g.drawText (valueText(), r.removeFromTop (16), juce::Justification::centred, false);
        g.setColour (juce::Colour (0xff5e6a6d));
        g.setFont (Theme::mono (8.0f));
        g.drawText (unit, r.removeFromTop (11), juce::Justification::centred, false);
        g.setColour (Theme::textMuted.brighter (0.1f));
        g.setFont (Theme::mono (8.0f, 0.08f));
        g.drawText (label.toUpperCase(), r.removeFromTop (14), juce::Justification::centred, false);
    }

private:
    juce::String valueText() const
    {
        const auto v = (float) slider.getValue();
        return (v < 0 ? Theme::u8 ("−") : juce::String()) + juce::String (std::abs (v), places);
    }

    KnobLookAndFeel lnf;
    juce::Slider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    juce::String label, unit;
    int places = 1;
};
