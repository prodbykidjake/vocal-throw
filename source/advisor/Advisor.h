#pragma once

#include "../analysis/Features.h"
#include <map>
#include <string>
#include <vector>

// Goal-driven recommendations. Pure functions of (goal, source, intensity, measured features,
// current parameter values), so every rule is unit-testable without audio or UI.
//
// A recommendation always separates:
//   measurements     - what the signal shows (numbers)
//   interpretations  - what that might mean (hedged, never stated as fact)
//   changes          - ordinary parameter edits that may help the chosen goal
// and may legitimately propose nothing.

enum class Goal { clarity, warmth, punch, presence, density, smoothness, brightness, body, dynamicControl, reducedHarshness };
enum class Source { vocal, drums, bass, guitar, keys, other };
enum class Module { eq, compressor, saturation };
enum class Outcome { suggested, noChangeNeeded, insufficientSignal };

// Evidence (defined, not a probability): strong = two independent measurements beyond their
// thresholds or one far beyond; moderate = one measurement beyond threshold; weak = none.
// Weak evidence never produces a change.
enum class Evidence { weak, moderate, strong };

using ParamValues = std::map<std::string, float>;

struct ParamChange
{
    std::string id;
    float from = 0.0f, to = 0.0f;
};

struct ChangeGroup
{
    Module module = Module::eq;
    std::string title;  // "Broad tonal adjustment"
    std::string detail; // "-2.1 dB · 320 Hz · Q 1.0"
    std::vector<ParamChange> params;
};

struct Recommendation
{
    Goal goal = Goal::clarity;
    Source source = Source::other;
    float intensity = 0.5f;
    Outcome outcome = Outcome::noChangeNeeded;
    Evidence evidence = Evidence::weak;

    std::string headline;
    std::vector<std::string> measurements;
    std::vector<std::string> interpretations;
    std::string rationale;                // what the proposal should accomplish (or why nothing is proposed)
    std::vector<std::string> manualSteps; // how to do the same by hand
    std::vector<std::string> alternatives; // what to try instead when nothing is proposed
    std::string conceptTip;                  // one-sentence explanation of the processing involved (Guided mode)
    std::vector<ChangeGroup> changes;

    // id -> new value for every parameter this recommendation touches.
    ParamValues targetValues() const
    {
        ParamValues v;
        for (const auto& g : changes)
            for (const auto& p : g.params)
                v[p.id] = p.to;
        return v;
    }
};

const char* goalName (Goal);
const char* goalTagline (Goal);
const char* sourceName (Source);
const char* intensityName (float intensity01);
constexpr int numGoals = 10;
constexpr int numSources = 6;

// Hard limits no recommendation may exceed, regardless of input.
namespace AdvisorLimits
{
inline constexpr float maxEqGainDb = 6.0f;     // absolute EQ band gain after the change
inline constexpr float maxRatio = 4.0f;
inline constexpr float maxTargetReductionDb = 6.0f;
inline constexpr float maxSatDriveDb = 8.0f;
inline constexpr float maxSatMix = 60.0f;      // percent
}

Recommendation advise (Goal goal, Source source, float intensity01, const Features& features, const ParamValues& current);
