#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "AudioTap.h"

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

    static constexpr float minReference = 430.0f, maxReference = 450.0f;

    void push (const float* samples, int numSamples, double sampleRate) noexcept override;

    /** Message thread: analyses the most recent audio. Invalid until enough audio has arrived. */
    PitchReading read();

    /** Forget buffered audio (call when the tuner opens, so no stale note is shown). */
    void clear();

    std::unique_ptr<juce::XmlElement> toXml() const;
    void fromXml (const juce::XmlElement&);

private:
    static constexpr int ringSize = 1 << 15;     // > samplesNeeded() at 192 kHz (16384)

    // The writer never laps the reader in practice: a read copies at most 16384 samples
    // and would have to be stalled for ~16k more samples to be overwritten.
    std::vector<float> ring;
    std::atomic<juce::int64> written { 0 };
    std::atomic<double> rate { 0.0 };
    juce::int64 clearedAt = 0;      // message thread: `written` when clear() was called

    std::vector<float> scratch;
    PitchDetector detector;
};

} // namespace pv
