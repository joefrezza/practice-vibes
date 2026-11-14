#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "LatencyCalibrator.h"
#include "MetronomeProcessor.h"

namespace pv
{

struct LoopSet;

/** A 4-track looper locked to the metronome's bar grid.

    It sits at two places in the audio graph (two tiny nodes, see createBeforeFxNode() /
    createAfterFxNode()):
      - before the plugins: records the dry guitar; "before FX" loops are mixed back into the
        plugin chain input, so they are heard through the current rig (tone can change later);
      - after the plugins: records the processed guitar; "after FX" loops go straight to the output.
    The choice applies to the whole looper and can be changed while it is empty.

    Timing: everything is placed on the bar grid taken from the metronome when the first loop
    starts (tempo and bar length are then locked until the looper is cleared). Every track has a
    whole number of bars and plays from the shared grid, so tracks never drift apart. Recorded
    audio is shifted back by the round-trip latency (device in + out + plugins + a manual offset),
    so it lines up with what the player heard while playing.

    Pedal-style control, one button per track: empty -> record -> play -> overdub -> play...
    A first recording starts on the nearest bar line (slightly late presses start retroactively),
    and stops after a fixed number of bars or, in free mode, on the bar line nearest to the press.
    Overdubs start and stop immediately. The last overdub can be undone.

    Commands come from the message thread (lock-free queue); all audio state lives on the
    audio thread; status is published through atomics.
*/
class Looper final
{
public:
    static constexpr int numTracks = 4;
    static constexpr double maxLoopSeconds = 60.0;

    enum class Source { afterFx = 0, beforeFx = 1 };
    enum class State  { empty = 0, armed, recording, playing, overdubbing };

    explicit Looper (MetronomeProcessor&);
    ~Looper();

    // Settings (UI -> audio)
    std::atomic<int> source { (int) Source::afterFx };   // used when the looper is empty
    std::atomic<int> fixedBars { 0 };                     // 0 = free length
    std::atomic<float> latencyOffsetMs { 0.0f };          // manual fine-tune, added to the measured latency
    std::atomic<int> deviceLatencySamples { 0 };          // device input + output latency
    std::atomic<int> chainLatencySamples { 0 };           // plugin latency

    /** Measures the round trip on the dry input while active (see LatencyCalibrator). */
    LatencyCalibrator calibrator;

    //==============================================================================
    // Commands (message thread)
    /** The pedal button. `pressSeconds` (Time::getMillisecondCounterHiRes() / 1000, e.g. a MIDI
        timestamp) dates the press, so the time it took to reach the audio thread doesn't count;
        0 = now. */
    void pressTrack (int track, double pressSeconds = 0.0);
    void toggleMute (int track);
    void clearTrack (int track);
    void clearAll();
    void undo();
    void toggleMuteAll();               // mutes every playing track, or unmutes them all if all are muted
    void setTrackGain (int track, float gain);

    /** Copies the loops, each rotated to start on the same bar line (so they stay aligned in a
        DAW and when loaded again). Fails while a track records or overdubs. Blocks briefly. */
    bool copyLoops (LoopSet&, juce::String& error);

    /** Loads loops into the (empty) looper: sets the metronome to the set's tempo and meter and
        (re)starts it; the loops play from its first downbeat. */
    bool loadLoops (const LoopSet&, juce::String& error);

    // Status (any thread)
    State getState (int track) const noexcept      { return (State) status[(size_t) track].state.load(); }
    bool isMuted (int track) const noexcept        { return status[(size_t) track].muted.load(); }
    int getBars (int track) const noexcept         { return status[(size_t) track].bars.load(); }
    float getPosition (int track) const noexcept   { return status[(size_t) track].position.load(); }   // 0..1, -1 = n/a
    float getGain (int track) const noexcept       { return status[(size_t) track].gain.load(); }
    float takePeak (int track) noexcept            { return status[(size_t) track].peak.exchange (0.0f); }
    bool isEmpty() const noexcept                  { return emptyFlag.load(); }
    bool isLoading() const noexcept                { return loadPendingFlag.load(); }   // loaded loops wait for the metronome
    bool canUndo() const noexcept                  { return undoAvailable.load(); }
    Source getActiveSource() const noexcept        { return (Source) activeSource.load(); }
    int getCompensationSamples() const noexcept    { return compensation.load(); }
    double getSampleRate() const noexcept          { return sampleRateAtomic.load(); }
    int getCurrentBar() const noexcept             { return currentBar.load(); }   // 0-based bar of the loop grid, -1 = none

    //==============================================================================
    // Audio thread
    void prepare (double sampleRate, int maximumBlockSize);

    /** Called once per audio callback, after MetronomeProcessor::advanceClock(), before the graph. */
    void beginBlock (juce::int64 blockStart, int numSamples);

    /** The dry guitar (mono) in, dry guitar + "before FX" loops out. */
    void processBeforeFx (float* mono, int numSamples);

    /** The processed guitar in (recorded if needed); "after FX" loops out (replaces the input). */
    void processAfterFx (float* left, float* right, int numSamples);

    std::unique_ptr<juce::AudioProcessor> createBeforeFxNode();
    std::unique_ptr<juce::AudioProcessor> createAfterFxNode();

private:
    struct Command
    {
        enum class Type { press, mute, clear, clearAll, undo, load } type = Type::press;
        int track = 0;
        bool countIn = false;
        double pressSeconds = 0.0;      // wall-clock time of the press, 0 = when handled
    };

    struct Status
    {
        std::atomic<int> state { (int) State::empty };
        std::atomic<bool> muted { false };
        std::atomic<int> bars { 0 };
        std::atomic<float> position { -1.0f }, gain { 1.0f }, peak { 0.0f };
    };

    struct Track
    {
        juce::AudioBuffer<float> audio;     // allocated on the message thread while the track is empty
        State state = State::empty;
        bool muted = false;
        double origin = 0.0;                // grid time (samples) of the loop's first sample
        double period = 0.0;                // loop length in samples (0 = still unknown)
        int bars = 0;
        // Recording windows in "musical" time (graph time minus the latency compensation).
        double baseStart = 0.0, baseEnd = 0.0;      // first recording, into `audio`
        double dubStart = 0.0, dubEnd = 0.0;        // overdub, into `layer`
        double playStart = 0.0;             // graph time from which the loop plays
        bool countIn = false;               // first recording waits one bar after the metronome starts
        bool backfill = false;              // first recording started in the past: copy it from the history
        bool firstPass = false;             // base window still open or pending
        int layer = -1;                     // layer receiving the overdub
        float lastGain = 1.0f;
    };

    /** Overdubs are recorded into a separate layer so the last one can be undone; when another
        overdub starts, the previous layer is merged into its track a chunk per block. */
    struct Layer
    {
        enum class Status { free, active, undoable, merging, discarding };
        juce::AudioBuffer<float> audio;
        Status status = Status::free;
        int owner = -1;
        int progress = 0;
    };

    void push (Command);
    bool handleFreeze();
    void resolveLoad();
    void handle (const Command&);
    void press (int track, bool countIn);
    void finishActive (int exceptTrack);
    void finishFirstRecording (Track&, double musicalTime);
    void startOverdub (int track);
    void stopOverdub (Track&);
    void clear (int track);
    int acquireLayer();
    void advanceLayers (int maxSamples, bool finishAll = false);
    bool hasGrid() const noexcept                  { return gridBarSamples > 0.0; }
    void resolveArmed (Track&);
    void updateGridAndStatus();
    int trackLength (const Track&) const;
    void ensureAllocated (juce::AudioBuffer<float>&, int channels);

    void capture (Track&, const float* const* input, int numChannels, int numSamples);
    void backfillFromHistory (Track&, const juce::AudioBuffer<float>& history);
    void play (Track&, Status&, float* left, float* right, int numSamples);
    double musicalNow() const noexcept             { return (double) (now - comp); }
    double musicalTimeOf (double pressSeconds) const noexcept;
    void backfillLayer (Track&, double from, double to, float sign);
    int maxTrackSamples() const noexcept;
    static void writeHistory (juce::AudioBuffer<float>&, int channel, const float*, juce::int64 graphStart, int numSamples);

    MetronomeProcessor& metronome;

    std::array<Track, numTracks> tracks;
    std::array<Status, numTracks> status;
    std::array<Layer, 2> layers;
    int undoLayer = -1;

    juce::AbstractFifo fifo { 64 };
    std::array<Command, 64> commands;
    juce::SpinLock pushLock;

    double sampleRate = 48000.0;
    std::atomic<double> sampleRateAtomic { 48000.0 };
    int maxBlock = 512;

    // Grid (audio thread): anchor = any bar line, bar length in samples.
    double gridAnchor = 0.0, gridBarSamples = 0.0;
    int gridBeats = 4;
    int lockedSource = (int) Source::afterFx;

    juce::int64 now = 0;    // graph time of the current block's first sample
    int blockLength = 0;
    double blockWallSeconds = 0.0;  // wall clock when this block started (maps press times to graph time)
    double eventTime = 0.0;         // musical time of the command being handled (a dated press may be in the past)
    int comp = 0;           // latency compensation, samples

    juce::AudioBuffer<float> dryHistory, wetHistory;   // the last ~1.5 s of input, indexed by graph time
    juce::AudioBuffer<float> mixBuffer;

    // Copying loops: the message thread asks, the audio thread stops changing the buffers and answers.
    struct FrozenTrack { State state = State::empty; double origin = 0.0, period = 0.0; int bars = 0; bool muted = false; };
    struct Frozen { double anchor = 0.0, barSamples = 0.0; int beats = 4, source = 0; std::array<FrozenTrack, numTracks> tracks; };
    std::atomic<int> freezeRequest { 0 }, freezeAck { 0 };
    int freezeCounter = 0, handledFreeze = 0;
    bool frozenNow = false;
    Frozen frozen;

    // Loading loops: buffers are filled on the message thread while the tracks are empty;
    // the audio thread starts them on the metronome's next start.
    struct PendingLoad { std::array<int, numTracks> bars {}; std::array<bool, numTracks> muted {}; int source = 0; };
    PendingLoad pendingLoad;
    bool loadWaiting = false;
    juce::int64 loadRequestedAt = 0;

    std::atomic<bool> emptyFlag { true }, undoAvailable { false }, loadPendingFlag { false };
    std::atomic<int> activeSource { (int) Source::afterFx }, compensation { 0 }, currentBar { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Looper)
};

//==============================================================================
/** A copy of the looper's loops (see Looper::copyLoops / loadLoops, LoopFiles). */
struct LoopSet
{
    struct Loop
    {
        juce::AudioBuffer<float> audio;     // starts on a bar line shared by all loops; 1 channel = before FX
        int bars = 0;                       // 0 = unused track
        bool muted = false;
        float gain = 1.0f;
    };

    double sampleRate = 48000.0, barSamples = 0.0;
    int beatsPerBar = 4;
    Looper::Source source = Looper::Source::afterFx;
    std::array<Loop, Looper::numTracks> loops;

    double getBpm() const       { return barSamples > 0.0 ? 60.0 * sampleRate * beatsPerBar / barSamples : 0.0; }
    int getNumLoops() const     { int n = 0; for (auto& l : loops) n += l.bars > 0 ? 1 : 0; return n; }
};

} // namespace pv
