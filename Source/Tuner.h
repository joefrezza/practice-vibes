#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "AudioTap.h"
#include <optional>

namespace pv
{

/** One pitch estimate. frequency == 0 means "no clear pitch" (silence, noise, chords). */
struct PitchReading
{
    float frequency = 0.0f;     // Hz
    float clarity = 0.0f;       // 0..1, height of the chosen autocorrelation peak
    float level = 0.0f;         // RMS of the analysed window

    bool isValid() const noexcept { return frequency > 0.0f; }
};

/** McLeod pitch method (normalised square difference function, computed with an FFT).
    Copes with guitar notes whose fundamental is weaker than their harmonics.
    Not thread-safe: one instance per analysing thread.
*/
class PitchDetector
{
public:
    static constexpr int windowSize = 4096;     // samples at the analysis rate (<= 48 kHz): ~85 ms
    static constexpr float minFrequency = 25.0f, maxFrequency = 1500.0f;
    static constexpr float minLevel = 0.0015f;  // RMS gate (about -56 dBFS)
    static constexpr float minClarity = 0.85f;

    PitchDetector();

    /** How many input samples analyse() wants at this sample rate (more is fine, the end is used). */
    static int samplesNeeded (double sampleRate) noexcept    { return windowSize * decimationFor (sampleRate); }

    /** Analyses the last samplesNeeded() samples of `samples`. */
    PitchReading analyse (const float* samples, int numSamples, double sampleRate);

    /** Rates above 48 kHz are averaged down by this factor first. */
    static int decimationFor (double sampleRate) noexcept    { return juce::jmax (1, (int) std::round (sampleRate / 48000.0)); }

private:
    juce::dsp::FFT fft;
    std::vector<float> window, spectrum, nsdf;
};

/** Where a frequency sits on the equal-tempered scale. */
struct NoteInfo
{
    int midiNote = -1;          // nearest note
    float cents = 0.0f;         // -50..+50 from that note
    juce::String name;          // "E", "C#"
    int octave = 0;             // scientific pitch notation (A4 = 440)

    static NoteInfo fromFrequency (float frequency, float referenceA4 = 440.0f);
};

/** Steadies the display: median of the last few readings of the same note, and a short hold
    over dropouts so the needle does not flicker between plucks. */
class PitchSmoother
{
public:
    static constexpr int historySize = 5;
    static constexpr double holdSeconds = 0.4;

    /** Returns the frequency to show (0 = nothing to show). */
    float add (const PitchReading&, double nowSeconds);
    void reset()                                            { count = 0; lastValidTime = -1.0e9; }

private:
    std::array<float, historySize> history {};
    int count = 0, next = 0;
    double lastValidTime = -1.0e9;
    float shown = 0.0f;
};

/** Six guitar strings, low to high, as MIDI notes. */
using StringNotes = std::array<int, 6>;

struct Tuning
{
    const char* name;
    StringNotes strings;
};

/** The built-in tunings; the first one is standard. */
const std::vector<Tuning>& tuningPresets();

/** Preset name for these strings, or "Custom". */
juce::String tuningName (const StringNotes&);

/** Frequency of a MIDI note for this A4 reference. */
float noteFrequency (int midiNote, float referenceA4) noexcept;

/** The string whose open note is closest to `frequency` (index 0 = low string), and how far
    away in cents (+ = sharp). -1 when no string is within maxCents. */
int nearestString (const StringNotes&, float frequency, float referenceA4, float& centsAway, float maxCents = 350.0f);

/** One string in a strum check. */
struct StringReading
{
    bool heard = false;
    float frequency = 0.0f;
    float cents = 0.0f;         // from the string's target note (+ = sharp)
};

using StrumResult = std::array<StringReading, 6>;

/** Polyphonic check of all six open strings at once: we know which notes to expect, so a long
    FFT is searched for a peak near each string's target (its 2nd harmonic if the fundamental is
    weak and the harmonic is not shared with another string). Not thread-safe.
*/
class StrumAnalyser
{
public:
    static constexpr double windowSeconds = 0.5;
    static constexpr float searchCents = 95.0f;         // how far from the target a string is looked for
    static constexpr float relativeFloorDb = -36.0f;    // a string quieter than this vs. the loudest is "not heard"
    static constexpr float minProminence = 6.0f;        // peak vs. the median of its band (noise reaches ~3)

    StrumAnalyser();

    static int samplesNeeded (double sampleRate) noexcept;

    /** Analyses the last samplesNeeded() samples of `samples`. Nothing is heard unless at
        least three strings are. */
    StrumResult analyse (const float* samples, int numSamples, double sampleRate,
                         const StringNotes&, float referenceA4);

private:
    struct Peak { float frequency = 0.0f, magnitude = 0.0f; };
    Peak findPeak (float target, double binHz) const;

    /** Best frequency for a partial near `start` in the band around `target`, given the other
        strings' partials (Hz) in that band. Uses the complex spectrum left by analyse(). */
    Peak fitPartial (int windowLength, double rate, float target, double start, const std::vector<double>& others) const;

    juce::dsp::FFT fft;
    std::vector<float> window, spectrum, magnitude;
};

/** Chromatic tuner: the guitar input stage pushes the dry signal into a ring buffer while the
    tuner is active; the UI calls read() on the message thread a few dozen times per second.
*/
class Tuner final : public AudioTap
{
public:
    Tuner();

    /** While false, push() ignores the audio (nothing to analyse, no cost). */
    std::atomic<bool> active { false };

    // Settings (message thread), saved in tuner.xml.
    float referenceA4 = 440.0f;             // 430..450
    bool muteWhileTuning = true;            // silence the guitar output while the tuner is open
    StringNotes strings = tuningPresets().front().strings;
    bool strobe = false;                    // strobe band instead of the needle
    bool strumMode = false;                 // check all six strings at once

    static constexpr float minReference = 430.0f, maxReference = 450.0f;
    static constexpr int minStringNote = 28, maxStringNote = 76;    // E1 .. E5 (baritone to high strings)

    void push (const float* samples, int numSamples, double sampleRate) noexcept override;

    /** Message thread: analyses the most recent audio. Invalid until enough audio has arrived. */
    PitchReading read();

    /** Message thread, strum mode: watches for a strum and, once enough of it has rung, returns
        the per-string result (empty otherwise). A new strum while waiting restarts the wait. */
    std::optional<StrumResult> readStrum();

    /** Message thread: true between a detected strum and its result. */
    bool isStrumPending() const noexcept    { return strumOnset >= 0; }

    /** Forget buffered audio (call when the tuner opens, so no stale note is shown). */
    void clear();

    static constexpr double strumSettleSeconds = 0.12;     // skip the pick attack

    std::unique_ptr<juce::XmlElement> toXml() const;
    void fromXml (const juce::XmlElement&);

private:
    static constexpr int ringSize = 1 << 17;     // > strum window + settle at 192 kHz (~119k)

    // The writer never laps the reader in practice: a read copies at most 16384 samples
    // and would have to be stalled for ~16k more samples to be overwritten.
    std::vector<float> ring;
    std::atomic<juce::int64> written { 0 };
    std::atomic<double> rate { 0.0 };
    juce::int64 clearedAt = 0;      // message thread: `written` when clear() was called

    // Strum onset detection (message thread): RMS of fixed chunks scanned since the last call.
    juce::int64 strumScanned = 0, strumOnset = -1;
    std::array<float, 32> chunkLevels {};     // ~170 ms of 256-sample chunks at 48 kHz
    int chunkCount = 0, chunkNext = 0;
    std::array<float, 4> chunkEnergies {};
    int energyCount = 0, energyNext = 0;
    StrumAnalyser strumAnalyser;

    std::vector<float> scratch;
    PitchDetector detector;
};

} // namespace pv
