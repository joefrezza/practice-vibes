#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "MetronomeProcessor.h"

namespace pv
{

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

    //==============================================================================
    // Commands (message thread)
    void pressTrack (int track);        // the pedal button
    void toggleMute (int track);
    void clearTrack (int track);
    void clearAll();
    void undo();
    void setTrackGain (int track, float gain);

    // Status (any thread)
    State getState (int track) const noexcept      { return (State) status[(size_t) track].state.load(); }
    bool isMuted (int track) const noexcept        { return status[(size_t) track].muted.load(); }
    int getBars (int track) const noexcept         { return status[(size_t) track].bars.load(); }
    float getPosition (int track) const noexcept   { return status[(size_t) track].position.load(); }   // 0..1, -1 = n/a
    float getGain (int track) const noexcept       { return status[(size_t) track].gain.load(); }
    float takePeak (int track) noexcept            { return status[(size_t) track].peak.exchange (0.0f); }
    bool isEmpty() const noexcept                  { return emptyFlag.load(); }
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
        enum class Type { press, mute, clear, clearAll, undo } type = Type::press;
        int track = 0;
        bool countIn = false;
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
    int comp = 0;           // latency compensation, samples

    juce::AudioBuffer<float> dryHistory, wetHistory;   // the last ~1.5 s of input, indexed by graph time
    juce::AudioBuffer<float> mixBuffer;

    std::atomic<bool> emptyFlag { true }, undoAvailable { false };
    std::atomic<int> activeSource { (int) Source::afterFx }, compensation { 0 }, currentBar { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Looper)
};

} // namespace pv
