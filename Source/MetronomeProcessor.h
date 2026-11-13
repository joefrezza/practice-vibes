#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace pv
{

/** A sample-accurate click track with an optional "speed trainer":
    every N bars (or N minutes) the tempo rises by a step, up to a maximum.
    Tempo changes are applied on bar lines.

    Settings are atomics written by the UI; the audio thread publishes
    the current tempo and beat for display.
*/
class MetronomeProcessor final : public juce::AudioProcessor
{
public:
    MetronomeProcessor();

    enum class IntervalUnit { bars = 0, minutes = 1 };

    // Settings (UI -> audio)
    std::atomic<double> startBpm { 100.0 };
    std::atomic<int> beatsPerBar { 4 };
    std::atomic<bool> accentFirstBeat { true };
    std::atomic<float> gain { 0.7f };

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
    void onBeat (int beatInBar);

    double sampleRate = 48000.0;

    std::atomic<bool> running { false }, startRequested { false }, resetTempoRequested { false };
    std::atomic<double> currentBpm { 100.0 };
    std::atomic<int> currentBeat { -1 };

    // Audio-thread state
    double samplesUntilNextBeat = 0.0;
    int beatIndex = 0;                  // beat within bar of the NEXT click
    double bpm = 100.0;
    int barsSinceStep = 0;
    double secondsSinceStep = 0.0;

    // Click voice
    double clickPhase = 0.0, clickPhaseDelta = 0.0;
    float clickEnvelope = 0.0f, clickDecay = 0.999f, clickLevel = 1.0f;
    float lastGain = 0.7f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MetronomeProcessor)
};

} // namespace pv
