#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace pv
{

/** A sample-accurate click track with subdivisions, selectable (synthesised) sounds
    and an optional "speed trainer": every N bars (or N minutes) the tempo rises by a
    step, up to a maximum. Tempo changes are applied on bar lines.

    Settings are atomics written by the UI; the audio thread publishes
    the current tempo and beat for display.
*/
class MetronomeProcessor final : public juce::AudioProcessor
{
public:
    MetronomeProcessor();

    enum class IntervalUnit { bars = 0, minutes = 1 };
    enum class Subdivision  { quarters = 0, eighths = 1, triplets = 2, sixteenths = 3 };
    enum class Sound        { beep = 0, click = 1, drums = 2 };

    static int ticksPerBeat (Subdivision s)     { return s == Subdivision::eighths ? 2 : s == Subdivision::triplets ? 3
                                                       : s == Subdivision::sixteenths ? 4 : 1; }

    // Settings (UI -> audio)
    std::atomic<double> startBpm { 100.0 };
    std::atomic<int> beatsPerBar { 4 };
    std::atomic<bool> accentFirstBeat { true };
    std::atomic<float> gain { 0.7f };
    std::atomic<int> subdivision { (int) Subdivision::quarters };
    std::atomic<int> sound { (int) Sound::beep };
    std::atomic<float> subdivisionLevel { 0.45f };   // relative to a normal beat

    std::atomic<bool> trainerEnabled { false };
    std::atomic<double> trainerStepBpm { 2.0 };
    std::atomic<double> trainerInterval { 1.0 };
    std::atomic<int> trainerUnit { (int) IntervalUnit::minutes };
    std::atomic<double> trainerMaxBpm { 160.0 };

    /** Starts from startBpm, on beat 1. */
    void start();
    void stop();
    bool isRunning() const noexcept             { return running.load(); }

    /** Applies a startBpm change immediately while running (otherwise on next start). */
    void tempoChangedByUser()                   { resetTempoRequested = true; }

    // Status (audio -> UI)
    double getCurrentBpm() const noexcept       { return currentBpm.load(); }
    int getCurrentBeat() const noexcept         { return currentBeat.load(); }   // 0-based, -1 when stopped

    //==============================================================================
    const juce::String getName() const override { return "Metronome"; }
    void prepareToPlay (double sampleRate, int maximumBlockSize) override;
    void releaseResources() override            {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    bool isBusesLayoutSupported (const BusesLayout& layout) const override
    {
        return layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    double getTailLengthSeconds() const override                { return 0.0; }
    bool acceptsMidi() const override                           { return false; }
    bool producesMidi() const override                          { return false; }
    bool hasEditor() const override                             { return false; }
    juce::AudioProcessorEditor* createEditor() override         { return nullptr; }
    int getNumPrograms() override                               { return 1; }
    int getCurrentProgram() override                            { return 0; }
    void setCurrentProgram (int) override                       {}
    const juce::String getProgramName (int) override            { return {}; }
    void changeProgramName (int, const juce::String&) override  {}
    void getStateInformation (juce::MemoryBlock&) override      {}
    void setStateInformation (const void*, int) override        {}

private:
    /** One synthesised hit: a (pitch-swept) sine, filtered noise, or both. */
    struct Voice
    {
        double phase = 0.0, frequency = 0.0, frequencyEnd = 0.0, sweep = 1.0;
        float toneLevel = 0.0f, toneEnvelope = 0.0f, toneDecay = 0.0f;
        float noiseLevel = 0.0f, noiseEnvelope = 0.0f, noiseDecay = 0.0f;
        float highPassAmount = 0.0f, lastNoise = 0.0f;   // crude high-pass for hats/snare
        bool active = false;
    };

    void onTick (int beatInBar, int tickInBeat);
    void trigger (double frequency, double frequencyEnd, float toneLevel, double toneDecaySeconds,
                  float noiseLevel = 0.0f, double noiseDecaySeconds = 0.01, float highPass = 0.0f);
    float renderVoices();

    double sampleRate = 48000.0;

    std::atomic<bool> running { false }, startRequested { false }, resetTempoRequested { false };
    std::atomic<double> currentBpm { 100.0 };
    std::atomic<int> currentBeat { -1 };

    // Audio-thread state
    double samplesUntilNextTick = 0.0;
    int beatIndex = 0, tickIndex = 0;   // position of the NEXT tick
    double bpm = 100.0;
    int barsSinceStep = 0;
    double secondsSinceStep = 0.0;

    std::array<Voice, 8> voices;
    size_t nextVoice = 0;
    juce::Random random;
    float lastGain = 0.7f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MetronomeProcessor)
};

} // namespace pv
