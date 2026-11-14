#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "ChordAnalyzer.h"
#include "PlayerProcessor.h"

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
    std::atomic<bool> clicksMuted { false };         // silent clicks, clock keeps running (e.g. drum plugin plays)

    std::atomic<bool> trainerEnabled { false };
    std::atomic<double> trainerStepBpm { 2.0 };
    std::atomic<double> trainerInterval { 1.0 };
    std::atomic<int> trainerUnit { (int) IntervalUnit::minutes };
    std::atomic<double> trainerMaxBpm { 160.0 };

    /** "Follow song": while the player plays, click on the song's detected beats (and bars)
        instead of a fixed tempo. Needs setPlayer() and a beat grid. */
    std::atomic<bool> followSong { false };
    void setPlayer (const PlayerProcessor* p)                   { player = p; }
    void setSongGrid (std::shared_ptr<const ChordTrack>);       // message thread; nullptr clears
    bool isFollowing() const noexcept                           { return following.load(); }

    /** Starts from startBpm, on beat 1. */
    void start();
    void stop();
    bool isRunning() const noexcept             { return running.load(); }

    /** Applies a startBpm change immediately while running (otherwise on next start). */
    void tempoChangedByUser()                   { resetTempoRequested = true; }

    //==============================================================================
    /** The bar clock. In the app it is advanced once per audio callback, before the graph runs
        (see PluginChain::setBlockHook), so other processors (the looper) can read this block's
        bar lines. Times are absolute sample counts of that shared clock. If nobody calls it,
        processBlock() advances it itself (offline tests).
    */
    void advanceClock (juce::int64 blockStart, int numSamples);

    // Audio thread only, valid after advanceClock().
    bool isClockRunning() const noexcept        { return clockRunning; }
    double getClockBarStart() const noexcept    { return barStart; }            // start of the current bar
    double getClockBarSamples() const noexcept  { return barBeats * beatSamples; }
    int getClockBeatsPerBar() const noexcept    { return barBeats; }
    double getClockStartTime() const noexcept   { return clockStartTime; }      // first downbeat after start()
    double getClockBeatSamples() const noexcept { return beatSamples; }
    double getClockBarPpq() const noexcept      { return barPpq; }              // beats from start() to barStart
    int getClockPreviousBarBeats() const noexcept { return previousBarBeats; }
    double getClockPreviousBeatSamples() const noexcept { return previousBeatSamples; }   // the bar before barStart

    /** Audio thread: while locked (the looper holds loops) tempo and bar length are frozen to this
        grid, follow mode and the trainer pause, and start() lines the clicks up with the grid. */
    void setLockedGrid (bool locked, double anchor, double barSamples, int beats);

    /** For the UI: true while the looper holds the tempo. */
    bool isTempoLocked() const noexcept         { return tempoLocked.load(); }

    // Status (audio -> UI)
    double getCurrentBpm() const noexcept       { return currentBpm.load(); }
    int getCurrentBeat() const noexcept         { return currentBeat.load(); }   // 0-based, -1 when stopped
    int getCurrentBeatsPerBar() const noexcept  { return currentBeatsPerBar.load(); }

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

    using Tick = std::pair<int, std::array<int, 3>>;    // (sample offset, {beatInBar, tick, barLength})

    void onTick (int beatInBar, int tickInBeat, int barLength);
    void onBarLine();
    void renderFollowingTicks (int numSamples, const ChordTrack&, std::vector<int>& tickOffsets);
    double followTickTime (const ChordTrack&, int beat, int tick, int ticks) const;
    void trigger (double frequency, double frequencyEnd, float toneLevel, double toneDecaySeconds,
                  float noiseLevel = 0.0f, double noiseDecaySeconds = 0.01, float highPass = 0.0f);
    float renderVoices();

    double sampleRate = 48000.0;

    std::atomic<bool> running { false }, startRequested { false }, resetTempoRequested { false };
    std::atomic<double> currentBpm { 100.0 };
    std::atomic<int> currentBeat { -1 }, currentBeatsPerBar { 4 };
    std::atomic<bool> following { false };

    const PlayerProcessor* player = nullptr;
    juce::SpinLock gridLock;
    std::shared_ptr<const ChordTrack> songGrid;

    // Follow-mode audio-thread state
    juce::int64 samplesRendered = 0;
    bool followLocked = false;
    double expectedSongTime = 0.0;
    int followBeat = 0, followTick = 0;     // the NEXT tick to play
    std::vector<Tick> pendingTicks;

    // Bar clock (audio thread). Tick times are computed from the bar start, so they never drift.
    bool clockRunning = false, clockAdvanced = false, resetVoices = false;
    juce::int64 nextBlockStart = 0;
    double barStart = 0.0, beatSamples = 24000.0, clockStartTime = 0.0;
    // For plugins' play head: beats counted from start() up to barStart, and the bar before it.
    double barPpq = 0.0, previousBeatSamples = 24000.0;
    int barBeats = 4, lastTicks = 1, previousBarBeats = 4;
    int nextBeat = 0, nextTick = 0;     // the NEXT tick within the current bar
    std::vector<Tick> clockTicks;
    double bpm = 100.0;
    int barsSinceStep = 0;
    double secondsSinceStep = 0.0;

    bool gridLocked = false;
    double lockedAnchor = 0.0, lockedBarSamples = 0.0;
    int lockedBeats = 4;
    std::atomic<bool> tempoLocked { false };

    std::array<Voice, 8> voices;
    size_t nextVoice = 0;
    juce::Random random;
    float lastGain = 0.7f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MetronomeProcessor)
};

} // namespace pv
