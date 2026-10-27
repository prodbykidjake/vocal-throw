#include "PluginEditor.h"

namespace
{
constexpr float designWidth = (float) vox::layout::designWidth;
constexpr float designHeight = (float) vox::layout::designHeight;
constexpr float minScale = 0.7f;
constexpr float maxScale = 1.0f;
} // namespace

PluginEditor::PluginEditor (PluginProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    setLookAndFeel (&lookAndFeel);

    content = std::make_unique<vox::EditorContent> (processorRef, theme);
    content->onScaleRequested = [this] (float scale) { setScale (scale); };
    addAndMakeVisible (*content);

    constrainer.setFixedAspectRatio ((double) designWidth / (double) designHeight);
    constrainer.setSizeLimits (juce::roundToInt (designWidth * minScale), juce::roundToInt (designHeight * minScale),
        juce::roundToInt (designWidth * maxScale), juce::roundToInt (designHeight * maxScale));
    setConstrainer (&constrainer);
    setResizable (true, true);

    const auto scale = juce::jlimit (minScale, maxScale, processorRef.getUiScale());
    setSize (juce::roundToInt (designWidth * scale), juce::roundToInt (designHeight * scale));

   #if JUCE_DEBUG
    setWantsKeyboardFocus (true); // lets Ctrl/Cmd+Alt+I open the inspector while developing
   #endif
}

PluginEditor::~PluginEditor()
{
    setLookAndFeel (nullptr);
}

void PluginEditor::setScale (float scale)
{
    scale = juce::jlimit (minScale, maxScale, scale);
    setSize (juce::roundToInt (designWidth * scale), juce::roundToInt (designHeight * scale));
}

void PluginEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme.p().bgDeep);
}

void PluginEditor::resized()
{
    const auto scale = (float) getWidth() / designWidth;
    content->setBounds (0, 0, vox::layout::designWidth, vox::layout::designHeight);
    content->setTransform (juce::AffineTransform::scale (scale));
    processorRef.setUiScale (scale);
}

#if JUCE_DEBUG
bool PluginEditor::keyPressed (const juce::KeyPress& key)
{
    if (key.getModifiers().isCommandDown() && key.getModifiers().isAltDown() && key.getKeyCode() == 'I')
    {
        if (! inspector)
        {
            inspector = std::make_unique<melatonin::Inspector> (*this);
            inspector->onClose = [this]() { inspector.reset(); };
        }

        inspector->setVisible (true);
        return true;
    }

    return false;
}
#endif
