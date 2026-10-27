#include "DesignTokens.h"
#include "BinaryData.h"

namespace
{
    // Owns the embedded typefaces. JUCE deletes it at shutdown, so nothing outlives the message manager.
    class FontCache : private juce::DeletedAtShutdown
    {
    public:
        FontCache()
            : medium (juce::Typeface::createSystemTypefaceFor (BinaryData::SpaceGroteskMedium_ttf, static_cast<size_t> (BinaryData::SpaceGroteskMedium_ttfSize))),
              bold (juce::Typeface::createSystemTypefaceFor (BinaryData::SpaceGroteskBold_ttf, static_cast<size_t> (BinaryData::SpaceGroteskBold_ttfSize)))
        {
        }

        ~FontCache() override { clearSingletonInstance(); }

        juce::Typeface::Ptr medium, bold;

        JUCE_DECLARE_SINGLETON (FontCache, false)
    };

    JUCE_IMPLEMENT_SINGLETON (FontCache)
}

namespace Design
{
    juce::Font medium (float heightInPixels)
    {
        return juce::Font (juce::FontOptions (FontCache::getInstance()->medium).withHeight (heightInPixels));
    }

    juce::Font bold (float heightInPixels)
    {
        return juce::Font (juce::FontOptions (FontCache::getInstance()->bold).withHeight (heightInPixels));
    }

    juce::Font mono (float heightInPixels, bool isBold)
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), heightInPixels, isBold ? juce::Font::bold : juce::Font::plain)
                               .withKerningFactor (0.08f));
    }
}
