#pragma once

#include "../PluginProcessor.h"

// Factory presets (in code) and user presets (XML files). A preset is a set of parameter values;
// bypass and loudness-match are never part of a preset. Loading is one undoable step.
class PresetManager
{
public:
    struct Preset
    {
        juce::String name;
        bool factory;
    };

    PresetManager (PluginProcessor& p, juce::File userDirectory = defaultUserDirectory());

    static juce::File defaultUserDirectory();
    static std::vector<std::pair<juce::String, std::map<std::string, float>>> factoryPresets();

    std::vector<Preset> list() const;
    bool load (const juce::String& name);
    bool save (const juce::String& name); // user presets only; returns false for invalid names
    bool remove (const juce::String& name);

    static juce::String sanitise (const juce::String& name);

private:
    juce::File fileFor (const juce::String& name) const { return directory.getChildFile (sanitise (name) + ".spreset"); }
    void apply (const std::map<std::string, float>& overrides, const juce::String& name);

    PluginProcessor& processor;
    juce::File directory;
};
