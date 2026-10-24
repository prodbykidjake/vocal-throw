// Offline "audio lab": runs realistic test material through the real plugin for every preset and goal and reports
// objective metrics (loudness change, peak change, DC, clicks) plus spectrograms. Hidden by default:
//   SIGNALPATH_LAB_DIR=/some/dir ./Tests "[.lab]"
#include "../helpers/dsp_test_utils.h"
#include "../helpers/material.h"
#include <PluginProcessor.h>
#include <catch2/catch_test_macros.hpp>
#include <dsp/Biquad.h>
#include <dsp/Meters.h>
#include <engine/PresetManager.h>
#include <engine/RecommendationSession.h>
#include <iomanip>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <iostream>

namespace
{
constexpr double fs = 48000.0;
constexpr int block = 480;

struct Material
{
    juce::String name;
    Source source;
    juce::AudioBuffer<float> audio;
};

struct Metrics
{
    double lufsDelta = 0, peakDelta = 0, crestDelta = 0, dc = 0, maxStep = 0;
    bool nonFinite = false;
};

double lufsOf (const juce::AudioBuffer<float>& b)
{
    LoudnessMeter m;
    m.prepare (fs);
    double sum = 0;
    int count = 0;
    for (int s = 0; s + block <= b.getNumSamples(); s += block)
    {
        juce::AudioBuffer<float> view (const_cast<float* const*> (b.getArrayOfReadPointers()), 2, s, block);
        m.process (view, 2);
        if (s > b.getNumSamples() / 2)
        {
            sum += m.getLufs();
            ++count;
        }
    }
    return count ? sum / count : -120.0;
}

double peakDbOf (const juce::AudioBuffer<float>& b) { return juce::Decibels::gainToDecibels (b.getMagnitude (0, b.getNumSamples())); }

juce::AudioBuffer<float> render (PluginProcessor& p, const juce::AudioBuffer<float>& in)
{
    auto out = in;
    juce::MidiBuffer midi;
    for (int s = 0; s + block <= in.getNumSamples(); s += block)
    {
        juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, s, block);
        p.processBlock (view, midi);
    }
    return out;
}

Metrics measure (const juce::AudioBuffer<float>& dry, const juce::AudioBuffer<float>& wet)
{
    Metrics m;
    const int skip = (int) fs; // ignore the first second (smoothing settling)
    m.lufsDelta = lufsOf (wet) - lufsOf (dry);
    m.peakDelta = peakDbOf (wet) - peakDbOf (dry);
    auto crest = [&] (const juce::AudioBuffer<float>& b) { return peakDbOf (b) - test::rmsDb (b, 0, skip, b.getNumSamples() - skip); };
    m.crestDelta = crest (wet) - crest (dry);
    double dc = 0;
    for (int i = skip; i < wet.getNumSamples(); ++i)
    {
        const auto v = wet.getSample (0, i);
        dc += v;
        m.nonFinite |= ! std::isfinite (v);
        if (i > skip)
            m.maxStep = std::max (m.maxStep, (double) std::abs (v - wet.getSample (0, i - 1)) - (double) std::abs (dry.getSample (0, i) - dry.getSample (0, i - 1)) * 4.0);
    }
    m.dc = dc / (wet.getNumSamples() - skip);
    return m;
}

void writeSpectrogram (const juce::AudioBuffer<float>& dry, const juce::AudioBuffer<float>& wet, const juce::File& file)
{
    constexpr int order = 11, size = 1 << order, hop = 1024, rows = 200;
    juce::dsp::FFT fft (order);
    auto panel = [&] (const juce::AudioBuffer<float>& b, juce::Image& img, int y0, int frames, bool diffMode, const juce::AudioBuffer<float>* ref) {
        std::vector<float> data ((size_t) size * 2), refData ((size_t) size * 2);
        for (int f = 0; f < frames; ++f)
        {
            const int start = f * hop;
            if (start + size > b.getNumSamples()) break;
            for (int i = 0; i < size; ++i)
            {
                const float w = 0.5f - 0.5f * std::cos (2.0f * std::numbers::pi_v<float> * (float) i / size);
                data[(size_t) i] = b.getSample (0, start + i) * w;
                if (ref) refData[(size_t) i] = ref->getSample (0, start + i) * w;
            }
            std::fill (data.begin() + size, data.end(), 0.0f);
            fft.performFrequencyOnlyForwardTransform (data.data());
            if (ref)
            {
                std::fill (refData.begin() + size, refData.end(), 0.0f);
                fft.performFrequencyOnlyForwardTransform (refData.data());
            }
            for (int r = 0; r < rows; ++r)
            {
                const double hz = 40.0 * std::pow (20000.0 / 40.0, (double) r / (rows - 1));
                const int bin = juce::jlimit (1, size / 2 - 1, (int) std::lround (hz / fs * size));
                const float db = 20.0f * std::log10 (data[(size_t) bin] / size * 4.0f + 1.0e-9f);
                if (! diffMode)
                {
                    const float t = juce::jlimit (0.0f, 1.0f, (db + 100.0f) / 70.0f);
                    img.setPixelAt (f, y0 + rows - 1 - r, juce::Colour::fromHSV (0.66f - 0.66f * t, 0.9f, t * 0.9f + 0.05f, 1.0f));
                }
                else
                {
                    const float rdb = 20.0f * std::log10 (refData[(size_t) bin] / size * 4.0f + 1.0e-9f);
                    const float d = (db < -70.0f && rdb < -70.0f) ? 0.0f : juce::jlimit (-12.0f, 12.0f, db - rdb) / 12.0f; // blue = quieter, red = louder; ignore bins that are both near-silent
                    img.setPixelAt (f, y0 + rows - 1 - r, d >= 0 ? juce::Colour::fromFloatRGBA (0.1f + 0.9f * d, 0.1f, 0.1f, 1.0f) : juce::Colour::fromFloatRGBA (0.1f, 0.1f, 0.1f - 0.9f * d, 1.0f));
                }
            }
        }
    };
    const int frames = std::min (600, (dry.getNumSamples() - size) / hop);
    juce::Image img (juce::Image::RGB, frames, rows * 3 + 4, true);
    img.clear (img.getBounds(), juce::Colours::black);
    panel (dry, img, 0, frames, false, nullptr);
    panel (wet, img, rows + 2, frames, false, nullptr);
    panel (wet, img, 2 * (rows + 2), frames, true, &dry);
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat().writeImageToStream (img, out);
}

void writeWav (const juce::AudioBuffer<float>& b, const juce::File& file)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (fs).withNumChannels (2).withBitsPerSample (16));
    if (writer != nullptr)
        writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

std::unique_ptr<PluginProcessor> fresh()
{
    auto p = std::make_unique<PluginProcessor>();
    p->setPlayConfigDetails (2, 2, fs, block);
    p->prepareToPlay (fs, block);
    return p;
}
}

TEST_CASE ("audio lab", "[.lab]")
{
    const auto dirEnv = juce::SystemStats::getEnvironmentVariable ("SIGNALPATH_LAB_DIR", {});
    juce::File outDir (dirEnv.isNotEmpty() ? dirEnv : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("signalpath_lab").getFullPathName());
    outDir.createDirectory();

    std::vector<Material> materials;
    materials.push_back ({ "voice_quiet", Source::vocal, test::material::voice (12, 1, -26.0f) });
    materials.push_back ({ "voice_normal", Source::vocal, test::material::voice (12, 2, -18.0f) });
    materials.push_back ({ "voice_hot", Source::vocal, test::material::voice (12, 3, -11.0f) });
    materials.push_back ({ "drums", Source::drums, test::material::drums (12, 4, -17.0f) });

    std::cout << "\n=== PRESETS (loudness / peak / crest change, DC, max click step) ===\n";
    for (auto& m : materials)
        for (const auto& [name, values] : PresetManager::factoryPresets())
        {
            auto p = fresh();
            PresetManager presets (*p, outDir.getChildFile ("presets"));
            presets.load (name);
            auto wet = render (*p, m.audio);
            const auto r = measure (m.audio, wet);
            std::cout << std::left << std::setw(14) << m.name.toStdString() << std::setw(28) << name.toStdString() << std::fixed << std::setprecision(2)
                      << " dLUFS " << std::setw(6) << r.lufsDelta << " dPeak " << std::setw(6) << r.peakDelta << " dCrest " << std::setw(6) << r.crestDelta
                      << " DC " << std::setprecision(5) << r.dc << " step " << r.maxStep << (r.nonFinite ? " NONFINITE" : "") << "\n";
        }

    std::cout << "\n=== GOALS (analyse -> apply the advisor's proposal), intensity 0.5 ===\n";
    for (auto& m : materials)
        for (int g = 0; g < numGoals; ++g)
        {
            auto p = fresh();
            RecommendationSession session (*p);
            render (*p, m.audio); // let the analyzer hear it
            session.analyse ((Goal) g, m.source, 0.5f);
            const auto& rec = session.getRecommendation();
            juce::AudioBuffer<float> wet;
            if (rec.outcome == Outcome::suggested)
            {
                session.apply();
                wet = render (*p, m.audio);
            }
            std::cout << std::left << std::setw(14) << m.name.toStdString() << std::setw(20) << goalName ((Goal) g);
            if (rec.outcome != Outcome::suggested)
            {
                std::cout << (rec.outcome == Outcome::noChangeNeeded ? "no change" : "insufficient signal") << "\n";
                continue;
            }
            const auto r = measure (m.audio, wet);
            std::cout << std::fixed << std::setprecision(2) << "dLUFS " << std::setw(6) << r.lufsDelta << " dPeak " << std::setw(6) << r.peakDelta << " dCrest " << std::setw(6) << r.crestDelta << "  ";
            for (const auto& c : rec.changes)
                std::cout << "[" << c.title << ": " << c.detail << "] ";
            std::cout << "\n";
            const auto base = m.name + "_" + juce::String (goalName ((Goal) g)).replace (" ", "");
            writeSpectrogram (m.audio, wet, outDir.getChildFile (base + ".png"));
        }
    for (auto& m : materials)
        writeWav (m.audio, outDir.getChildFile (m.name + "_dry.wav"));
}
