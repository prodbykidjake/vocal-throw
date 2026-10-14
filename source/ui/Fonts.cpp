#include "Fonts.h"
#include "BinaryData.h"

namespace vox::fonts
{
namespace
{
    juce::Typeface::Ptr load (const void* data, int size)
    {
        return juce::Typeface::createSystemTypefaceFor (data, (size_t) size);
    }

    juce::Typeface::Ptr typefaceFor (int weight)
    {
        static const auto regular = load (BinaryData::DMSansRegular_ttf, BinaryData::DMSansRegular_ttfSize);
        static const auto medium = load (BinaryData::DMSansMedium_ttf, BinaryData::DMSansMedium_ttfSize);
        static const auto semibold = load (BinaryData::DMSansSemiBold_ttf, BinaryData::DMSansSemiBold_ttfSize);
        static const auto bold = load (BinaryData::DMSansBold_ttf, BinaryData::DMSansBold_ttfSize);

        if (weight >= 650)
            return bold;
        if (weight >= 550)
            return semibold;
        if (weight >= 450)
            return medium;
        return regular;
    }
} // namespace

juce::Font make (int weight, float px, float tracking)
{
    juce::Font font (juce::FontOptions (typefaceFor (weight)).withPointHeight (px));
    if (! juce::approximatelyEqual (tracking, 0.0f))
    {
        // JUCE's kerning factor is a fraction of the JUCE height (ascent + descent), CSS letter-spacing is in em
        const auto ratio = font.getHeight() / juce::jmax (0.001f, font.getHeightInPoints());
        font = font.withExtraKerningFactor (tracking / ratio);
    }
    return font;
}

float width (const juce::Font& font, const juce::String& text)
{
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (font, text, 0.0f, 0.0f);
    return glyphs.getBoundingBox (0, -1, false).getWidth();
}

void draw (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> bounds, const juce::Font& font,
    juce::Colour colour, juce::Justification justification)
{
    g.setColour (colour);
    g.setFont (font);
    g.drawText (text, bounds, justification, false);
}
} // namespace vox::fonts
