#include "Advisor.h"
#include "../params/Parameters.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace
{
constexpr float kTol = 3.0f; // dB outside the typical range before a deviation counts as evidence

std::string num (float v, int decimals = 1)
{
    std::ostringstream s;
    s.setf (std::ios::fixed);
    s.precision (decimals);
    s << (std::abs (v) < 0.05f && decimals == 1 ? 0.0f : v);
    return s.str();
}
std::string signedDb (float v) { return (v >= 0 ? "+" : "-") + num (std::abs (v)) + " dB"; }
std::string hz (float f) { return f >= 1000.0f ? num (f / 1000.0f, f >= 10000.0f ? 0 : 1) + " kHz" : num (f, 0) + " Hz"; }

// Nominal group levels relative to the 200 Hz - 5 kHz reference (dB). Provisional heuristics, wide
// tolerance on purpose; to be tuned against the listening-test library before release.
struct Nominal
{
    float bass, lowMid, mid, upperMid, high, air;
};
constexpr Nominal nominalFor (Source s)
{
    switch (s)
    {
        case Source::vocal: return { -8, 1, 0, -1, -8, -16 };
        case Source::drums: return { 2, -1, -1, -2, -5, -10 };
        case Source::bass: return { 8, 2, -8, -14, -22, -30 };
        case Source::guitar: return { -5, 1, 1, -1, -9, -18 };
        case Source::keys: return { -3, 1, 0, -3, -10, -18 };
        case Source::other: return { -4, 0, 0, -2, -8, -16 };
    }
    return { 0, 0, 0, 0, 0, 0 };
}

struct Balance
{
    float bass, lowMid, mid, upperMid, high, air; // measured minus nominal, dB
};

Balance balanceOf (const Features& f, Source s)
{
    const auto ref = f.groupDb (200.0f, 5000.0f);
    const auto n = nominalFor (s);
    return { f.groupDb (60, 200) - ref - n.bass, f.groupDb (200, 500) - ref - n.lowMid, f.groupDb (500, 2000) - ref - n.mid,
             f.groupDb (2000, 5000) - ref - n.upperMid, f.groupDb (5000, 10000) - ref - n.high, f.groupDb (10000, 18000) - ref - n.air };
}

const char* nameForSource (Source s)
{
    switch (s)
    {
        case Source::vocal: return "a lead vocal";
        case Source::drums: return "drums";
        case Source::bass: return "a bass";
        case Source::guitar: return "a guitar";
        case Source::keys: return "keys";
        case Source::other: return "this type of source";
    }
    return "";
}

std::string versus (float dev, const char* region, Source s)
{
    return "Energy at " + std::string (region) + " is " + num (std::abs (dev)) + " dB " + (dev >= 0 ? "above" : "below") + " the typical level for " + nameForSource (s) + ".";
}

struct Resonance
{
    float freq = 0.0f;
    float prominenceDb = 0.0f;
};

// Largest narrow peak (relative to the higher of its 1/3..2/3-octave neighbourhoods on either side) between loHz and hiHz.
Resonance findResonance (const Features& f, float loHz, float hiHz)
{
    Resonance best;
    for (int i = 4; i < Features::numBands - 4; ++i)
    {
        const auto fc = Features::bandCentre (i);
        if (fc < loHz || fc > hiHz)
            continue;
        const auto b = [&] (int k) { return f.bandDb[(size_t) k]; };
        // A resonance stands above BOTH sides; the edge of a broad step or roll-off does not.
        const auto below = (b (i - 4) + b (i - 3) + b (i - 2)) / 3.0f;
        const auto above = (b (i + 2) + b (i + 3) + b (i + 4)) / 3.0f;
        const auto prom = b (i) - std::max (below, above);
        if (prom > best.prominenceDb)
            best = { fc, prom };
    }
    return best;
}

float strongestBandIn (const Features& f, float loHz, float hiHz)
{
    int best = -1;
    for (int i = 0; i < Features::numBands; ++i)
    {
        const auto fc = Features::bandCentre (i);
        if (fc >= loHz && fc < hiHz && (best < 0 || f.bandDb[(size_t) i] > f.bandDb[(size_t) best]))
            best = i;
    }
    return best < 0 ? std::sqrt (loHz * hiHz) : Features::bandCentre (best);
}

// Collects parameter edits relative to the current values and keeps them inside the safety limits.
class Builder
{
public:
    Builder (const ParamValues& currentValues, Recommendation& r) : cur (currentValues), rec (r) {}

    float get (const std::string& id, float fallback) const
    {
        const auto it = cur.find (id);
        return it == cur.end() ? fallback : it->second;
    }
    bool isOn (const char* id) const { return get (id, 0.0f) > 0.5f; }

    // band: 0 low shelf, 1 bell, 2 bell, 3 high shelf. Returns false when the change would be negligible.
    bool addEq (int band, float freq, float deltaDb, float q, const std::string& title)
    {
        const auto idx = (size_t) band;
        const auto curGain = get (ParamIDs::eqGain[idx], 0.0f);
        const bool busy = std::abs (curGain) > 0.25f; // the user (or an earlier edit) is already using this band
        const auto newGain = std::clamp (curGain + deltaDb, -AdvisorLimits::maxEqGainDb, AdvisorLimits::maxEqGainDb);
        if (std::abs (newGain - curGain) < 0.2f)
            return false;

        ChangeGroup g { Module::eq, title, "", {} };
        const auto curFreq = get (ParamIDs::eqFreq[idx], Params::eqFreqRange[idx].def);
        const auto curQ = get (ParamIDs::eqQ[idx], 1.0f);
        const auto useFreq = busy ? curFreq : clampTo (freq, Params::eqFreqRange[idx]);
        const auto useQ = busy ? curQ : clampTo (q, Params::eqQRange);
        g.params.push_back ({ ParamIDs::eqGain[idx], curGain, newGain });
        if (! busy)
        {
            g.params.push_back ({ ParamIDs::eqFreq[idx], curFreq, useFreq });
            g.params.push_back ({ ParamIDs::eqQ[idx], curQ, useQ });
        }
        if (! isOn (ParamIDs::eqEnable) && cur.count (ParamIDs::eqEnable))
            g.params.push_back ({ ParamIDs::eqEnable, 0.0f, 1.0f });
        g.detail = signedDb (newGain) + " · " + hz (useFreq) + " · Q " + num (useQ);
        static const char* kinds[] = { "low shelf", "bell", "bell", "high shelf" };
        rec.manualSteps.push_back ("EQ band " + std::to_string (band + 1) + " (" + kinds[idx] + "): " + hz (useFreq) + ", " + signedDb (newGain) + ", Q " + num (useQ) + ".");
        rec.changes.push_back (std::move (g));
        return true;
    }

    // Returns false (and adds nothing) when the compressor is already in use so a user's settings are never overridden.
    bool addCompressor (float ratio, float attackMs, float releaseMs, float kneeDb, float targetReductionDb, const Features& f, const std::string& title)
    {
        if (isOn (ParamIDs::compEnable))
            return false;
        ratio = std::clamp (ratio, 1.5f, AdvisorLimits::maxRatio);
        targetReductionDb = std::clamp (targetReductionDb, 0.5f, AdvisorLimits::maxTargetReductionDb);
        // threshold so that "typical peaks" (RMS + half the crest factor) get the target reduction
        const auto typicalPeak = f.rmsDb + 0.5f * f.crestDb;
        const auto threshold = std::clamp (typicalPeak - targetReductionDb / (1.0f - 1.0f / ratio), Params::thresholdRange.min, Params::thresholdRange.max);

        ChangeGroup g { Module::compressor, title, "", {} };
        auto push = [&] (const char* id, float to, float fallback) { g.params.push_back ({ id, get (id, fallback), to }); };
        push (ParamIDs::compThreshold, threshold, Params::thresholdRange.def);
        push (ParamIDs::compRatio, ratio, Params::ratioRange.def);
        push (ParamIDs::compAttack, attackMs, Params::attackRange.def);
        push (ParamIDs::compRelease, releaseMs, Params::releaseRange.def);
        push (ParamIDs::compKnee, kneeDb, Params::kneeRange.def);
        push (ParamIDs::compMakeup, 0.0f, 0.0f);
        push (ParamIDs::compMix, 100.0f, 100.0f);
        g.params.push_back ({ ParamIDs::compEnable, get (ParamIDs::compEnable, 0.0f), 1.0f });
        g.detail = num (ratio) + ":1 · " + num (attackMs, 0) + " ms attack · ~" + num (targetReductionDb) + " dB GR";
        rec.manualSteps.push_back ("Compressor: threshold " + num (threshold, 0) + " dB, ratio " + num (ratio) + ":1, attack " + num (attackMs, 0) + " ms, release " + num (releaseMs, 0)
                                   + " ms, knee " + num (kneeDb, 0) + " dB; aim for roughly " + num (targetReductionDb) + " dB of gain reduction on the loudest parts.");
        rec.changes.push_back (std::move (g));
        return true;
    }

    bool addSaturation (float driveDb, float mixPercent, const std::string& title)
    {
        if (isOn (ParamIDs::satEnable))
            return false;
        driveDb = std::min (driveDb, AdvisorLimits::maxSatDriveDb);
        mixPercent = std::min (mixPercent, AdvisorLimits::maxSatMix);
        ChangeGroup g { Module::saturation, title, "", {} };
        g.params.push_back ({ ParamIDs::satDrive, get (ParamIDs::satDrive, Params::driveRange.def), driveDb });
        g.params.push_back ({ ParamIDs::satMix, get (ParamIDs::satMix, 100.0f), mixPercent });
        g.params.push_back ({ ParamIDs::satEnable, get (ParamIDs::satEnable, 0.0f), 1.0f });
        g.detail = "Drive " + num (driveDb, 0) + " dB · mix " + num (mixPercent, 0) + "%";
        rec.manualSteps.push_back ("Saturation: drive " + num (driveDb, 0) + " dB, mix " + num (mixPercent, 0) + "%.");
        rec.changes.push_back (std::move (g));
        return true;
    }

private:
    static float clampTo (float v, const Params::Range& r) { return std::clamp (v, r.min, r.max); }

    const ParamValues& cur;
    Recommendation& rec;
};

void raise (Evidence& e, Evidence to)
{
    if (to > e)
        e = to;
}

struct Ctx
{
    const Features& f;
    Source source;
    float i; // intensity 0..1
    Balance bal;
    Builder& b;
    Recommendation& rec;
    bool tasteOk = true; // false when the measurements argue against any move in this goal's direction

    void measure (const std::string& s) { rec.measurements.push_back (s); }
    void interpret (const std::string& s) { rec.interpretations.push_back (s); }
};

// ------------------------------------------------------------------------------------------------
void clarity (Ctx& c)
{
    c.rec.conceptTip = "A bell EQ cut in the low-mids reduces the build-up that can make a sound feel cloudy, so other detail is easier to hear.";
    c.measure (versus (c.bal.lowMid, "200-500 Hz", c.source));
    c.measure (versus (c.bal.upperMid, "2-5 kHz", c.source));
    bool acted = false;
    if (c.bal.lowMid > kTol)
    {
        c.interpret ("Extra low-mid energy often reads as muddiness or boxiness, though it can also be part of the source's character.");
        const auto freq = strongestBandIn (c.f, 200.0f, 450.0f);
        const auto cut = -std::min (0.5f * c.bal.lowMid, 4.0f) * c.i;
        acted |= c.b.addEq (1, freq, cut, 1.0f, "Low-mid cleanup");
        raise (c.rec.evidence, c.bal.lowMid > 6.0f ? Evidence::strong : Evidence::moderate);
    }
    if (c.bal.upperMid < -kTol && c.bal.high < kTol)
    {
        c.interpret ("The 2-5 kHz region is lean, which can make detail feel recessed.");
        const auto boost = std::min (-0.4f * c.bal.upperMid, 2.5f) * c.i;
        const bool did = c.b.addEq (2, 3500.0f, boost, 0.9f, "Gentle definition lift");
        acted |= did;
        raise (c.rec.evidence, acted && did && c.rec.changes.size() > 1 ? Evidence::strong : Evidence::moderate);
    }
    if (! acted)
    {
        c.rec.rationale = "The low-mid and presence balance are both within the typical range for this source, so an EQ change is unlikely to add clarity. "
                          "If it still sounds unclear, the cause may be the arrangement, masking by other tracks, or the recording itself.";
        c.rec.alternatives.push_back ("Solo the track and sweep a narrow EQ boost to find any problem region by ear before cutting.");
    }
    else
        c.rec.rationale = "Reduce low-mid build-up and restore definition without changing the overall tone much.";
}

void warmth (Ctx& c)
{
    c.rec.conceptTip = "A gentle low-mid boost adds weight, and light saturation adds harmonics that make a sound feel fuller and closer.";
    c.measure (versus (c.bal.lowMid, "200-500 Hz", c.source));
    c.measure (versus (c.bal.upperMid, "2-5 kHz", c.source));
    if (c.bal.lowMid > kTol)
    {
        c.tasteOk = false;
        c.interpret ("The low-mids are already full; adding more risks muddiness.");
        c.rec.rationale = "This signal already has plenty of low-mid weight, so warmth is not limited by the tonal balance.";
        c.rec.alternatives.push_back ("If it still feels cold, try the saturator at a low mix and compare at matched loudness.");
        return;
    }
    if (c.bal.lowMid >= -2.5f)
    {
        c.interpret ("The low-mid balance is typical, so warmth here is a matter of taste rather than a measured deficiency.");
        c.rec.rationale = "Measurements do not show a lean low end, so I will not guess. Warmth is subjective: listen, then try the alternatives.";
        c.rec.alternatives.push_back ("Saturation: drive 2-4 dB at 30-40% mix.");
        c.rec.alternatives.push_back ("EQ band 2: +1 to +2 dB around 250 Hz with a wide Q.");
        return;
    }
    c.interpret ("The low-mid region is lean, which can sound thin or cold, though that may be intentional.");
    const auto boost = std::min (0.5f * (-c.bal.lowMid), 3.0f) * c.i;
    c.b.addEq (1, 250.0f, boost, 0.7f, "Broad tonal adjustment");
    raise (c.rec.evidence, c.bal.lowMid < -6.0f ? Evidence::strong : Evidence::moderate);
    if (c.f.clippingEvents == 0 && c.bal.upperMid < 4.0f && c.f.crestDb > 6.0f)
        c.b.addSaturation (1.5f + 3.0f * c.i, 35.0f, "Restrained harmonic weight");
    c.rec.rationale = "Add low-mid weight, with light saturation for harmonics, while keeping the top end intact.";
}

void punch (Ctx& c)
{
    c.rec.conceptTip = "A compressor with a slower attack lets the first part of each hit through before turning the level down, which emphasises impact.";
    c.measure ("Crest factor is " + num (c.f.crestDb) + " dB.");
    c.measure ("Level rises up to " + num (c.f.onsetStrengthDb) + " dB per 10 ms at note onsets.");
    if (c.f.crestDb < 8.0f)
    {
        c.tasteOk = false;
        c.interpret ("A low crest factor means the peaks are already close to the average level. That can come from compression or from a sustained source.");
        c.rec.rationale = "Peaks are already close to the average level, so more compression would reduce punch rather than add it.";
        c.rec.alternatives.push_back ("Try transient emphasis on a drum bus, or a short parallel-compressed copy, if you want more snap.");
        return;
    }
    if (c.f.crestDb >= 10.0f && c.f.onsetStrengthDb >= 3.0f)
    {
        c.interpret ("There are strong peaks and clear onsets, so a slow-attack compressor can shape the transient while evening the body.");
        c.b.addCompressor (2.0f + 2.0f * c.i, 25.0f, 120.0f, 3.0f, 2.0f + 3.0f * c.i, c.f, "Transient-friendly compression");
        raise (c.rec.evidence, (c.f.crestDb >= 13.0f && c.f.onsetStrengthDb >= 5.0f) ? Evidence::strong : Evidence::moderate);
    }
    if ((c.source == Source::drums || c.source == Source::bass) && c.bal.bass < -kTol)
    {
        c.measure (versus (c.bal.bass, "60-200 Hz", c.source));
        c.interpret ("A lean low end can reduce the felt weight of each hit.");
        if (c.b.addEq (0, 90.0f, std::min (0.4f * -c.bal.bass, 2.5f) * c.i, 0.7f, "Low-end weight"))
            raise (c.rec.evidence, Evidence::moderate);
    }
    if (c.rec.changes.empty())
    {
        c.rec.rationale = "The dynamics measurements do not clearly show where extra punch would come from. A recommendation here would be a guess.";
        c.rec.alternatives.push_back ("Try a compressor with 20-30 ms attack and 3:1 ratio, and judge it at matched loudness.");
    }
    else
        c.rec.rationale = "Let the attack through, then tighten the body.";
}

void presence (Ctx& c)
{
    c.rec.conceptTip = "A broad boost between 2 and 5 kHz makes a sound feel closer and more forward because that is where the ear is most sensitive.";
    c.measure (versus (c.bal.upperMid, "2-5 kHz", c.source));
    const auto res = findResonance (c.f, 2000.0f, 8000.0f);
    if (res.prominenceDb >= 6.0f)
        c.measure ("There is a narrow peak near " + hz (res.freq) + " (" + num (res.prominenceDb) + " dB above its surroundings).");
    if (c.bal.upperMid > kTol)
    {
        c.tasteOk = false;
        c.interpret ("The upper-mids are already prominent.");
        c.rec.rationale = "This signal is already forward in the presence region; boosting would more likely cause harshness than presence.";
        c.rec.alternatives.push_back ("If it still sits back in the mix, check level and the other tracks competing at 2-5 kHz.");
        return;
    }
    if (res.prominenceDb >= 6.0f)
    {
        c.tasteOk = false;
        c.interpret ("A broad boost would also lift that narrow peak, which could sound harsh.");
        c.rec.rationale = "Boosting presence would emphasise the narrow peak near " + hz (res.freq) + ". Try the Reduced harshness goal first.";
        return;
    }
    if (c.bal.upperMid >= -kTol)
    {
        c.interpret ("The presence region is within the typical range, so any boost would be a taste decision.");
        c.rec.rationale = "Measurements do not show a lean presence region, so I will not add a boost automatically.";
        c.rec.alternatives.push_back ("EQ band 3: +1 to +2 dB around 3 kHz with Q 0.9; compare at matched loudness.");
        return;
    }
    c.interpret ("The presence region is lean, which can make a sound feel distant.");
    const float centre = c.source == Source::vocal ? 3500.0f : (c.source == Source::drums ? 4500.0f : 3200.0f);
    c.b.addEq (2, centre, std::min (-0.6f * c.bal.upperMid, 3.5f) * c.i, 0.9f, "Presence lift");
    raise (c.rec.evidence, c.bal.upperMid < -6.0f ? Evidence::strong : Evidence::moderate);
    c.rec.rationale = "Bring the signal forward with a broad, modest lift.";
}

void density (Ctx& c)
{
    c.rec.conceptTip = "Gentle compression reduces the gap between loud and quiet parts so the sound feels thicker and steadier.";
    c.measure ("Crest factor is " + num (c.f.crestDb) + " dB.");
    c.measure ("Level varies by " + num (c.f.dynamicRangeDb) + " dB between quiet and loud passages.");
    if (c.f.dynamicRangeDb < 5.0f && c.f.crestDb < 9.0f)
    {
        c.tasteOk = false;
        c.interpret ("Level is already steady and peaks are modest, so the signal is already dense.");
        c.rec.rationale = "This signal is already fairly dense. More compression would mostly add pumping.";
        c.rec.alternatives.push_back ("Light parallel saturation can add perceived density without more compression.");
        return;
    }
    c.interpret ("There is noticeable level variation, so gentle compression can make the signal feel thicker.");
    c.b.addCompressor (2.5f, 12.0f, 150.0f, 6.0f, 2.0f + 2.0f * c.i, c.f, "Gentle dynamic support");
    raise (c.rec.evidence, (c.f.dynamicRangeDb >= 8.0f && c.f.crestDb >= 11.0f) ? Evidence::strong : Evidence::moderate);
    c.rec.rationale = "Thicken the signal by evening out the level.";
}

void smoothness (Ctx& c)
{
    c.rec.conceptTip = "Soft compression evens out sudden jumps in level, and trimming the top end reduces spiky highs; together they make a sound feel less jagged.";
    c.measure ("Level varies by " + num (c.f.dynamicRangeDb) + " dB between quiet and loud passages.");
    c.measure (versus (c.bal.high, "5-10 kHz", c.source));
    if (c.f.dynamicRangeDb >= 6.0f)
    {
        c.interpret ("Wide level variation can make a performance feel uneven.");
        c.b.addCompressor (2.0f, 20.0f, 200.0f, 12.0f, 1.5f + 1.5f * c.i, c.f, "Soft level evening");
        raise (c.rec.evidence, Evidence::moderate);
    }
    if (c.bal.high > kTol)
    {
        c.interpret ("The highs are strong for this source, which can sound edgy.");
        if (c.b.addEq (3, 7000.0f, -std::min (0.4f * c.bal.high, 3.0f) * c.i, 0.7f, "High-frequency softening"))
            raise (c.rec.evidence, c.f.dynamicRangeDb >= 6.0f ? Evidence::strong : Evidence::moderate);
    }
    if (c.rec.changes.empty())
    {
        c.rec.rationale = "Neither the level variation nor the high-frequency balance suggests an obvious source of roughness.";
        c.rec.alternatives.push_back ("Listen for the roughness, then identify whether it is level-related (compress) or tonal (EQ).");
    }
    else
        c.rec.rationale = "Even out the level and soften edgy highs without dulling the sound.";
}

void brightness (Ctx& c)
{
    c.rec.conceptTip = "A high shelf lifts everything above its frequency, adding air and sparkle.";
    const auto dHi = 0.6f * c.bal.high + 0.4f * c.bal.air;
    c.measure (versus (c.bal.high, "5-10 kHz", c.source));
    c.measure (versus (c.bal.air, "10-18 kHz", c.source));
    if (c.f.clippingEvents > 2)
    {
        c.measure ("Clipping was detected (" + std::to_string (c.f.clippingEvents) + " events).");
        c.tasteOk = false;
        c.interpret ("Boosting highs on clipped audio makes the distortion more audible.");
        c.rec.rationale = "Clipping is present, so a high-frequency boost would exaggerate it. Lower the source level or fix the clipping first.";
        return;
    }
    if (dHi > kTol || c.bal.upperMid > kTol + 1.0f)
    {
        c.tasteOk = false;
        c.interpret ("The highs are already strong for this source.");
        c.rec.rationale = "There is no measured shortage of high-frequency energy, so a boost is unlikely to help.";
        return;
    }
    if (dHi > -kTol)
    {
        c.interpret ("The high-frequency balance is typical, so more brightness is a taste decision.");
        c.rec.rationale = "Measurements do not show a dull top end, so I will not add a boost automatically.";
        c.rec.alternatives.push_back ("EQ band 4: +1 to +2 dB high shelf at 8 kHz, then compare at matched loudness. Beware lifting noise.");
        return;
    }
    c.interpret ("The top end is lower than typical for this source, which can sound dull. High-frequency boosts also raise any noise present.");
    c.b.addEq (3, 8000.0f, std::min (-0.5f * dHi, 4.0f) * c.i, 0.7f, "Top-end lift");
    raise (c.rec.evidence, dHi < -6.0f ? Evidence::strong : Evidence::moderate);
    c.rec.rationale = "Restore some air with a gentle high shelf.";
}

void body (Ctx& c)
{
    c.rec.conceptTip = "A low shelf raises everything below its frequency, adding weight and fullness.";
    c.measure (versus (c.bal.bass, "60-200 Hz", c.source));
    c.measure (versus (c.bal.lowMid, "200-500 Hz", c.source));
    const auto lean = std::max (-c.bal.bass, -c.bal.lowMid);
    if (c.bal.bass > kTol || c.bal.lowMid > kTol)
    {
        c.tasteOk = false;
        c.interpret ("There is already substantial low-end energy for this source.");
        c.rec.rationale = "Body is unlikely to be limited by the tonal balance here. More low end could sound boomy or muddy.";
        c.rec.alternatives.push_back ("If it still sounds thin in the mix, check for high-pass filtering on the track or masking from other low instruments.");
        return;
    }
    if (lean < kTol)
    {
        c.interpret ("The low-end balance is typical.");
        c.rec.rationale = "Measurements do not show a lean low end, so I will not add weight automatically.";
        c.rec.alternatives.push_back ("EQ band 1: +1 to +2 dB low shelf at 120 Hz; compare at matched loudness.");
        return;
    }
    c.interpret ("The low end is lean for this source, which can sound thin.");
    c.b.addEq (0, 120.0f, std::min (0.5f * lean, 3.5f) * c.i, 0.7f, "Low-end weight");
    raise (c.rec.evidence, lean > 6.0f ? Evidence::strong : Evidence::moderate);
    c.rec.rationale = "Add weight with a low shelf.";
}

void dynamicControl (Ctx& c)
{
    c.rec.conceptTip = "A compressor turns down the loud parts so the level stays more consistent.";
    c.measure ("Level varies by " + num (c.f.dynamicRangeDb) + " dB between quiet and loud passages.");
    c.measure ("Crest factor is " + num (c.f.crestDb) + " dB.");
    if (c.f.dynamicRangeDb < 6.0f && c.f.crestDb < 12.0f)
    {
        c.tasteOk = false;
        c.interpret ("The level is already fairly consistent.");
        c.rec.rationale = "There is little level variation to control, so compression would mostly change the character.";
        c.rec.alternatives.push_back ("If one note or word still jumps out, try clip gain or automation instead.");
        return;
    }
    c.interpret ("Level varies noticeably, so compression can make the performance sit more consistently in a mix.");
    const auto ratio = std::min (AdvisorLimits::maxRatio, 2.0f + 2.0f * c.i + (c.f.dynamicRangeDb >= 10.0f ? 0.5f : 0.0f));
    c.b.addCompressor (ratio, 15.0f, 150.0f, 6.0f, 2.0f + 3.0f * c.i, c.f, "Level control");
    raise (c.rec.evidence, (c.f.dynamicRangeDb >= 10.0f) ? Evidence::strong : Evidence::moderate);
    c.rec.rationale = "Keep the level more consistent.";
}

void reducedHarshness (Ctx& c)
{
    c.rec.conceptTip = "A narrow EQ cut at a resonant frequency turns down a harsh spike without dulling the rest of the sound.";
    const auto res = findResonance (c.f, 2000.0f, 8000.0f);
    c.measure ("The strongest narrow peak in 2-8 kHz is near " + hz (res.freq) + ", " + num (res.prominenceDb) + " dB above its surroundings.");
    c.measure (versus (c.bal.upperMid, "2-5 kHz", c.source));
    if (res.prominenceDb >= 4.0f)
    {
        c.interpret ("A narrow peak like this can sound harsh or whistly, but it may also be a held note or deliberate character.");
        const auto cut = -std::min (0.6f * res.prominenceDb, 5.0f) * c.i;
        if (c.b.addEq (2, res.freq, cut, 2.5f, "Narrow resonance cut"))
            raise (c.rec.evidence, res.prominenceDb >= 8.0f ? Evidence::strong : Evidence::moderate);
        c.rec.rationale = "Reduce the peak near " + hz (res.freq) + " modestly instead of dulling the whole top end.";
    }
    else if (c.bal.upperMid > kTol)
    {
        c.interpret ("There is no single dominant peak, but the whole upper-mid region is strong for this source, which can sound harsh.");
        c.b.addEq (2, 4000.0f, -std::min (0.5f * c.bal.upperMid, 3.0f) * c.i, 0.8f, "Broad upper-mid cut");
        raise (c.rec.evidence, Evidence::moderate);
        c.rec.rationale = "Ease the upper-mids with a broad, gentle cut.";
    }
    else
    {
        c.interpret ("No resonance stands out and the upper-mid balance is typical.");
        c.rec.rationale = "The measurements do not show a harsh region, so I will not cut anything. If it still sounds harsh to you, it may be distortion or the playback chain.";
        c.rec.alternatives.push_back ("Sweep a narrow EQ boost through 2-8 kHz to find the offending frequency by ear, then cut it.");
    }
}

// A small, reversible move that is purely a matter of taste (used only when nothing was measured as a problem and
// the measurements do not argue against it). Always scaled by the intensity and clearly labelled optional.
void tasteMove (Ctx& c, Goal goal)
{
    const float i = c.i;
    switch (goal)
    {
        case Goal::clarity: c.b.addEq (1, 300.0f, -3.0f * i, 1.0f, "Gentle low-mid trim"); break;
        case Goal::warmth:
            c.b.addEq (1, 250.0f, 2.4f * i, 0.7f, "Soft low-mid warmth");
            if (c.f.clippingEvents == 0)
                c.b.addSaturation (1.5f + 2.0f * i, 25.0f, "Light harmonic weight");
            break;
        case Goal::punch: c.b.addCompressor (2.5f, 25.0f, 120.0f, 3.0f, 1.5f + 1.5f * i, c.f, "Slow-attack compression"); break;
        case Goal::presence: c.b.addEq (2, c.source == Source::vocal ? 3500.0f : 3200.0f, 2.4f * i, 0.9f, "Soft presence lift"); break;
        case Goal::density: c.b.addCompressor (2.0f, 12.0f, 150.0f, 6.0f, 1.5f + 1.0f * i, c.f, "Gentle thickening"); break;
        case Goal::smoothness: c.b.addEq (3, 7000.0f, -2.0f * i, 0.7f, "Soft top-end ease"); break;
        case Goal::brightness: c.b.addEq (3, 8000.0f, 2.4f * i, 0.7f, "Soft air lift"); break;
        case Goal::body: c.b.addEq (0, 120.0f, 2.4f * i, 0.7f, "Soft low-end weight"); break;
        case Goal::dynamicControl: c.b.addCompressor (2.5f, 15.0f, 150.0f, 6.0f, 2.0f + 1.0f * i, c.f, "Light level control"); break;
        case Goal::reducedHarshness: c.b.addEq (2, 4000.0f, -1.8f * i, 0.9f, "Gentle upper-mid ease"); break;
    }
}

void enforceLimits (Recommendation& r)
{
    for (auto& g : r.changes)
        for (auto& p : g.params)
        {
            const auto id = p.id;
            auto clampTo = [&] (const Params::Range& range) { p.to = std::clamp (p.to, range.min, range.max); };
            for (size_t i = 0; i < 4; ++i)
            {
                if (id == ParamIDs::eqGain[i])
                    p.to = std::clamp (p.to, -AdvisorLimits::maxEqGainDb, AdvisorLimits::maxEqGainDb);
                if (id == ParamIDs::eqFreq[i])
                    clampTo (Params::eqFreqRange[i]);
                if (id == ParamIDs::eqQ[i])
                    clampTo (Params::eqQRange);
            }
            if (id == ParamIDs::compThreshold) clampTo (Params::thresholdRange);
            if (id == ParamIDs::compRatio) p.to = std::clamp (p.to, 1.0f, AdvisorLimits::maxRatio);
            if (id == ParamIDs::compAttack) clampTo (Params::attackRange);
            if (id == ParamIDs::compRelease) clampTo (Params::releaseRange);
            if (id == ParamIDs::compKnee) clampTo (Params::kneeRange);
            if (id == ParamIDs::satDrive) p.to = std::clamp (p.to, 0.0f, AdvisorLimits::maxSatDriveDb);
            if (id == ParamIDs::satMix) p.to = std::clamp (p.to, 0.0f, AdvisorLimits::maxSatMix);
        }
}
}

// ------------------------------------------------------------------------------------------------
const char* goalName (Goal g)
{
    static const char* names[] = { "Clarity", "Warmth", "Punch", "Presence", "Density", "Smoothness", "Brightness", "Body", "Dynamic control", "Reduced harshness" };
    return names[(int) g];
}

const char* goalTagline (Goal g)
{
    static const char* t[] = { "Open and defined", "Rich and rounded", "Impact and movement", "Forward and focused", "Thick and steady",
                               "Even and polished", "Air and sparkle", "Weight and fullness", "Consistent level", "Less edge" };
    return t[(int) g];
}

const char* sourceName (Source s)
{
    static const char* n[] = { "Lead vocal", "Drums", "Bass", "Guitar", "Keys", "Other" };
    return n[(int) s];
}

const char* intensityName (float v)
{
    return v < 0.25f ? "Subtle" : (v < 0.65f ? "Balanced" : (v < 0.9f ? "Strong" : "Maximum"));
}

Recommendation advise (Goal goal, Source source, float intensity01, const Features& features, const ParamValues& current)
{
    Recommendation rec;
    rec.goal = goal;
    rec.source = source;
    rec.intensity = std::clamp (intensity01, 0.0f, 1.0f);

    if (! features.valid)
    {
        rec.outcome = Outcome::insufficientSignal;
        rec.headline = "Not enough signal to analyze";
        rec.measurements.push_back ("Only " + num (features.activeSeconds) + " s of signal above the silence gate was heard (at least 1.0 s is needed).");
        rec.rationale = "Play the loudest, most representative section of the track, then analyze again.";
        return rec;
    }

    Builder builder (current, rec);
    Ctx ctx { features, source, rec.intensity, balanceOf (features, source), builder, rec };
    switch (goal)
    {
        case Goal::clarity: clarity (ctx); break;
        case Goal::warmth: warmth (ctx); break;
        case Goal::punch: punch (ctx); break;
        case Goal::presence: presence (ctx); break;
        case Goal::density: density (ctx); break;
        case Goal::smoothness: smoothness (ctx); break;
        case Goal::brightness: brightness (ctx); break;
        case Goal::body: body (ctx); break;
        case Goal::dynamicControl: dynamicControl (ctx); break;
        case Goal::reducedHarshness: reducedHarshness (ctx); break;
    }

    if (features.clippingEvents > 2 && goal != Goal::brightness)
        rec.measurements.push_back ("Clipping was detected (" + std::to_string (features.clippingEvents) + " events). Processing cannot repair clipped audio.");

    // Nothing measured as a problem: offer a small taste move (clearly labelled, weak evidence) rather than a dead end,
    // unless the measurements argue against moving in this goal's direction.
    if (rec.changes.empty() && ctx.tasteOk)
    {
        const auto measuredReason = rec.rationale;
        tasteMove (ctx, goal);
        if (! rec.changes.empty())
        {
            rec.optional = true;
            rec.evidence = Evidence::weak;
            rec.manualSteps.insert (rec.manualSteps.begin(), "This is optional: nothing here measures as a problem, so judge it by ear.");
            if (! measuredReason.empty())
                rec.interpretations.insert (rec.interpretations.begin(), measuredReason);
            rec.rationale = "Nothing measures as a problem for this goal, so this is an optional taste move, not a fix. Preview it and keep it only if you like it.";
            rec.alternatives.clear();
        }
    }

    enforceLimits (rec);

    if (rec.changes.empty())
    {
        rec.outcome = Outcome::noChangeNeeded;
        rec.evidence = Evidence::weak;
        rec.headline = "No change suggested";
        rec.manualSteps.clear();
        if (rec.rationale.empty())
            rec.rationale = "The measurements do not show a clear reason to process for this goal.";
        if (rec.intensity < 0.05f)
            rec.rationale += " (Intensity is at its minimum.)";
    }
    else
    {
        rec.outcome = Outcome::suggested;
        rec.headline = rec.optional ? std::string (goalName (goal)) + ": an optional starting point"
                                    : (rec.changes.size() == 1 ? rec.changes.front().title : std::string (goalName (goal)) + ": a focused starting point");
    }
    return rec;
}
