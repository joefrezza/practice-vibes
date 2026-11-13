#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <signalsmith-stretch/signalsmith-stretch.h>

namespace pv
{

/** A decoded song, held in memory as stereo float. */
struct LoadedSong
{
    juce::AudioBuffer<float> audio;     // always 2 channels
    double sampleRate = 44100.0;
    juce::String name;

    /** Waveform cache: min/max of the mono mix per bucket, at several resolutions
        (finest first, each level 4x coarser), so any zoom level draws quickly and sharply.
    */
    struct PeakLevel
    {
        int samplesPerBucket = 0;
        std::vector<float> mins, maxs;
    };

    std::vector<PeakLevel> peakLevels;

    double getLengthSeconds() const     { return audio.getNumSamples() / sampleRate; }

    /** Builds peakLevels from the audio. */
    void buildPeaks();
};

/** Plays a song with independent speed and pitch (Signalsmith Stretch), and A-B looping.

    Lives in the audio graph as a stereo source. Control methods are called from
    the message thread; parameters are handed to the audio thread via atomics.
*/
class PlayerProcessor final : public juce::AudioProcessor
{
public:
    PlayerProcessor();
    ~PlayerProcessor() override;

    //==============================================================================
    using LoadCallback = std::function<void (const juce::String& errorOrEmpty)>;

    /** Decodes the file on a background thread, then makes it the current song. */
    void loadAsync (const juce::URL& file, LoadCallback onDone);

    /** Makes an already-decoded song current (message thread). */
    void setSong (std::shared_ptr<LoadedSong>);

    std::shared_ptr<const LoadedSong> getSong() const  { return song; }
    bool hasSong() const                                { return song != nullptr; }

    void play();
    void pause();
    void stop();                                        // pause and go back to the start (or loop start)
    bool isPlaying() const noexcept                     { return playing.load(); }

    void seekToSeconds (double seconds);
    double getPositionSeconds() const noexcept          { return positionSeconds.load(); }
    double getLengthSeconds() const                     { return song != nullptr ? song->getLengthSeconds() : 0.0; }

    /** Playback speed, 1.0 = original. Pitch is not affected. */
    void setSpeed (double newSpeed)                     { speed = juce::jlimit (0.25, 2.0, newSpeed); }
    double getSpeed() const noexcept                    { return speed.load(); }

    /** Pitch shift in semitones (fractional allowed). Speed is not affected. */
    void setSemitones (double newSemitones)             { semitones = juce::jlimit (-12.0, 12.0, newSemitones); }
    double getSemitones() const noexcept                { return semitones.load(); }

    void setLoopSeconds (double startSeconds, double endSeconds);
    void setLoopEnabled (bool shouldLoop);
    bool isLoopEnabled() const noexcept                 { return loopEnabled.load(); }
    bool hasLoop() const noexcept                       { return loopEnd.load() > loopStart.load(); }
    double getLoopStartSeconds() const;
    double getLoopEndSeconds() const;

    std::atomic<float> gain { 1.0f };

    //==============================================================================
    const juce::String getName() const override         { return "Song player"; }
    void prepareToPlay (double sampleRate, int maximumBlockSize) override;
    void releaseResources() override                    {}
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
    juce::int64 secondsToSamples (double seconds) const;

    juce::AudioFormatManager formatManager;
    juce::ThreadPool loaderPool { 1 };

    // The song is swapped under this lock; the audio thread only ever try-locks it.
    juce::SpinLock songLock;
    std::shared_ptr<LoadedSong> song;

    signalsmith::stretch::SignalsmithStretch<float> stretch;
    double deviceSampleRate = 48000.0;
    double inputRemainder = 0.0;
    juce::int64 readPosition = 0;
    float lastGain = 1.0f;
    double lastTransposeFactor = 0.0;

    std::atomic<bool> playing { false };
    std::atomic<juce::int64> seekTarget { -1 };
    std::atomic<double> speed { 1.0 }, semitones { 0.0 }, positionSeconds { 0.0 };
    std::atomic<bool> loopEnabled { false };
    std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 };

    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayerProcessor)
};

} // namespace pv
