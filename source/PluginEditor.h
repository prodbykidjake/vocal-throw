#pragma once

#include "BinaryData.h"
#include "PluginProcessor.h"
#include "melatonin_inspector/melatonin_inspector.h"
#include "ui/EditorContent.h"
#include "ui/Theme.h"
#include "ui/VoxLookAndFeel.h"

//==============================================================================
/** The plugin window. All the UI lives in one 1480 x 860 "design pixel" component that this scales to fit
    the window, which can be resized with the aspect ratio locked. */
class PluginEditor : public juce::AudioProcessorEditor
{
public:
    explicit PluginEditor (PluginProcessor&);
    ~PluginEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

    vox::EditorContent& getContent() noexcept { return *content; }

   #if JUCE_DEBUG
    bool keyPressed (const juce::KeyPress&) override;
   #endif

private:
    void setScale (float scale);

    // This reference is provided as a quick way for your editor to
    // access the processor object that created it.
    PluginProcessor& processorRef;
    std::unique_ptr<melatonin::Inspector> inspector;

    vox::Theme theme;
    vox::VoxLookAndFeel lookAndFeel { theme }; // declared before the content so it outlives it
    std::unique_ptr<vox::EditorContent> content;
    juce::ComponentBoundsConstrainer constrainer;
    juce::TooltipWindow tooltips { this, 700 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
