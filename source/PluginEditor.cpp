#include "PluginEditor.h"
#include "ui/AnalysisPanel.h"
#include "ui/ChainPanel.h"
#include "ui/DirectionPanel.h"
#include "ui/RecommendationPanel.h"
#include "ui/TopBars.h"

//==============================================================================
class PluginEditor::Content : public juce::Component
{
public:
    Content (PluginProcessor& p, RecommendationSession& s, PresetManager& pm)
        : processor (p), session (s),
          header (p, pm, [this] { refreshAll(); }),
          modeBar ([this] (int m) { onModeChanged (m); }),
          direction (p.apvts.state, [this] { onSelectionChanged(); }, [this] { analyse(); }),
          recommendation (p, s, p.apvts.state, [this] { header.refresh(); chain.refreshLive(); }),
          chain (p, p.apvts.state, [this] { layoutPanels(); })
    {
        header.onSettingsChanged = [this] { recommendation.refreshLive(); };
        for (auto* c : std::initializer_list<juce::Component*> { &header, &modeBar, &analysis, &direction, &recommendation, &chain })
            addAndMakeVisible (c);
        const int mode = juce::jlimit (0, 2, (int) processor.apvts.state.getProperty (PluginProcessor::mode_, 0));
        modeBar.setMode (mode, false);
        recommendation.setMode (mode);
        setSize (Theme::designWidth, Theme::designHeight);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::window);
        // workspace column dividers
        g.setColour (juce::Colour (0xff293133));
        g.drawVerticalLine (analysis.getRight(), (float) analysis.getY(), (float) analysis.getBottom());
        g.drawVerticalLine (direction.getRight(), (float) direction.getY(), (float) direction.getBottom());
        g.drawHorizontalLine (chain.getY() - 1, 0.0f, (float) getWidth());

        auto foot = footerBounds();
        g.setColour (Theme::footer);
        g.fillRect (foot);
        g.setColour (Theme::borderSoft);
        g.drawHorizontalLine (foot.getY(), 0.0f, (float) getWidth());
        g.setColour (Theme::textFaint);
        g.setFont (Theme::mono (8.5f, 0.09f));
        g.drawText ("SIGNALPATH", foot.getX() + 20, foot.getY(), 90, foot.getHeight(), juce::Justification::centredLeft, false);
        g.setColour (juce::Colour (0xff596568));
        g.drawText (juce::String ("v") + VERSION, foot.getX() + 98, foot.getY(), 60, foot.getHeight(), juce::Justification::centredLeft, false);

        const auto load = processor.getCpuLoad();
        const float thresholds[4] = { 0.005f, 0.10f, 0.25f, 0.50f };
        const int cx = foot.getCentreX() - 40;
        g.setColour (Theme::textFaint);
        g.drawText ("CPU", cx - 30, foot.getY(), 28, foot.getHeight(), juce::Justification::centredRight, false);
        for (int i = 0; i < 4; ++i)
        {
            g.setColour (load >= thresholds[i] ? Theme::accent : juce::Colour (0xff253033));
            g.fillRect (cx + i * 5, foot.getCentreY() - 3, 2, 7);
        }
        g.setColour (juce::Colour (0xff657174));
        g.drawText (juce::String (load * 100.0f, 1) + "%", cx + 24, foot.getY(), 50, foot.getHeight(), juce::Justification::centredLeft, false);
        g.setColour (Theme::textFaint);
        g.drawText (Theme::u8 ("LOCAL PROCESSING · NO AUDIO LEAVES YOUR DEVICE"), foot.getRight() - 420, foot.getY(), 400, foot.getHeight(), juce::Justification::centredRight, false);
    }

    void resized() override { layoutPanels(); }

    void layoutPanels()
    {
        const int w = getWidth();
        header.setBounds (0, 0, w, 76);
        modeBar.setBounds (0, 76, w, 52);
        const int chainH = chain.preferredHeight();
        const int footerH = 28;
        const int workH = getHeight() - 128 - chainH - footerH;
        const double total = 0.86 + 1.14 + 1.2;
        const int w1 = (int) std::lround (w * 0.86 / total), w2 = (int) std::lround (w * 1.14 / total);
        analysis.setBounds (0, 128, w1, workH);
        direction.setBounds (w1, 128, w2, workH);
        recommendation.setBounds (w1 + w2, 128, w - w1 - w2, workH);
        chain.setBounds (0, 128 + workH, w, chainH);
        repaint();
    }

    juce::Rectangle<int> footerBounds() const { return { 0, getHeight() - 28, getWidth(), 28 }; }

    void refreshAll()
    {
        direction.refresh();
        chain.refreshLayout();
        recommendation.refresh();
        header.refresh();
        const int mode = juce::jlimit (0, 2, (int) processor.apvts.state.getProperty (PluginProcessor::mode_, 0));
        modeBar.setMode (mode, false);
        recommendation.setMode (mode);
        layoutPanels();
    }

    void onModeChanged (int m)
    {
        processor.apvts.state.setProperty (PluginProcessor::mode_, m, nullptr);
        if (m == 2)
            chain.setAdvancedOpen (true); // Creative exposes the full chain by default
        recommendation.setMode (m);
        layoutPanels();
    }

    void onSelectionChanged()
    {
        session.invalidate();
        recommendation.refresh();
    }

    void analyse()
    {
        auto& st = processor.apvts.state;
        const auto goal = (Goal) juce::jlimit (0, numGoals - 1, (int) st.getProperty (PluginProcessor::goal_, 0));
        const auto source = (Source) juce::jlimit (0, numSources - 1, (int) st.getProperty (PluginProcessor::source_, 0));
        const auto intensity = (float) st.getProperty (PluginProcessor::intensity_, 0.5);
        session.analyse (goal, source, intensity);
        const int mode = (int) st.getProperty (PluginProcessor::mode_, 0);
        if (mode == 1 && (bool) st.getProperty (PluginProcessor::autoApply_, false) && session.getRecommendation().outcome == Outcome::suggested)
            session.apply();
        recommendation.refresh();
        header.refresh();
        chain.refreshLive();
    }

    // One UI tick: pull meters and analysis audio, update the live displays.
    void tick()
    {
        processor.pumpAnalysis();
        auto& m = processor.inputMeter;
        const bool stereo = processor.getTotalNumInputChannels() > 1;
        const auto db = [] (float lin) { return juce::Decibels::gainToDecibels (lin, -100.0f); };
        float pk[2] = { db (m.takeSamplePeak (0)), db (m.takeSamplePeak (1)) };
        if (! stereo)
            pk[1] = pk[0];

        for (size_t c = 0; c < 2; ++c)
        {
            disp[c] = pk[c] > disp[c] ? pk[c] : std::max (pk[c], disp[c] - 0.9f);
            if (pk[c] >= hold[c])
            {
                hold[c] = pk[c];
                holdAge[c] = 0;
            }
            else if (++holdAge[c] > 45)
                hold[c] = std::max (pk[c], hold[c] - 0.6f);
        }

        const float blockPeak = std::max (pk[0], pk[1]);
        recentPeaks[(size_t) (recentIndex++ % (int) recentPeaks.size())] = blockPeak;
        waveAccum = std::max (waveAccum, blockPeak);
        if (++waveTick % 2 == 0)
        {
            std::move (wave.begin() + 1, wave.end(), wave.begin());
            wave.back() = juce::jlimit (0.0f, 1.0f, (waveAccum + 60.0f) / 60.0f);
            waveAccum = -100.0f;
        }
        const float peak3s = *std::max_element (recentPeaks.begin(), recentPeaks.end());

        const float rmsLin = stereo ? std::sqrt (0.5f * (m.getRms (0) * m.getRms (0) + m.getRms (1) * m.getRms (1))) : m.getRms (0);
        const float rmsDb = db (rmsLin);
        const bool signal = peak3s > -70.0f;

        AnalysisPanel::Live live;
        live.sampleRate = processor.getSampleRate() > 0 ? processor.getSampleRate() : 48000.0;
        live.wave = wave;
        auto fill = [] (float d) { return juce::jlimit (0.0f, 1.0f, (d + 60.0f) / 60.0f); };
        live.fillL = fill (disp[0]);
        live.fillR = fill (disp[1]);
        live.tickL = fill (hold[0]);
        live.tickR = fill (hold[1]);
        live.textL = signal ? Theme::dbText (hold[0]) : "--";
        live.textR = signal ? Theme::dbText (hold[1]) : "--";
        live.peak = signal ? Theme::dbText (peak3s) : "--";
        live.rms = (signal && rmsDb > -80.0f) ? Theme::dbText (rmsDb) : "--";
        live.crest = (signal && rmsDb > -80.0f) ? juce::String (std::max (0.0f, peak3s - rmsDb), 1) : "--";
        live.width = (signal && stereo) ? juce::String ((int) std::lround (m.getWidthIndex() * 100.0f)) : "--";
        if (! signal)
        {
            live.noteTitle = "No signal";
            live.noteBody = "Play audio through the plugin to see its level and balance.";
        }
        else if (peak3s >= -0.3f)
        {
            live.warning = true;
            live.noteTitle = "Possible clipping";
            live.noteBody = "The input reached " + Theme::dbText (peak3s) + " dBFS. Lower the level before the plugin; processing cannot repair clipped audio.";
        }
        else if (peak3s > -3.0f)
        {
            live.warning = true;
            live.noteTitle = "Hot input level";
            live.noteBody = "Peaks reach " + Theme::dbText (peak3s) + " dBFS, leaving little headroom for boosts.";
        }
        else if (peak3s < -30.0f)
        {
            live.noteTitle = "Very quiet input";
            live.noteBody = "Peaks only reach " + Theme::dbText (peak3s) + " dBFS. Analysis works best on a louder section.";
        }
        else
        {
            live.noteTitle = "Healthy input level";
            live.noteBody = "Peaks reach " + Theme::dbText (peak3s) + " dBFS: good headroom and no detected clipping.";
        }
        analysis.setLive (std::move (live));
        modeBar.setLive (signal, signal ? "LIVE INPUT" : "NO SIGNAL");
        chain.refreshLive();
        if (session.getState() != lastSessionState)
        {
            lastSessionState = session.getState();
            recommendation.refresh();
            header.refresh();
        }
        if (++slowTick % 6 == 0)
        {
            recommendation.refreshLive();
            repaint (footerBounds());
        }
    }

    PluginProcessor& processor;
    RecommendationSession& session;
    HeaderBar header;
    ModeBar modeBar;
    AnalysisPanel analysis;
    DirectionPanel direction;
    RecommendationPanel recommendation;
    ChainPanel chain;

    std::array<float, 2> disp { -100.0f, -100.0f }, hold { -100.0f, -100.0f };
    std::array<int, 2> holdAge { 0, 0 };
    std::array<float, 90> recentPeaks = [] { std::array<float, 90> a; a.fill (-100.0f); return a; }();
    int recentIndex = 0, waveTick = 0, slowTick = 0;
    RecommendationSession::State lastSessionState = RecommendationSession::State::idle;
    float waveAccum = -100.0f;
    std::array<float, AnalysisPanel::numBars> wave {};
};

//==============================================================================
PluginEditor::PluginEditor (PluginProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p), session (p), presets (p)
{
    content = std::make_unique<Content> (p, session, presets);
    addAndMakeVisible (*content);

    const double defaultScale = 0.9;
    setResizeLimits ((int) (Theme::designWidth * 0.6), (int) (Theme::designHeight * 0.6), (int) (Theme::designWidth * 1.6), (int) (Theme::designHeight * 1.6));
    getConstrainer()->setFixedAspectRatio ((double) Theme::designWidth / Theme::designHeight);
    setResizable (true, true);
    setSize ((int) (Theme::designWidth * defaultScale), (int) (Theme::designHeight * defaultScale));
    startTimerHz (30);
}

PluginEditor::~PluginEditor()
{
    stopTimer();
}

void PluginEditor::resized()
{
    const auto scale = (float) getWidth() / (float) Theme::designWidth;
    content->setBounds (0, 0, Theme::designWidth, Theme::designHeight);
    content->setTransform (juce::AffineTransform::scale (scale));
}

void PluginEditor::timerCallback() { content->tick(); }
void PluginEditor::updateLive() { content->tick(); }
void PluginEditor::runAnalysis() { content->analyse(); }
