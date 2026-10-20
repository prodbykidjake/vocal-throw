#pragma once

#include "../PluginProcessor.h"
#include "../advisor/Advisor.h"

// Message-thread controller connecting the analyzer and advisor to the live parameters.
// Everything a recommendation does is an ordinary, undoable parameter edit.
class RecommendationSession
{
public:
    enum class State { idle, ready, previewing, applied };

    explicit RecommendationSession (PluginProcessor& p) : processor (p) {}

    // Analyzes the most recent audio (up to `seconds`) and computes a recommendation. No parameters change.
    void analyse (Goal goal, Source source, float intensity01, float seconds = 10.0f)
    {
        processor.pumpAnalysis();
        features = processor.analyzer.analyseRecent (seconds);
        recommendation = advise (goal, source, intensity01, features, processor.getParamValues());
        snapshot.clear();
        state = State::ready;
    }

    // Applies the proposal so it can be auditioned; Reject restores exactly what was there before.
    bool preview()
    {
        if (state != State::ready || recommendation.outcome != Outcome::suggested)
            return false;
        snapshot = processor.getParamValues();
        previewUndoName = juce::String ("Apply ") + goalName (recommendation.goal);
        processor.setParamValues (recommendation.targetValues(), previewUndoName);
        state = State::previewing;
        return true;
    }

    // Keeps the previewed (or directly applies the proposed) settings. They stay editable and undoable.
    bool apply()
    {
        if (state == State::ready && ! preview())
            return false;
        if (state != State::previewing)
            return false;
        state = State::applied;
        return true;
    }

    // Restores the settings from before the preview. Only valid while previewing.
    bool reject()
    {
        if (state != State::previewing)
            return false;
        // If nothing else has happened since the preview, simply undo it so the history stays clean.
        if (processor.undoManager.getUndoDescription() == previewUndoName)
            processor.undoManager.undo();
        else
            processor.setParamValues (snapshot, "Reject recommendation");
        state = State::ready;
        return true;
    }

    // The applied settings were undone by the user: the proposal can be previewed or applied again.
    void markUndone()
    {
        if (state == State::applied || state == State::previewing)
            state = State::ready;
    }

    // Call when the user changes the goal/source/intensity or edits parameters: the proposal is stale.
    void invalidate()
    {
        if (state == State::ready)
            state = State::idle;
    }

    State getState() const { return state; }
    const Recommendation& getRecommendation() const { return recommendation; }
    const Features& getFeatures() const { return features; }

private:
    PluginProcessor& processor;
    State state = State::idle;
    Features features;
    Recommendation recommendation;
    ParamValues snapshot;
    juce::String previewUndoName;
};
