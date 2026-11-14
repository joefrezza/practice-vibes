#include "Looper.h"

namespace pv
{

namespace
{
    constexpr double never = 1.0e300;
    constexpr double historySeconds = 1.5;
    constexpr double retroSeconds = 0.4;        // a press this late after a bar line still starts on it
    constexpr int mergeChunk = 65536;           // samples per block when merging/discarding an overdub layer
    constexpr double maxPressAge = 0.5;         // a dated press is moved back at most this far (s)

    /** A graph node that hands its audio to one side of the looper. */
    class LooperNode final : public juce::AudioProcessor
    {
    public:
        LooperNode (Looper& l, bool beforeFxIn)
            : AudioProcessor (BusesProperties()
                                  .withInput  ("Input",  beforeFxIn ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo(), true)
                                  .withOutput ("Output", beforeFxIn ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo(), true)),
              looper (l), beforeFx (beforeFxIn)
        {
        }

        const juce::String getName() const override                 { return beforeFx ? "Looper (before FX)" : "Looper (after FX)"; }
        void prepareToPlay (double rate, int maxBlock) override     { looper.prepare (rate, maxBlock); }
        void releaseResources() override                            {}

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            if (beforeFx)
                looper.processBeforeFx (buffer.getWritePointer (0), buffer.getNumSamples());
            else
                looper.processAfterFx (buffer.getWritePointer (0), buffer.getWritePointer (buffer.getNumChannels() > 1 ? 1 : 0),
                                       buffer.getNumSamples());
        }

        using AudioProcessor::processBlock;

        bool isBusesLayoutSupported (const BusesLayout& layout) const override
        {
            const auto wanted = beforeFx ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
            return layout.getMainInputChannelSet() == wanted && layout.getMainOutputChannelSet() == wanted;
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
        Looper& looper;
        const bool beforeFx;
    };

    int positiveIndex (double position, double period)
    {
        const auto wrapped = std::fmod (position, period);
        return (int) (wrapped < 0.0 ? wrapped + period : wrapped);
    }
}

//==============================================================================
Looper::Looper (MetronomeProcessor& m) : metronome (m) {}
Looper::~Looper() = default;

std::unique_ptr<juce::AudioProcessor> Looper::createBeforeFxNode()  { return std::make_unique<LooperNode> (*this, true); }
std::unique_ptr<juce::AudioProcessor> Looper::createAfterFxNode()   { return std::make_unique<LooperNode> (*this, false); }

//==============================================================================
// Message thread

void Looper::ensureAllocated (juce::AudioBuffer<float>& buffer, int channels)
{
    const auto needed = (int) std::ceil (maxLoopSeconds * sampleRateAtomic.load()) + 2;

    if (buffer.getNumSamples() < needed || buffer.getNumChannels() < channels)
        buffer.setSize (channels, needed, false, true, false);
}

void Looper::push (Command command)
{
    const juce::SpinLock::ScopedLockType lock (pushLock);
    const auto scope = fifo.write (1);

    if (scope.blockSize1 > 0)
        commands[(size_t) scope.startIndex1] = command;
}

void Looper::pressTrack (int track, double pressSeconds)
{
    if (! juce::isPositiveAndBelow (track, numTracks))
        return;

    const auto state = getState (track);
    Command command { Command::Type::press, track, false, pressSeconds };

    // Buffers are allocated here, never on the audio thread. An empty track's buffer and the
    // overdub layers are untouched by the audio thread until the command below arrives.
    if (state == State::empty)
    {
        ensureAllocated (tracks[(size_t) track].audio, 2);

        if (isEmpty() && ! metronome.isRunning())
        {
            metronome.start();          // one bar of count-in, then recording starts
            command.countIn = true;
        }
    }
    else if (state == State::playing)
    {
        // Allocated once, on the first overdub (the audio thread never resizes them).
        for (auto& layer : layers)
            if (layer.audio.getNumSamples() == 0)
                ensureAllocated (layer.audio, 2);
    }

    push (command);
}

void Looper::toggleMute (int track)  { push ({ Command::Type::mute, track }); }
void Looper::clearTrack (int track)  { push ({ Command::Type::clear, track }); }
void Looper::clearAll()              { push ({ Command::Type::clearAll, 0 }); }
void Looper::undo()                  { push ({ Command::Type::undo, 0 }); }

void Looper::toggleMuteAll()
{
    bool anyPlaying = false;

    for (int t = 0; t < numTracks; ++t)
        anyPlaying = anyPlaying || (getState (t) == State::playing && ! isMuted (t));

    for (int t = 0; t < numTracks; ++t)
        if (getState (t) == State::playing && isMuted (t) != anyPlaying)
            toggleMute (t);
}

void Looper::setTrackGain (int track, float gain)
{
    if (juce::isPositiveAndBelow (track, numTracks))
        status[(size_t) track].gain = juce::jmax (0.0f, gain);
}

bool Looper::copyLoops (LoopSet& set, juce::String& error)
{
    if (isEmpty())
    {
        error = "There are no loops to save.";
        return false;
    }

    const auto id = ++freezeCounter;
    freezeRequest.store (id, std::memory_order_release);
    const auto deadline = juce::Time::getMillisecondCounter() + 1000;
    int ack = 0;

    while ((ack = freezeAck.load (std::memory_order_acquire)) != id && ack != -id)
    {
        if (juce::Time::getMillisecondCounter() > deadline)
        {
            freezeRequest = 0;
            error = "The audio device is not running.";
            return false;
        }

        juce::Thread::sleep (2);
    }

    if (ack == -id)
    {
        freezeRequest = 0;
        error = "Finish recording or overdubbing first.";
        return false;
    }

    // Frozen: the audio thread only reads the buffers until freezeRequest goes back to 0.
    set.sampleRate = sampleRateAtomic.load();
    set.barSamples = frozen.barSamples;
    set.beatsPerBar = frozen.beats;
    set.source = (Source) frozen.source;
    const auto channels = set.source == Source::beforeFx ? 1 : 2;

    for (int t = 0; t < numTracks; ++t)
    {
        const auto& f = frozen.tracks[(size_t) t];
        auto& loop = set.loops[(size_t) t];
        loop.audio.setSize (0, 0);
        loop.bars = 0;
        loop.muted = f.muted;
        loop.gain = status[(size_t) t].gain.load();

        if (f.state != State::playing || f.period <= 0.0)
            continue;

        const auto& track = tracks[(size_t) t];
        const auto length = juce::jmin ((int) std::round (f.period), track.audio.getNumSamples());
        const auto offset = positiveIndex (frozen.anchor - f.origin, f.period);   // where the shared bar line falls
        loop.audio.setSize (channels, length);
        loop.bars = f.bars;

        for (int c = 0; c < channels; ++c)
        {
            auto* dest = loop.audio.getWritePointer (c);

            for (int i = 0; i < length; ++i)
                dest[i] = track.audio.getSample (c, (offset + i) % length);

            // Overdubs not merged into the track yet (the undoable one, one being merged)
            for (auto& layer : layers)
                if (layer.owner == t && (layer.status == Layer::Status::undoable || layer.status == Layer::Status::merging))
                    for (int i = 0; i < length; ++i)
                        dest[i] += layer.audio.getSample (c, (offset + i) % length);
        }
    }

    freezeRequest.store (0, std::memory_order_release);
    return true;
}

bool Looper::loadLoops (const LoopSet& set, juce::String& error)
{
    if (! isEmpty() || isLoading())
    {
        error = "Clear all loops first.";
        return false;
    }

    const auto bpm = set.getBpm();

    if (set.getNumLoops() == 0 || bpm < 20.0 || bpm > 400.0)
    {
        error = "These loops can't be used (no loops, or tempo out of range).";
        return false;
    }

    const auto rate = sampleRateAtomic.load();
    const auto ratio = set.sampleRate / rate;       // input samples per output sample
    const auto barSamplesHere = set.barSamples / ratio;

    for (auto& loop : set.loops)
    {
        if (loop.bars * barSamplesHere > maxTrackSamples())
        {
            error = "A loop is longer than " + juce::String ((int) maxLoopSeconds) + " seconds.";
            return false;
        }
    }

    // The tracks are empty, so the audio thread doesn't touch their buffers.
    for (int t = 0; t < numTracks; ++t)
    {
        const auto& loop = set.loops[(size_t) t];
        const auto used = loop.bars > 0 && loop.audio.getNumSamples() > 0 && loop.audio.getNumChannels() > 0;
        pendingLoad.bars[(size_t) t] = used ? loop.bars : 0;
        pendingLoad.muted[(size_t) t] = loop.muted;

        if (! used)
            continue;

        auto& track = tracks[(size_t) t];
        ensureAllocated (track.audio, 2);
        track.audio.clear();
        const auto inLength = loop.audio.getNumSamples();
        const auto outLength = juce::jmin (track.audio.getNumSamples(), (int) std::round (inLength / ratio));

        for (int c = 0; c < 2; ++c)
        {
            const auto* in = loop.audio.getReadPointer (juce::jmin (c, loop.audio.getNumChannels() - 1));

            if (std::abs (ratio - 1.0) < 1.0e-9)
            {
                track.audio.copyFrom (c, 0, in, juce::jmin (inLength, outLength));
            }
            else
            {
                juce::LagrangeInterpolator interpolator;
                interpolator.process (ratio, in, track.audio.getWritePointer (c), outLength, inLength, inLength);
            }
        }

        setTrackGain (t, loop.gain);
    }

    pendingLoad.source = (int) set.source;
    source = (int) set.source;
    metronome.startBpm = bpm;
    metronome.beatsPerBar = set.beatsPerBar;
    loadPendingFlag = true;
    push ({ Command::Type::load, 0 });
    return true;
}

//==============================================================================
// Audio thread

bool Looper::handleFreeze()
{
    const auto request = freezeRequest.load (std::memory_order_acquire);

    if (request == 0)
    {
        frozenNow = false;
        return false;
    }

    if (request != handledFreeze)
    {
        handledFreeze = request;
        auto busy = loadWaiting;

        for (auto& track : tracks)
            busy = busy || track.state == State::armed || track.state == State::recording
                        || track.state == State::overdubbing || track.firstPass;

        for (auto& layer : layers)
            busy = busy || layer.status == Layer::Status::active;

        frozen.anchor = gridAnchor;
        frozen.barSamples = gridBarSamples;
        frozen.beats = gridBeats;
        frozen.source = lockedSource;

        for (size_t t = 0; t < tracks.size(); ++t)
            frozen.tracks[t] = { tracks[t].state, tracks[t].origin, tracks[t].period, tracks[t].bars, tracks[t].muted };

        frozenNow = ! busy;
        freezeAck.store (busy ? -request : request, std::memory_order_release);
    }

    return frozenNow;
}

void Looper::resolveLoad()
{
    if (! loadWaiting || ! metronome.isClockRunning() || metronome.getClockStartTime() <= (double) loadRequestedAt)
        return;

    loadWaiting = false;
    gridAnchor = metronome.getClockStartTime();
    gridBarSamples = metronome.getClockBarSamples();
    gridBeats = metronome.getClockBeatsPerBar();
    lockedSource = pendingLoad.source;

    for (int t = 0; t < numTracks; ++t)
    {
        auto& track = tracks[(size_t) t];
        const auto bars = pendingLoad.bars[(size_t) t];

        if (bars <= 0 || track.state != State::empty)
            continue;

        clear (t);
        track.state = State::playing;
        track.muted = pendingLoad.muted[(size_t) t];
        track.lastGain = track.muted ? 0.0f : status[(size_t) t].gain.load();   // no fade-in from the old volume
        track.bars = bars;
        track.period = bars * gridBarSamples;
        track.origin = gridAnchor;
        track.baseStart = track.baseEnd = gridAnchor;
        track.playStart = gridAnchor;
    }

    loadPendingFlag = false;
}

void Looper::prepare (double newSampleRate, int maximumBlockSize)
{
    maxBlock = juce::jmax (maxBlock, maximumBlockSize);

    if (newSampleRate > 0 && newSampleRate != sampleRate)
    {
        // Loops recorded at another rate would play at the wrong speed: start over.
        for (int t = 0; t < numTracks; ++t)
            clear (t);

        for (auto& layer : layers)
        {
            layer.status = Layer::Status::free;
            layer.owner = -1;
            layer.progress = 0;
            layer.audio.clear();
        }

        undoLayer = -1;
        sampleRate = newSampleRate;
        sampleRateAtomic = newSampleRate;
        updateGridAndStatus();
    }

    calibrator.prepare (sampleRate);
    const auto historyLength = (int) std::ceil (historySeconds * sampleRate);

    if (dryHistory.getNumSamples() != historyLength)
    {
        dryHistory.setSize (1, historyLength);
        wetHistory.setSize (2, historyLength);
        dryHistory.clear();
        wetHistory.clear();
    }
}

int Looper::maxTrackSamples() const noexcept
{
    return (int) std::floor (maxLoopSeconds * sampleRate);
}

int Looper::trackLength (const Track& track) const
{
    if (track.period <= 0.0)
        return 0;

    return juce::jmin (track.audio.getNumSamples(), (int) std::ceil (track.period) + 1);
}

void Looper::beginBlock (juce::int64 blockStart, int numSamples)
{
    now = blockStart;
    blockLength = numSamples;
    blockWallSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
    eventTime = musicalNow();

    const auto offset = (double) latencyOffsetMs.load() * sampleRate / 1000.0;
    comp = juce::jmax (0, deviceLatencySamples.load() + chainLatencySamples.load() + juce::roundToInt (offset));
    compensation = comp;

    if (handleFreeze())
    {
        // The message thread is copying the loops: nothing may change them (commands wait).
        updateGridAndStatus();
        return;
    }

    {
        const auto scope = fifo.read (fifo.getNumReady());

        for (int i = 0; i < scope.blockSize1; ++i)  handle (commands[(size_t) (scope.startIndex1 + i)]);
        for (int i = 0; i < scope.blockSize2; ++i)  handle (commands[(size_t) (scope.startIndex2 + i)]);
    }

    eventTime = musicalNow();
    resolveLoad();

    const auto m = musicalNow();

    for (auto& track : tracks)
    {
        if (track.state == State::armed)
            resolveArmed (track);

        if (track.state == State::recording)
        {
            if (track.period > 0.0 && (double) now >= track.playStart)
                track.state = State::playing;                       // fixed length reached
            else if (track.period <= 0.0 && m - track.origin >= maxTrackSamples() - gridBarSamples)
                finishFirstRecording (track, m);                    // free recording hit the maximum length
        }

        if (track.firstPass && m >= track.baseEnd)
            track.firstPass = false;
    }

    advanceLayers (mergeChunk);
    updateGridAndStatus();
}

void Looper::handle (const Command& command)
{
    eventTime = command.pressSeconds > 0.0 ? musicalTimeOf (command.pressSeconds) : musicalNow();

    if (command.type == Command::Type::clearAll)
    {
        for (int t = 0; t < numTracks; ++t)
            clear (t);

        return;
    }

    if (command.type == Command::Type::load)
    {
        // The loops start on the metronome's first downbeat after this (re)start.
        loadWaiting = true;
        loadRequestedAt = now;
        metronome.start();
        return;
    }

    if (command.type == Command::Type::undo)
    {
        for (auto& track : tracks)
            if (track.state == State::overdubbing)
                stopOverdub (track);

        if (undoLayer >= 0)
        {
            layers[(size_t) undoLayer].status = Layer::Status::discarding;
            undoLayer = -1;
        }

        return;
    }

    if (! juce::isPositiveAndBelow (command.track, numTracks))
        return;

    auto& track = tracks[(size_t) command.track];

    switch (command.type)
    {
        case Command::Type::press:  press (command.track, command.countIn); break;
        case Command::Type::clear:  clear (command.track); break;

        case Command::Type::mute:
            if (track.state != State::empty)
                track.muted = ! track.muted;
            break;

        case Command::Type::clearAll:
        case Command::Type::undo:
        case Command::Type::load:
            break;
    }
}

void Looper::finishActive (int exceptTrack)
{
    for (int t = 0; t < numTracks; ++t)
    {
        if (t == exceptTrack)
            continue;

        auto& other = tracks[(size_t) t];

        if (other.state == State::armed)
            clear (t);
        else if (other.state == State::recording)
            finishFirstRecording (other, eventTime);
        else if (other.state == State::overdubbing)
            stopOverdub (other);
    }
}

void Looper::press (int t, bool countIn)
{
    auto& track = tracks[(size_t) t];

    switch (track.state)
    {
        case State::empty:
            finishActive (t);

            if (track.audio.getNumSamples() == 0)
                return;     // not allocated (shouldn't happen)

            track.state = State::armed;
            track.countIn = countIn;
            track.muted = false;

            if (! hasGrid())
                lockedSource = source.load();

            resolveArmed (track);
            break;

        case State::armed:
            clear (t);
            break;

        case State::recording:
            if (eventTime < track.origin)
                clear (t);          // still waiting for its bar line: cancel
            else
                finishFirstRecording (track, eventTime);
            break;

        case State::playing:
            if (track.muted)
            {
                track.muted = false;
                break;
            }

            finishActive (t);
            startOverdub (t);
            break;

        case State::overdubbing:
            stopOverdub (track);
            break;
    }
}

void Looper::resolveArmed (Track& track)
{
    if (! hasGrid())
    {
        if (! metronome.isClockRunning())
            return;     // waiting for the metronome to start

        gridAnchor = metronome.getClockBarStart();
        gridBarSamples = metronome.getClockBarSamples();
        gridBeats = metronome.getClockBeatsPerBar();
    }

    const auto m = eventTime;
    double origin = -1.0;

    if (track.countIn)
    {
        origin = metronome.getClockStartTime() + gridBarSamples;

        if (origin < m - 0.05 * sampleRate)
            origin = -1.0;      // too late for that: fall back to the nearest bar line
    }

    if (origin < 0.0)
    {
        const auto previous = gridAnchor + std::floor ((m - gridAnchor) / gridBarSamples) * gridBarSamples;
        const auto late = m - previous;
        origin = late <= juce::jmin (retroSeconds * sampleRate, 0.5 * gridBarSamples) ? previous : previous + gridBarSamples;
    }

    track.origin = origin;
    track.backfill = origin < musicalNow();
    track.firstPass = true;
    track.baseStart = origin;
    track.dubStart = track.dubEnd = 0.0;
    track.layer = -1;

    const auto maxBars = juce::jmax (1, (int) std::floor (maxTrackSamples() / gridBarSamples));
    const auto fixed = juce::jlimit (0, maxBars, fixedBars.load());

    if (fixed > 0)
    {
        track.bars = fixed;
        track.period = fixed * gridBarSamples;
        track.baseEnd = origin + track.period;
        track.playStart = origin + track.period;
    }
    else
    {
        track.bars = 0;
        track.period = 0.0;
        track.baseEnd = never;
        track.playStart = never;
    }

    track.state = State::recording;
}

void Looper::finishFirstRecording (Track& track, double m)
{
    const auto maxBars = juce::jmax (1, (int) std::floor (maxTrackSamples() / gridBarSamples));
    const auto upper = track.period > 0.0 ? track.bars : maxBars;
    const auto bars = juce::jlimit (1, upper, (int) std::round ((m - track.origin) / gridBarSamples));

    track.bars = bars;
    track.period = bars * gridBarSamples;
    track.baseEnd = track.origin + track.period;
    track.playStart = track.origin + track.period;     // may be in the past: then it plays right away
    track.state = State::playing;
}

int Looper::acquireLayer()
{
    // Only the latest overdub stays undoable: the previous one is merged into its track.
    if (undoLayer >= 0)
    {
        layers[(size_t) undoLayer].status = Layer::Status::merging;
        undoLayer = -1;
    }

    for (int pass = 0; pass < 2; ++pass)
    {
        for (int i = 0; i < (int) layers.size(); ++i)
            if (layers[(size_t) i].status == Layer::Status::free && layers[(size_t) i].audio.getNumSamples() > 0)
                return i;

        advanceLayers (std::numeric_limits<int>::max(), true);     // rare: both busy, finish now
    }

    return -1;
}

void Looper::startOverdub (int t)
{
    auto& track = tracks[(size_t) t];
    const auto index = acquireLayer();

    if (index < 0 || track.period <= 0.0)
        return;

    auto& layer = layers[(size_t) index];
    layer.status = Layer::Status::active;
    layer.owner = t;
    layer.progress = 0;

    track.layer = index;
    track.dubStart = juce::jmax (eventTime, track.firstPass ? track.baseEnd : 0.0);
    track.dubEnd = never;
    track.state = State::overdubbing;

    // A dated press: what was played since the stomp is already in the input history.
    if (track.dubStart < musicalNow())
        backfillLayer (track, track.dubStart, musicalNow(), 1.0f);
}

void Looper::stopOverdub (Track& track)
{
    track.dubEnd = juce::jmax (track.dubStart, eventTime);
    track.state = State::playing;

    // A dated press: take out what was recorded after the stomp (it is in the history too).
    if (track.layer >= 0 && track.dubEnd < musicalNow())
        backfillLayer (track, track.dubEnd, musicalNow(), -1.0f);

    if (track.layer >= 0)
    {
        layers[(size_t) track.layer].status = Layer::Status::undoable;
        undoLayer = track.layer;
    }

    track.layer = -1;
}

void Looper::clear (int t)
{
    auto& track = tracks[(size_t) t];

    for (int i = 0; i < (int) layers.size(); ++i)
    {
        auto& layer = layers[(size_t) i];

        if (layer.owner == t && layer.status != Layer::Status::free)
        {
            layer.status = Layer::Status::discarding;

            if (undoLayer == i)
                undoLayer = -1;
        }
    }

    track.state = State::empty;
    track.muted = false;
    track.period = 0.0;
    track.bars = 0;
    track.layer = -1;
    track.firstPass = track.backfill = track.countIn = false;
    track.baseStart = track.baseEnd = track.dubStart = track.dubEnd = 0.0;
    track.playStart = never;
}

void Looper::advanceLayers (int maxSamples, bool finishAll)
{
    for (auto& layer : layers)
    {
        if (layer.status != Layer::Status::merging && layer.status != Layer::Status::discarding)
            continue;

        auto& track = tracks[(size_t) juce::jmax (0, layer.owner)];
        const auto length = juce::jmin (layer.audio.getNumSamples(),
                                        layer.status == Layer::Status::merging ? trackLength (track) : layer.audio.getNumSamples());
        const auto chunk = juce::jmin (finishAll ? length : maxSamples, length - layer.progress);

        if (chunk > 0)
        {
            for (int c = 0; c < layer.audio.getNumChannels(); ++c)
            {
                if (layer.status == Layer::Status::merging && c < track.audio.getNumChannels())
                    track.audio.addFrom (c, layer.progress, layer.audio, c, layer.progress, chunk);

                layer.audio.clear (c, layer.progress, chunk);
            }

            layer.progress += chunk;
        }

        if (layer.progress >= length)
        {
            layer.status = Layer::Status::free;
            layer.owner = -1;
            layer.progress = 0;
        }
    }
}

void Looper::updateGridAndStatus()
{
    bool any = false, overdubbing = false;

    for (int t = 0; t < numTracks; ++t)
    {
        auto& track = tracks[(size_t) t];
        auto& s = status[(size_t) t];
        any = any || track.state != State::empty;
        overdubbing = overdubbing || track.state == State::overdubbing;

        auto shown = track.state;

        if (shown == State::recording && musicalNow() < track.origin)
            shown = State::armed;

        s.state = (int) shown;
        s.muted = track.muted;
        s.bars = track.state == State::recording && track.period <= 0.0
                   ? juce::jmax (0, (int) std::floor ((musicalNow() - track.origin) / juce::jmax (1.0, gridBarSamples)) + 1)
                   : track.bars;

        if (track.period > 0.0 && (double) now >= track.playStart)
            s.position = (float) (positiveIndex ((double) now - track.origin, track.period) / track.period);
        else if (track.state == State::recording && musicalNow() >= track.origin && track.period > 0.0)
            s.position = (float) ((musicalNow() - track.origin) / track.period);
        else
            s.position = -1.0f;
    }

    if (! any)
    {
        gridBarSamples = 0.0;
        lockedSource = source.load();
    }

    metronome.setLockedGrid (hasGrid(), gridAnchor, gridBarSamples, gridBeats);

    emptyFlag = ! any;
    undoAvailable = undoLayer >= 0 || overdubbing;
    activeSource = lockedSource;
    currentBar = hasGrid() ? (int) std::floor (((double) now - gridAnchor) / gridBarSamples) : -1;
}

double Looper::musicalTimeOf (double pressSeconds) const noexcept
{
    // Input sample now + blockLength arrived about when this callback started.
    const auto graph = (double) (now + blockLength) - (blockWallSeconds - pressSeconds) * sampleRate;
    return juce::jlimit ((double) now - maxPressAge * sampleRate, (double) now, graph) - comp;
}

void Looper::backfillLayer (Track& track, double from, double to, float sign)
{
    if (track.layer < 0 || track.period <= 0.0)
        return;

    auto& layer = layers[(size_t) track.layer].audio;
    const auto& history = lockedSource == (int) Source::beforeFx ? dryHistory : wetHistory;
    const auto historyLength = (juce::int64) history.getNumSamples();
    const auto layerLength = juce::jmin (layer.getNumSamples(), trackLength (track));

    if (historyLength == 0)
        return;

    // Graph times (history index) whose musical time m = g - comp lies in [from, to).
    const auto first = juce::jmax ((juce::int64) std::ceil (from + comp), now - historyLength + 1);
    const auto last = juce::jmin ((juce::int64) std::ceil (to + comp), now);

    for (auto g = first; g < last; ++g)
    {
        const auto index = positiveIndex ((double) (g - comp) - track.origin, track.period);

        if (index >= layerLength)
            continue;

        for (int c = 0; c < history.getNumChannels() && c < layer.getNumChannels(); ++c)
            layer.addSample (c, index, sign * history.getSample (c, (int) (g % historyLength)));
    }
}

//==============================================================================
void Looper::writeHistory (juce::AudioBuffer<float>& history, int channel, const float* data, juce::int64 graphStart, int numSamples)
{
    const auto length = history.getNumSamples();

    if (length == 0)
        return;

    auto* dest = history.getWritePointer (channel);
    auto index = (int) (graphStart % length);

    for (int i = 0; i < numSamples; ++i)
    {
        dest[index] = data[i];

        if (++index >= length)
            index = 0;
    }
}

void Looper::backfillFromHistory (Track& track, const juce::AudioBuffer<float>& history)
{
    track.backfill = false;
    const auto length = (juce::int64) history.getNumSamples();
    const auto bufferLength = track.audio.getNumSamples();

    if (length == 0)
        return;

    // Graph times whose (latency-shifted) audio belongs to the loop and has already gone by.
    const auto first = juce::jmax ((juce::int64) std::ceil (track.origin + comp), now - length + 1);

    for (auto g = first; g < now; ++g)
    {
        const auto m = (double) (g - comp);

        if (m < track.baseStart || m >= track.baseEnd)
            continue;

        const auto index = (int) std::floor (m - track.origin);

        if (! juce::isPositiveAndBelow (index, bufferLength))
            continue;

        for (int c = 0; c < history.getNumChannels(); ++c)
            track.audio.setSample (c, index, history.getSample (c, (int) (g % length)));
    }
}

void Looper::capture (Track& track, const float* const* input, int numChannels, int numSamples)
{
    const auto bufferLength = track.audio.getNumSamples();
    const auto hasLayer = track.layer >= 0;
    auto* layer = hasLayer ? &layers[(size_t) track.layer].audio : nullptr;
    const auto layerLength = hasLayer ? juce::jmin (layer->getNumSamples(), trackLength (track)) : 0;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto m = (double) (now + i - comp);

        if (track.firstPass && m >= track.baseStart && m < track.baseEnd)
        {
            const auto index = (int) std::floor (m - track.origin);

            if (juce::isPositiveAndBelow (index, bufferLength))
                for (int c = 0; c < numChannels; ++c)
                    track.audio.setSample (c, index, input[c][i]);
        }

        if (hasLayer && m >= track.dubStart && m < track.dubEnd)
        {
            const auto index = positiveIndex (m - track.origin, track.period);

            if (index < layerLength)
                for (int c = 0; c < numChannels; ++c)
                    layer->addSample (c, index, input[c][i]);
        }
    }
}

void Looper::play (Track& track, Status& s, float* left, float* right, int numSamples)
{
    const auto target = track.muted ? 0.0f : s.gain.load();
    const auto startGain = track.lastGain;
    track.lastGain = target;

    if (track.period <= 0.0 || (startGain == 0.0f && target == 0.0f))
        return;

    const auto length = trackLength (track);
    const auto stereo = right != nullptr;

    // Layers still belonging to this track (overdub being recorded, undoable, or being merged).
    std::array<const juce::AudioBuffer<float>*, 2> extra {};
    size_t numExtra = 0;

    for (auto& layer : layers)
        if (layer.owner == (int) (&track - tracks.data())
            && (layer.status == Layer::Status::active || layer.status == Layer::Status::undoable || layer.status == Layer::Status::merging))
            extra[numExtra++] = &layer.audio;

    float peak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto g = (double) (now + i);

        if (g < track.playStart)
            continue;

        const auto index = positiveIndex (g - track.origin, track.period);

        if (index >= length)
            continue;

        auto l = track.audio.getSample (0, index);
        auto r = stereo ? track.audio.getSample (1, index) : 0.0f;

        for (size_t e = 0; e < numExtra; ++e)
        {
            l += extra[e]->getSample (0, index);

            if (stereo)
                r += extra[e]->getSample (1, index);
        }

        const auto gain = startGain + (target - startGain) * ((float) i / (float) numSamples);
        left[i] += l * gain;
        peak = juce::jmax (peak, std::abs (l * gain));

        if (stereo)
        {
            right[i] += r * gain;
            peak = juce::jmax (peak, std::abs (r * gain));
        }
    }

    if (peak > s.peak.load (std::memory_order_relaxed))
        s.peak.store (peak, std::memory_order_relaxed);
}

void Looper::processBeforeFx (float* mono, int numSamples)
{
    if (metronome.isClockRunning())
        calibrator.process (mono, numSamples, now, metronome.getClockBarStart(), metronome.getClockBeatSamples());

    if (lockedSource == (int) Source::beforeFx)
    {
        const float* input[] = { mono };

        for (auto& track : tracks)
        {
            if (track.state == State::empty || track.state == State::armed)
                continue;

            if (track.backfill)
                backfillFromHistory (track, dryHistory);

            capture (track, input, 1, numSamples);
        }
    }

    writeHistory (dryHistory, 0, mono, now, numSamples);

    if (lockedSource == (int) Source::beforeFx)
        for (int t = 0; t < numTracks; ++t)
            if (tracks[(size_t) t].state != State::empty && tracks[(size_t) t].state != State::armed)
                play (tracks[(size_t) t], status[(size_t) t], mono, nullptr, numSamples);
}

void Looper::processAfterFx (float* left, float* right, int numSamples)
{
    if (lockedSource == (int) Source::afterFx)
    {
        const float* input[] = { left, right };

        for (auto& track : tracks)
        {
            if (track.state == State::empty || track.state == State::armed)
                continue;

            if (track.backfill)
                backfillFromHistory (track, wetHistory);

            capture (track, input, 2, numSamples);
        }
    }

    writeHistory (wetHistory, 0, left, now, numSamples);
    writeHistory (wetHistory, 1, right, now, numSamples);

    juce::FloatVectorOperations::clear (left, numSamples);

    if (right != left)
        juce::FloatVectorOperations::clear (right, numSamples);

    if (lockedSource == (int) Source::afterFx)
        for (int t = 0; t < numTracks; ++t)
            if (tracks[(size_t) t].state != State::empty && tracks[(size_t) t].state != State::armed)
                play (tracks[(size_t) t], status[(size_t) t], left, right, numSamples);
}

} // namespace pv
