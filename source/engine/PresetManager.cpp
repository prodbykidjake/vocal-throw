#include "PresetManager.h"

PresetManager::PresetManager (PluginProcessor& p, juce::File userDirectory)
    : processor (p), directory (std::move (userDirectory))
{
}

juce::File PresetManager::defaultUserDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SignalPath").getChildFile ("Presets");
}

juce::String PresetManager::sanitise (const juce::String& name)
{
    return name.retainCharacters (juce::String::fromUTF8 ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.·()")).trim().substring (0, 64);
}

std::vector<std::pair<juce::String, std::map<std::string, float>>> PresetManager::factoryPresets()
{
    using namespace ParamIDs;
    return {
        { "Init", {} }, // everything at its default
        { juce::String::fromUTF8 ("Vocal · Starting Point"),
          { { eqHpEnable, 1.0f }, { eqHpFreq, 90.0f },
            { eqGain[1], -1.5f }, { eqFreq[1], 300.0f }, { eqQ[1], 1.0f },
            { eqGain[2], 1.5f }, { eqFreq[2], 3500.0f }, { eqQ[2], 0.9f },
            { compEnable, 1.0f }, { compThreshold, -22.0f }, { compRatio, 3.0f }, { compAttack, 12.0f }, { compRelease, 140.0f }, { compKnee, 8.0f },
            { satEnable, 1.0f }, { satDrive, 3.0f }, { satMix, 30.0f } } },
        { juce::String::fromUTF8 ("Drum Bus · Punch"),
          { { eqGain[0], 1.5f }, { eqFreq[0], 90.0f },
            { eqGain[3], 1.0f }, { eqFreq[3], 9000.0f },
            { compEnable, 1.0f }, { compThreshold, -18.0f }, { compRatio, 3.0f }, { compAttack, 28.0f }, { compRelease, 110.0f }, { compKnee, 3.0f },
            { satEnable, 1.0f }, { satDrive, 4.0f }, { satMix, 35.0f } } },
        { juce::String::fromUTF8 ("Bass · Weight"),
          { { eqHpEnable, 1.0f }, { eqHpFreq, 30.0f },
            { eqGain[0], 2.0f }, { eqFreq[0], 110.0f }, { eqGain[1], -1.5f }, { eqFreq[1], 280.0f },
            { compEnable, 1.0f }, { compThreshold, -20.0f }, { compRatio, 3.5f }, { compAttack, 20.0f }, { compRelease, 160.0f },
            { satEnable, 1.0f }, { satDrive, 6.0f }, { satMix, 40.0f } } },
        { "Smooth Top",
          { { eqGain[2], -1.5f }, { eqFreq[2], 4500.0f }, { eqQ[2], 1.2f },
            { eqGain[3], -1.0f }, { eqFreq[3], 9000.0f },
            { compEnable, 1.0f }, { compThreshold, -24.0f }, { compRatio, 2.0f }, { compAttack, 20.0f }, { compRelease, 200.0f }, { compKnee, 12.0f } } },
    };
}

std::vector<PresetManager::Preset> PresetManager::list() const
{
    std::vector<Preset> out;
    for (const auto& [name, values] : factoryPresets())
        out.push_back ({ name, true });
    for (const auto& f : directory.findChildFiles (juce::File::findFiles, false, "*.spreset"))
        out.push_back ({ f.getFileNameWithoutExtension(), false });
    std::sort (out.begin() + (long) factoryPresets().size(), out.end(), [] (const Preset& a, const Preset& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return out;
}

void PresetManager::apply (const std::map<std::string, float>& overrides, const juce::String& name)
{
    // Start from the defaults of every preset-able parameter so a preset fully defines the sound.
    std::map<std::string, float> values;
    for (const auto& [id, current] : processor.getParamValues())
        if (auto* p = processor.apvts.getParameter (id))
            values[id] = p->convertFrom0to1 (p->getDefaultValue());
    // Gains stay as the user has them: a preset is about the sound, not the level staging
    values[ParamIDs::inputGain] = processor.getParamValues().at (ParamIDs::inputGain);
    values[ParamIDs::outputGain] = processor.getParamValues().at (ParamIDs::outputGain);
    for (const auto& [id, v] : overrides)
        if (values.count (id) > 0)
            values[id] = v;
    processor.setParamValues (values, "Load preset " + name);
    processor.apvts.state.setProperty (PluginProcessor::presetName_, name, nullptr);
}

bool PresetManager::load (const juce::String& name)
{
    for (const auto& [factoryName, values] : factoryPresets())
        if (factoryName == name)
        {
            apply (values, name);
            return true;
        }

    const auto file = fileFor (name);
    if (! file.existsAsFile())
        return false;
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("SignalPathPreset"))
        return false;

    std::map<std::string, float> values;
    for (auto* e : xml->getChildWithTagNameIterator ("Param"))
        values[e->getStringAttribute ("id").toStdString()] = (float) e->getDoubleAttribute ("value");
    apply (values, name);
    return true;
}

bool PresetManager::save (const juce::String& name)
{
    const auto clean = sanitise (name);
    if (clean.isEmpty())
        return false;
    for (const auto& [factoryName, v] : factoryPresets())
        if (factoryName.equalsIgnoreCase (clean))
            return false; // never shadow a factory preset

    if (! directory.createDirectory().wasOk())
        return false;
    juce::XmlElement xml ("SignalPathPreset");
    xml.setAttribute ("version", 1);
    xml.setAttribute ("name", clean);
    for (const auto& [id, v] : processor.getParamValues())
    {
        auto* e = xml.createNewChildElement ("Param");
        e->setAttribute ("id", juce::String (id));
        e->setAttribute ("value", (double) v);
    }
    if (! xml.writeTo (fileFor (clean)))
        return false;
    processor.apvts.state.setProperty (PluginProcessor::presetName_, clean, nullptr);
    return true;
}

bool PresetManager::remove (const juce::String& name)
{
    const auto file = fileFor (name);
    return file.existsAsFile() && file.deleteFile();
}
