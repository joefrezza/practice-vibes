#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <vector>

namespace pv
{

/** Result of the offline chord analysis of a song (all times in song seconds, i.e. at 1x speed). */
struct ChordTrack
{
    enum Quality { major, minor, dominant7, minor7, major7, power, numQualities };

    struct Chord
    {
        double start = 0.0, end = 0.0;
        int root = -1;                  // pitch class 0 = C ... 11 = B; -1 = no chord
        Quality quality = major;
        int bass = -1;                  // pitch class of a bass note other than the root (slash chord), else -1
        int startBeat = 0, numBeats = 0;
    };

    std::vector<Chord> chords;          // consecutive, covering the song
    std::vector<double> beats;          // beat times
    std::vector<int> beatPositions;     // per beat: position in its bar, 0 = downbeat
    std::vector<int> beatMeters;        // per beat: beats in its bar (the meter can change)
    int beatsPerBar = 4;                // main meter of the song
    int beatUnit = 4;                   // 4: beats are quarter notes; 8: eighth notes (5/8, 7/8)
    double bpm = 0.0;
    double tuningCents = 0.0;           // deviation of the recording from A = 440 Hz
    int keyRoot = 0;                    // estimated key, for sharp/flat spelling
    bool keyIsMinor = false;

    bool isDownbeat (int beatIndex) const
    {
        return beatIndex >= 0 && beatIndex < (int) beatPositions.size() && beatPositions[(size_t) beatIndex] == 0;
    }

    enum class GridUnit { beat, bar };

    /** Nearest beat or bar line to `seconds` (the song start counts as one). Returns `seconds` unchanged without beats. */
    double snapToGrid (double seconds, GridUnit) const;

    /** Closest grid line strictly after (direction > 0) or before (direction < 0) `seconds`, or -1 if there is none. */
    double neighbourGridLine (double seconds, GridUnit, int direction) const;

    /** Index of the chord playing at `seconds`, or -1. */
    int chordIndexAt (double seconds) const;

    /** Chord name, transposed by `semitones`, spelled with flats or sharps to suit the (transposed) key. */
    juce::String chordName (const Chord&, int semitones = 0) const;
    juce::String keyName (int semitones = 0) const;

    juce::String timeSignature() const      { return juce::String (beatsPerBar) + "/" + juce::String (beatUnit); }

    /** Time signature of the bar containing the given beat. */
    juce::String timeSignatureAt (int beatIndex) const
    {
        const auto meter = beatIndex >= 0 && beatIndex < (int) beatMeters.size() ? beatMeters[(size_t) beatIndex] : beatsPerBar;
        return juce::String (meter) + "/" + juce::String (beatUnit);
    }

    /** True if the meter changes somewhere in the song. */
    bool hasMeterChanges() const
    {
        return std::any_of (beatMeters.begin(), beatMeters.end(), [this] (int m) { return m != beatsPerBar; });
    }

    static juce::String noteName (int pitchClass, bool flats);
    static const char* qualitySuffix (Quality);
};

/** Offline chord recognition: tuning estimate, beat tracking, beat-synchronous chroma,
    chord-template matching and Viterbi smoothing that prefers changes on bar/half-bar boundaries.

    `mono` is the whole song. Runs in a few hundred milliseconds for a typical song;
    call from a background thread. `shouldCancel` (optional) is polled.
*/
ChordTrack analyseChords (const float* mono, int numSamples, double sampleRate,
                          const std::atomic<bool>* shouldCancel = nullptr);

} // namespace pv
