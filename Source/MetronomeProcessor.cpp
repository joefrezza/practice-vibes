#include "MetronomeProcessor.h"

namespace pv
{

//==============================================================================
std::unique_ptr<juce::XmlElement> MetronomeProcessor::createSettingsXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("PRACTICE_VIBES_METRONOME");
    xml->setAttribute ("bpm", startBpm.load());
    xml->setAttribute ("beatsPerBar", beatsPerBar.load());
    xml->setAttribute ("accentFirstBeat", accentFirstBeat.load());
    xml->setAttribute ("gain", (double) gain.load());
    xml->setAttribute ("subdivision", subdivision.load());
    xml->setAttribute ("sound", sound.load());
    xml->setAttribute ("subdivisionLevel", (double) subdivisionLevel.load());
    xml->setAttribute ("swing", swing.load());
    xml->setAttribute ("followSong", followSong.load());
    xml->setAttribute ("trainer", trainerEnabled.load());
    xml->setAttribute ("trainerStepBpm", trainerStepBpm.load());
    xml->setAttribute ("trainerInterval", trainerInterval.load());
    xml->setAttribute ("trainerUnit", trainerUnit.load());
    xml->setAttribute ("trainerMaxBpm", trainerMaxBpm.load());
    return xml;
}

void MetronomeProcessor::applySettingsXml (const juce::XmlElement& xml)
{
    // Same ranges as the UI controls, so a damaged file cannot produce odd values.
    startBpm = juce::jlimit (30.0, 300.0, xml.getDoubleAttribute ("bpm", startBpm.load()));
    beatsPerBar = juce::jlimit (1, 12, xml.getIntAttribute ("beatsPerBar", beatsPerBar.load()));
    accentFirstBeat = xml.getBoolAttribute ("accentFirstBeat", accentFirstBeat.load());
    gain = juce::jlimit (0.0f, 2.0f, (float) xml.getDoubleAttribute ("gain", gain.load()));
    subdivision = juce::jlimit (0, 3, xml.getIntAttribute ("subdivision", subdivision.load()));
    sound = juce::jlimit (0, 2, xml.getIntAttribute ("sound", sound.load()));
    subdivisionLevel = juce::jlimit (0.0f, 1.0f, (float) xml.getDoubleAttribute ("subdivisionLevel", subdivisionLevel.load()));
    swing = juce::jlimit (0.5, 0.75, xml.getDoubleAttribute ("swing", swing.load()));
    followSong = xml.getBoolAttribute ("followSong", followSong.load());
    trainerEnabled = xml.getBoolAttribute ("trainer", trainerEnabled.load());
    trainerStepBpm = juce::jlimit (-20.0, 20.0, xml.getDoubleAttribute ("trainerStepBpm", trainerStepBpm.load()));
    trainerInterval = juce::jlimit (0.5, 60.0, xml.getDoubleAttribute ("trainerInterval", trainerInterval.load()));
    trainerUnit = juce::jlimit (0, 1, xml.getIntAttribute ("trainerUnit", trainerUnit.load()));
    trainerMaxBpm = juce::jlimit (30.0, 300.0, xml.getDoubleAttribute ("trainerMaxBpm", trainerMaxBpm.load()));
}

namespace
{
    float decayPerSample (double seconds, double sampleRate)
    {
        return (float) std::exp (-1.0 / (juce::jmax (0.0005, seconds) * sampleRate));
    }
}

MetronomeProcessor::MetronomeProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

void MetronomeProcessor::start()
{
    startRequested = true;
    running = true;
}

void MetronomeProcessor::stop()
{
    running = false;
    currentBeat = -1;
}

void MetronomeProcessor::prepareToPlay (double newSampleRate, int maximumBlockSize)
{
    sampleRate = newSampleRate > 0 ? newSampleRate : 48000.0;
    samplesRendered = 0;
    followLocked = false;
    pendingTicks.reserve ((size_t) juce::jmax (64, maximumBlockSize));
    clockTicks.reserve ((size_t) juce::jmax (64, maximumBlockSize));
    beatSamples = sampleRate * 60.0 / bpm;
}

void MetronomeProcessor::setSongGrid (std::shared_ptr<const ChordTrack> grid)
{
    const juce::SpinLock::ScopedLockType lock (gridLock);
    songGrid = std::move (grid);
}

double MetronomeProcessor::followTickTime (const ChordTrack& grid, int beat, int tick, int ticks) const
{
    const auto& b = grid.beats;
    const auto n = (int) b.size();
    const auto start = beat < n ? b[(size_t) beat] : b[(size_t) n - 1] + (beat - n + 1) * (b[(size_t) n - 1] - b[(size_t) n - 2]);
    const auto next = beat + 1 < n ? b[(size_t) beat + 1] : start + (b[(size_t) n - 1] - b[(size_t) n - 2]);
    return start + (next - start) * tickFraction (tick, ticks, tickSwing());
}

double MetronomeProcessor::tickSwing() const noexcept
{
    return canSwing ((Subdivision) subdivision.load()) ? juce::jlimit (0.5, 0.75, swing.load()) : 0.5;
}

//==============================================================================
void MetronomeProcessor::trigger (double frequency, double frequencyEnd, float toneLevel, double toneDecaySeconds,
                                  float noiseLevel, double noiseDecaySeconds, float highPass)
{
    auto& v = voices[nextVoice];
    nextVoice = (nextVoice + 1) % voices.size();

    v.active = true;
    v.phase = 0.0;
    v.frequency = frequency;
    v.frequencyEnd = frequencyEnd > 0.0 ? frequencyEnd : frequency;
    // Exponential pitch sweep over ~40 ms (used by the kick).
    v.sweep = std::pow (v.frequencyEnd / juce::jmax (1.0, v.frequency), 1.0 / (0.04 * sampleRate));
    v.toneLevel = toneLevel;
    v.toneEnvelope = toneLevel > 0.0f ? 1.0f : 0.0f;
    v.toneDecay = decayPerSample (toneDecaySeconds, sampleRate);
    v.noiseLevel = noiseLevel;
    v.noiseEnvelope = noiseLevel > 0.0f ? 1.0f : 0.0f;
    v.noiseDecay = decayPerSample (noiseDecaySeconds, sampleRate);
    v.highPassAmount = highPass;
    v.lastNoise = 0.0f;
}

float MetronomeProcessor::renderVoices()
{
    float out = 0.0f;

    for (auto& v : voices)
    {
        if (! v.active)
            continue;

        if (v.toneEnvelope > 0.0001f)
        {
            out += (float) std::sin (v.phase) * v.toneEnvelope * v.toneLevel;
            v.phase += juce::MathConstants<double>::twoPi * v.frequency / sampleRate;
            v.toneEnvelope *= v.toneDecay;

            if ((v.sweep < 1.0 && v.frequency > v.frequencyEnd) || (v.sweep > 1.0 && v.frequency < v.frequencyEnd))
                v.frequency *= v.sweep;
        }

        if (v.noiseEnvelope > 0.0001f)
        {
            const auto white = random.nextFloat() * 2.0f - 1.0f;
            const auto filtered = white - v.highPassAmount * v.lastNoise;   // first-difference style high-pass
            v.lastNoise = white;
            out += filtered * v.noiseEnvelope * v.noiseLevel;
            v.noiseEnvelope *= v.noiseDecay;
        }

        if (v.toneEnvelope <= 0.0001f && v.noiseEnvelope <= 0.0001f)
            v.active = false;
    }

    return out;
}

void MetronomeProcessor::onTick (int beatInBar, int tickInBeat, int barLength)
{
    const auto isBeat = tickInBeat == 0;
    const auto isDownbeat = isBeat && beatInBar == 0 && accentFirstBeat.load() && barLength > 1;
    const auto subLevel = subdivisionLevel.load();

    switch ((Sound) sound.load())
    {
        case Sound::beep:
            if (isDownbeat)      trigger (1760.0, 0.0, 1.0f, 0.012);
            else if (isBeat)     trigger (1100.0, 0.0, 0.6f, 0.012);
            else                 trigger (880.0, 0.0, 0.6f * subLevel, 0.010);
            break;

        case Sound::click:      // short woodblock-like knock
            if (isDownbeat)      trigger (2600.0, 0.0, 1.0f, 0.004, 0.25f, 0.002, 0.9f);
            else if (isBeat)     trigger (2000.0, 0.0, 0.65f, 0.004, 0.15f, 0.002, 0.9f);
            else                 trigger (1600.0, 0.0, 0.65f * subLevel, 0.003, 0.1f * subLevel, 0.002, 0.9f);
            break;

        case Sound::drums:
        {
            // Rock pattern: kick on odd beats (1, 3...), snare on even beats (2, 4...),
            // hi-hat on every tick (softer on subdivisions).
            const auto hatLevel = isBeat ? 0.25f : 0.25f * juce::jmax (0.2f, subLevel * 1.6f);
            trigger (0.0, 0.0, 0.0f, 0.001, hatLevel, 0.03, 0.95f);

            if (isBeat)
            {
                if (beatInBar % 2 == 0)
                    trigger (160.0, 48.0, isDownbeat ? 0.85f : 0.65f, 0.16);               // kick
                else
                    trigger (190.0, 0.0, 0.3f, 0.05, 0.45f, 0.11, 0.5f);                   // snare
            }
            break;
        }
    }

    if (isBeat)
    {
        currentBeat = beatInBar;
        currentBeatsPerBar = barLength;
    }
}

void MetronomeProcessor::setLockedGrid (bool locked, double anchor, double barSamples, int beats)
{
    gridLocked = locked && barSamples > 0.0 && beats > 0;
    lockedAnchor = anchor;
    lockedBarSamples = barSamples;
    lockedBeats = juce::jmax (1, beats);
    tempoLocked = gridLocked;
}

void MetronomeProcessor::onBarLine()
{
    ++barsSinceStep;

    if (gridLocked)
        return;

    barBeats = juce::jmax (1, beatsPerBar.load());

    // Speed trainer: tempo steps happen on bar lines.
    if (trainerEnabled.load() && ! following.load())
    {
        const auto interval = juce::jmax (0.01, trainerInterval.load());
        const auto due = trainerUnit.load() == (int) IntervalUnit::bars
                           ? barsSinceStep >= (int) std::round (interval)
                           : secondsSinceStep >= interval * 60.0;

        if (due)
        {
            const auto step = trainerStepBpm.load();
            const auto limit = trainerMaxBpm.load();
            bpm = step >= 0 ? juce::jmin (limit, bpm + step) : juce::jmax (limit, bpm + step);
            currentBpm = bpm;
            beatSamples = sampleRate * 60.0 / bpm;
            barsSinceStep = 0;
            secondsSinceStep = 0.0;
        }
    }
}

void MetronomeProcessor::advanceClock (juce::int64 blockStart, int numSamples)
{
    clockAdvanced = true;
    nextBlockStart = blockStart + numSamples;
    clockTicks.clear();

    const auto start = (double) blockStart;
    const auto ticks = ticksPerBeat ((Subdivision) subdivision.load());
    const auto swingNow = tickSwing();

    // Puts the next tick at or after `time` (the bar start and tempo being set).
    auto placeNextTick = [this, ticks, swingNow] (double time)
    {
        const auto position = juce::jmax (0.0, (time - barStart) / beatSamples);
        nextBeat = (int) std::floor (position);
        nextTick = 0;
        const auto within = (position - nextBeat) * beatSamples;   // samples into the beat

        while (nextTick < ticks && tickFraction (nextTick, ticks, swingNow) * beatSamples < within - 1.0e-6)
            ++nextTick;

        if (nextTick >= ticks)
        {
            nextTick = 0;
            ++nextBeat;
        }
    };

    if (startRequested.exchange (false))
    {
        resetVoices = true;
        barsSinceStep = 0;
        secondsSinceStep = 0.0;
        lastTicks = ticks;
        lastSwing = swingNow;

        if (gridLocked)
        {
            // Line up with the loops: same tempo, same bar lines.
            barBeats = lockedBeats;
            beatSamples = lockedBarSamples / barBeats;
            bpm = 60.0 * sampleRate / beatSamples;
            barStart = lockedAnchor + std::floor ((start - lockedAnchor) / lockedBarSamples) * lockedBarSamples;
            placeNextTick (start);
            clockStartTime = nextBeat == 0 && nextTick == 0 ? barStart : barStart + lockedBarSamples;
        }
        else
        {
            bpm = juce::jlimit (20.0, 400.0, startBpm.load());
            barBeats = juce::jmax (1, beatsPerBar.load());
            beatSamples = sampleRate * 60.0 / bpm;
            barStart = start;
            nextBeat = 0;
            nextTick = 0;
            clockStartTime = start;
        }

        currentBpm = bpm;
        barPpq = 0.0;
        previousBarBeats = barBeats;
        previousBeatSamples = beatSamples;
        clockRunning = true;
    }

    if (! running.load())
        clockRunning = false;

    if (! clockRunning)
        return;

    if (resetTempoRequested.exchange (false) && ! gridLocked)
    {
        // Keep the position within the bar; the new tempo applies from here on.
        const auto position = (start - barStart) / beatSamples;
        bpm = juce::jlimit (20.0, 400.0, startBpm.load());
        currentBpm = bpm;
        beatSamples = sampleRate * 60.0 / bpm;
        barStart = start - position * beatSamples;
        barsSinceStep = 0;
        secondsSinceStep = 0.0;
    }

    if (ticks != lastTicks || swingNow != lastSwing)
    {
        // The subdivision or swing changed: the next tick moves to the new grid.
        const auto position = nextBeat + tickFraction (nextTick, lastTicks, lastSwing);
        lastTicks = ticks;
        lastSwing = swingNow;
        placeNextTick (barStart + position * beatSamples);
    }

    const auto blockEnd = blockStart + numSamples;

    for (;;)
    {
        if (nextBeat >= barBeats)
        {
            // The bar line is crossed when the next downbeat is due, not after the last tick.
            if ((juce::int64) std::ceil (barStart + barBeats * beatSamples - 1.0e-9) >= blockEnd)
                break;

            barStart += barBeats * beatSamples;
            barPpq += barBeats;
            previousBarBeats = barBeats;
            previousBeatSamples = beatSamples;      // onBarLine() may change the tempo
            nextBeat = 0;
            nextTick = 0;
            onBarLine();
            continue;
        }

        const auto t = barStart + (nextBeat + tickFraction (nextTick, ticks, swingNow)) * beatSamples;
        const auto sample = (juce::int64) std::ceil (t - 1.0e-9);

        if (sample >= blockEnd)
            break;

        const auto offset = (int) juce::jlimit ((juce::int64) 0, (juce::int64) numSamples - 1, sample - blockStart);
        clockTicks.push_back ({ offset, { nextBeat, nextTick, barBeats } });

        if (++nextTick >= ticks)
        {
            nextTick = 0;
            ++nextBeat;
        }
    }

    if (! following.load())
        secondsSinceStep += numSamples / sampleRate;
}

void MetronomeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    buffer.clear();

    const auto numSamples = buffer.getNumSamples();

    if (! clockAdvanced)
        advanceClock (nextBlockStart, numSamples);    // nobody drives the clock (offline use)

    clockAdvanced = false;

    if (resetVoices)
    {
        resetVoices = false;

        for (auto& v : voices)
            v.active = false;
    }

    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
    const auto targetGain = clicksMuted.load() ? 0.0f : gain.load();
    const auto isRunning = running.load();

    // Follow mode: ticks come from the song's beat grid, placed where the player is.
    // Paused while the looper holds the tempo.
    std::shared_ptr<const ChordTrack> grid;

    if (followSong.load() && player != nullptr && ! gridLocked)
    {
        const juce::SpinLock::ScopedTryLockType lock (gridLock);

        if (lock.isLocked())
            grid = songGrid;
    }

    const auto followMode = isRunning && grid != nullptr && grid->beats.size() > 1;
    following = followMode;
    pendingTicks.clear();

    if (followMode)
    {
        std::vector<int> unused;
        renderFollowingTicks (numSamples, *grid, unused);
    }
    else
    {
        followLocked = false;
        pendingTicks = clockTicks;
    }

    size_t nextPending = 0;

    for (int i = 0; i < numSamples; ++i)
    {
        while (nextPending < pendingTicks.size() && pendingTicks[nextPending].first <= i)
        {
            const auto& t = pendingTicks[nextPending++].second;
            onTick (t[0], t[1], t[2]);
        }

        // Voices keep ringing briefly after stop, so stopping never clicks.
        const auto g = lastGain + (targetGain - lastGain) * ((float) i / (float) numSamples);
        const auto sample = juce::jlimit (-1.0f, 1.0f, renderVoices() * g);
        left[i] = sample;

        if (right != nullptr)
            right[i] = sample;
    }

    lastGain = targetGain;
    samplesRendered += numSamples;
}

void MetronomeProcessor::renderFollowingTicks (int numSamples, const ChordTrack& grid, std::vector<int>&)
{
    PlayerProcessor::Clock clock;

    if (! player->getClock (clock) || ! clock.playing)
    {
        followLocked = false;
        return;
    }

    // Song time at the first sample of this block. The player's clock may be from the end of
    // this very block or of the previous one, depending on processing order; the sample counts tell.
    const auto rate = clock.speed / sampleRate;    // song seconds per output sample
    const auto songTime = clock.heardSeconds - (double) (clock.samplesRendered - samplesRendered) * rate;
    const auto ticks = ticksPerBeat ((Subdivision) subdivision.load());
    const auto& beats = grid.beats;
    const auto numBeats = (int) beats.size();

    // After a seek, a loop jump or a pause, find the next tick again. A tick up to 30 ms
    // in the past still plays, so the first beat of a loop isn't skipped.
    if (! followLocked || std::abs (songTime - expectedSongTime) > 0.03)
    {
        const auto target = songTime - 0.03;
        followBeat = juce::jmax (0, (int) std::distance (beats.begin(), std::upper_bound (beats.begin(), beats.end(), target)) - 1);
        followTick = 0;

        while (followTickTime (grid, followBeat, followTick, ticks) < target)
        {
            if (++followTick >= ticks)
            {
                followTick = 0;
                ++followBeat;
            }
        }

        followLocked = true;
    }

    expectedSongTime = songTime + numSamples * rate;
    followTick = juce::jmin (followTick, ticks - 1);    // the subdivision may have changed

    for (;;)
    {
        const auto t = followTickTime (grid, followBeat, followTick, ticks);

        if (t >= expectedSongTime)
            break;

        const auto offset = juce::jlimit (0, numSamples - 1, (int) std::floor ((t - songTime) / rate));
        const auto inRange = followBeat < numBeats;
        const auto position = inRange && followBeat < (int) grid.beatPositions.size() ? grid.beatPositions[(size_t) followBeat] : -1;
        const auto barLength = inRange && followBeat < (int) grid.beatMeters.size() ? grid.beatMeters[(size_t) followBeat] : grid.beatsPerBar;

        if (inRange)
            pendingTicks.push_back ({ offset, { juce::jmax (0, position), followTick, position < 0 ? 1 : barLength } });

        if (followTick == 0 && inRange && followBeat + 1 < numBeats)
            currentBpm = 60.0 / (beats[(size_t) followBeat + 1] - beats[(size_t) followBeat]) * clock.speed;

        if (++followTick >= ticks)
        {
            followTick = 0;
            ++followBeat;
        }
    }
}

} // namespace pv
