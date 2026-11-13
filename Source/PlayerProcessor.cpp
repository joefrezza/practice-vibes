#include "PlayerProcessor.h"

namespace pv
{

namespace
{
    /** Gives the stretcher `reader[channel][i]` access to the song, starting at `start`,
        wrapping around the loop region and returning silence past the end.
    */
    struct SongReader
    {
        const LoadedSong* song = nullptr;
        juce::int64 start = 0;
        bool looping = false;
        juce::int64 loopStart = 0, loopEnd = 0;

        juce::int64 map (juce::int64 position) const noexcept
        {
            if (looping && position >= loopEnd)
                position = loopStart + (position - loopStart) % (loopEnd - loopStart);

            return position;
        }

        struct Channel
        {
            const SongReader& reader;
            const float* data;
            juce::int64 length;

            float operator[] (int index) const noexcept
            {
                const auto position = reader.map (reader.start + index);
                return (position >= 0 && position < length) ? data[position] : 0.0f;
            }
        };

        Channel operator[] (int channel) const noexcept
        {
            return { *this, song->audio.getReadPointer (channel), (juce::int64) song->audio.getNumSamples() };
        }
    };

    std::shared_ptr<LoadedSong> decodeSong (juce::AudioFormatManager& formats, const juce::URL& url, juce::String& error)
    {
        auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress));

        if (stream == nullptr)
        {
            error = "could not open the file";
            return {};
        }

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (std::move (stream)));

        if (reader == nullptr)
        {
            error = "unsupported or damaged audio file";
            return {};
        }

        const auto length = (int) juce::jmin<juce::int64> (reader->lengthInSamples, std::numeric_limits<int>::max());

        if (length <= 0)
        {
            error = "the file contains no audio";
            return {};
        }

        auto song = std::make_shared<LoadedSong>();
        song->sampleRate = reader->sampleRate > 0 ? reader->sampleRate : 44100.0;
        song->name = juce::URL::removeEscapeChars (url.getFileName());
        song->audio.setSize (2, length);

        reader->read (&song->audio, 0, length, 0, true, true);

        if (reader->numChannels == 1)
            song->audio.copyFrom (1, 0, song->audio, 0, 0, length);

        song->buildPeaks();

        return song;
    }
}

//==============================================================================
void LoadedSong::buildPeaks()
{
    peakLevels.clear();

    const auto length = audio.getNumSamples();

    if (length <= 0)
        return;

    // Finest level straight from the audio.
    PeakLevel level;
    level.samplesPerBucket = 256;
    const auto numBuckets = (length + level.samplesPerBucket - 1) / level.samplesPerBucket;
    level.mins.resize ((size_t) numBuckets);
    level.maxs.resize ((size_t) numBuckets);

    const auto* left = audio.getReadPointer (0);
    const auto* right = audio.getReadPointer (1);

    for (int bucket = 0; bucket < numBuckets; ++bucket)
    {
        const auto begin = bucket * level.samplesPerBucket;
        const auto end = juce::jmin (length, begin + level.samplesPerBucket);
        auto lo = 0.0f, hi = 0.0f;

        for (int i = begin; i < end; ++i)
        {
            const auto mono = 0.5f * (left[i] + right[i]);
            lo = juce::jmin (lo, mono);
            hi = juce::jmax (hi, mono);
        }

        level.mins[(size_t) bucket] = lo;
        level.maxs[(size_t) bucket] = hi;
    }

    peakLevels.push_back (std::move (level));

    // Each further level combines 4 buckets of the previous one.
    while (peakLevels.back().mins.size() > 512)
    {
        const auto& previous = peakLevels.back();
        PeakLevel coarser;
        coarser.samplesPerBucket = previous.samplesPerBucket * 4;
        const auto count = (previous.mins.size() + 3) / 4;
        coarser.mins.resize (count);
        coarser.maxs.resize (count);

        for (size_t i = 0; i < count; ++i)
        {
            auto lo = 0.0f, hi = 0.0f;

            for (size_t j = i * 4; j < juce::jmin (previous.mins.size(), i * 4 + 4); ++j)
            {
                lo = juce::jmin (lo, previous.mins[j]);
                hi = juce::jmax (hi, previous.maxs[j]);
            }

            coarser.mins[i] = lo;
            coarser.maxs[i] = hi;
        }

        peakLevels.push_back (std::move (coarser));
    }
}

//==============================================================================
PlayerProcessor::PlayerProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    formatManager.registerBasicFormats();
    stretch.presetDefault (2, (float) deviceSampleRate);
}

PlayerProcessor::~PlayerProcessor()
{
    alive.reset();
    loaderPool.removeAllJobs (true, 10000);
}

void PlayerProcessor::loadAsync (const juce::URL& file, LoadCallback onDone)
{
    std::weak_ptr<bool> weakAlive = alive;

    loaderPool.addJob ([this, weakAlive, file, done = std::move (onDone)]
    {
        juce::String error;
        auto decoded = decodeSong (formatManager, file, error);

        juce::MessageManager::callAsync ([this, weakAlive, decoded, error, done]
        {
            if (weakAlive.expired())
                return;

            if (decoded != nullptr)
            {
                setSong (decoded);
                juce::Logger::writeToLog ("Loaded song " + decoded->name + " ("
                                          + juce::String (decoded->getLengthSeconds(), 1) + " s, "
                                          + juce::String (decoded->sampleRate, 0) + " Hz)");
            }
            else
            {
                juce::Logger::writeToLog ("Could not load song: " + error);
            }

            if (done)
                done (error);
        });
    });
}

void PlayerProcessor::setSong (std::shared_ptr<LoadedSong> newSong)
{
    playing = false;
    loopEnabled = false;
    loopStart = 0;
    loopEnd = 0;
    positionSeconds = 0.0;

    std::shared_ptr<LoadedSong> old;

    {
        const juce::SpinLock::ScopedLockType lock (songLock);
        old = std::exchange (song, std::move (newSong));
        seekTarget = 0;
    }

    // `old` is released here, on the message thread, never on the audio thread.
}

//==============================================================================
juce::int64 PlayerProcessor::secondsToSamples (double seconds) const
{
    if (song == nullptr)
        return 0;

    return juce::jlimit<juce::int64> (0, song->audio.getNumSamples(), (juce::int64) (seconds * song->sampleRate));
}

void PlayerProcessor::play()
{
    if (song == nullptr)
        return;

    // Restart from the top if we had reached the end.
    if (! loopEnabled.load() && getPositionSeconds() >= getLengthSeconds() - 0.05)
        seekToSeconds (0.0);

    playing = true;
}

void PlayerProcessor::pause()
{
    playing = false;
}

void PlayerProcessor::stop()
{
    playing = false;
    seekToSeconds (loopEnabled.load() && hasLoop() ? getLoopStartSeconds() : 0.0);
}

void PlayerProcessor::seekToSeconds (double seconds)
{
    if (song == nullptr)
        return;

    seekTarget = secondsToSamples (seconds);
    positionSeconds = juce::jlimit (0.0, getLengthSeconds(), seconds);
}

void PlayerProcessor::setLoopSeconds (double startSeconds, double endSeconds)
{
    if (endSeconds < startSeconds)
        std::swap (startSeconds, endSeconds);

    const auto start = secondsToSamples (startSeconds);
    const auto end = secondsToSamples (endSeconds);

    // Store end first as "empty" so the audio thread never sees end < start.
    loopEnd = 0;
    loopStart = start;
    loopEnd = end;
}

void PlayerProcessor::setLoopEnabled (bool shouldLoop)
{
    loopEnabled = shouldLoop && hasLoop();

    // Jump into the loop if we're outside it.
    if (loopEnabled.load())
    {
        const auto position = getPositionSeconds();

        if (position < getLoopStartSeconds() || position >= getLoopEndSeconds())
            seekToSeconds (getLoopStartSeconds());
    }
}

double PlayerProcessor::getLoopStartSeconds() const
{
    return song != nullptr ? (double) loopStart.load() / song->sampleRate : 0.0;
}

double PlayerProcessor::getLoopEndSeconds() const
{
    return song != nullptr ? (double) loopEnd.load() / song->sampleRate : 0.0;
}

//==============================================================================
bool PlayerProcessor::getClock (Clock& out) const
{
    const juce::SpinLock::ScopedTryLockType lock (clockLock);

    if (! lock.isLocked())
        return false;

    out = clock;
    return true;
}

void PlayerProcessor::prepareToPlay (double sampleRate, int)
{
    deviceSampleRate = sampleRate > 0 ? sampleRate : 48000.0;
    samplesRendered = 0;
    stretch.presetDefault (2, (float) deviceSampleRate);
    lastTransposeFactor = 0.0;

    // Re-prime the stretcher at the current position.
    if (song != nullptr)
        seekTarget = secondsToSamples (getPositionSeconds());
}

void PlayerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    renderBlock (buffer);
    samplesRendered += buffer.getNumSamples();

    const juce::SpinLock::ScopedLockType lock (clockLock);
    clock.samplesRendered = samplesRendered;
    clock.heardSeconds = positionSeconds.load();
    clock.speed = speed.load();
    clock.playing = playing.load() && song != nullptr;
}

void PlayerProcessor::renderBlock (juce::AudioBuffer<float>& buffer)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    const juce::SpinLock::ScopedTryLockType lock (songLock);

    if (! lock.isLocked() || song == nullptr || buffer.getNumChannels() < 2)
        return;

    const auto numSamples = buffer.getNumSamples();
    const auto length = (juce::int64) song->audio.getNumSamples();
    const auto rateRatio = song->sampleRate / deviceSampleRate;   // song samples per device sample at 1x
    const auto playbackRate = speed.load() * rateRatio;

    SongReader reader;
    reader.song = song.get();
    reader.loopStart = loopStart.load();
    reader.loopEnd = loopEnd.load();
    reader.looping = loopEnabled.load() && reader.loopEnd - reader.loopStart > 256;

    // The stretcher preserves samples-per-cycle, so a sample-rate mismatch must be
    // corrected through the transpose factor as well as the read rate.
    const auto transposeFactor = rateRatio * std::pow (2.0, semitones.load() / 12.0);

    if (transposeFactor != lastTransposeFactor)
    {
        stretch.setTransposeFactor ((float) transposeFactor);
        lastTransposeFactor = transposeFactor;
    }

    if (const auto target = seekTarget.exchange (-1); target >= 0)
    {
        stretch.reset();
        reader.start = target;
        const auto preroll = stretch.inputLatency();
        stretch.seek (reader, preroll, playbackRate);
        readPosition = reader.map (target + preroll);
        inputRemainder = 0.0;
    }

    if (! playing.load())
        return;

    inputRemainder += numSamples * playbackRate;
    const auto inputSamples = (int) inputRemainder;
    inputRemainder -= inputSamples;

    reader.start = readPosition;
    stretch.process (reader, inputSamples, buffer.getArrayOfWritePointers(), numSamples);
    readPosition = reader.map (readPosition + inputSamples);

    const auto newGain = gain.load();
    buffer.applyGainRamp (0, numSamples, lastGain, newGain);
    lastGain = newGain;

    // What the listener hears now was read this many song samples ago.
    const auto delay = (juce::int64) (stretch.inputLatency() + stretch.outputLatency() * playbackRate);
    auto heard = readPosition - delay;

    if (reader.looping && heard < reader.loopStart && readPosition >= reader.loopStart)
        heard += reader.loopEnd - reader.loopStart;

    positionSeconds = (double) juce::jlimit<juce::int64> (0, length, heard) / song->sampleRate;

    if (! reader.looping && readPosition >= length + delay)
    {
        playing = false;
        positionSeconds = (double) length / song->sampleRate;
    }
}

} // namespace pv
