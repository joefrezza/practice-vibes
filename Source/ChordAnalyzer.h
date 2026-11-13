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
    int beatsPerBar = 4;
    double bpm = 0.0;
    double tuningCents = 0.0;           // deviation of the recording from A = 440 Hz
    int keyRoot = 0;                    // estimated key, for sharp/flat spelling
    bool keyIsMinor = false;

    bool isDownbeat (int beatIndex) const
    {
        return beatIndex >= 0 && beatIndex < (int) beatPositions.size() && beatPositions[(size_t) beatIndex] == 0;
    }

    /** Index of the chord playing at `seconds`, or -1. */
    int chordIndexAt (double seconds) const;

    /** Chord name, transposed by `semitones`, spelled with flats or sharps to suit the (transposed) key. */
    juce::String chordName (const Chord&, int semitones = 0) const;
    juce::String keyName (int semitones = 0) const;

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
