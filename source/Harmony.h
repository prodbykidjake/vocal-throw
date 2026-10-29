#pragma once

#include <juce_core/juce_core.h>
#include <array>

// Scale maths for Harmony mode: which note sits a 3rd or 5th away from the sung note, inside the chosen key.
namespace Harmony
{
    inline constexpr std::array<int, 7> majorScale { 0, 2, 4, 5, 7, 9, 11 };
    inline constexpr std::array<int, 7> minorScale { 0, 2, 3, 5, 7, 8, 10 };

    // The interval choices, in the order the Interval parameter lists them.
    enum class Interval
    {
        thirdUp,
        fifthUp,
        thirdAndFifth,
        thirdDown
    };

    inline constexpr int maxVoices = 2;

    // How many scale steps each harmony copy sits from the voice (2 = a 3rd, 4 = a 5th), and how many copies there are.
    struct Steps
    {
        std::array<int, maxVoices> steps {};
        int count = 1;
    };

    inline Steps stepsFor (Interval interval)
    {
        switch (interval)
        {
            case Interval::fifthUp:       return { { 4, 0 }, 1 };
            case Interval::thirdAndFifth: return { { 2, 4 }, 2 };
            case Interval::thirdDown:     return { { -2, 0 }, 1 };
            case Interval::thirdUp:
            default:                      return { { 2, 0 }, 1 };
        }
    }

    inline float midiFromHz (float hz)
    {
        return 69.0f + 12.0f * std::log2 (hz / 440.0f);
    }

    inline juce::String noteName (int midiNote)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[((midiNote % 12) + 12) % 12]) + juce::String (midiNote / 12 - 1);
    }

    // Semitones from the sung note to the note `steps` scale degrees away, in `key` (0 = C) major or minor.
    // A sung note outside the scale is treated as the nearest scale note, so the harmony stays in key.
    inline int semitonesFor (float sungMidi, int key, bool minor, int steps)
    {
        const auto& scale = minor ? minorScale : majorScale;
        const auto sung = juce::roundToInt (sungMidi);
        const auto pitchClass = ((sung - key) % 12 + 12) % 12;

        // nearest scale degree, the lower one on a tie
        int degree = 0;
        int bestDistance = 12;

        for (int d = 0; d < 7; ++d)
        {
            const auto distance = std::abs (scale[static_cast<size_t> (d)] - pitchClass);

            if (distance < bestDistance)
            {
                bestDistance = distance;
                degree = d;
            }
        }

        const auto target = degree + steps;
        const auto octaves = target >= 0 ? target / 7 : (target - 6) / 7;
        const auto targetDegree = target - octaves * 7;

        return scale[static_cast<size_t> (targetDegree)] + 12 * octaves - scale[static_cast<size_t> (degree)];
    }
}
