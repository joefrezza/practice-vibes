#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "MetronomeProcessor.h"

namespace pv
{

/** The host transport every plugin in the graph sees: tempo, time signature, bar position
    and play/stop taken from the metronome's bar clock (which the looper is locked to).
    Drum machines with their own patterns follow it; tempo-synced delays use its tempo.

    update() and getPosition() both run on the audio thread: update() once per device callback,
    after the clock advanced and before the graph runs; plugins ask during their processBlock.
    Beats are counted as quarter notes (the metronome's beats per bar over 4).
*/
class BarClockPlayHead final : public juce::AudioPlayHead
{
public:
    struct Snapshot
    {
        bool playing = false;
        double bpm = 120.0;
        int numerator = 4;
        double ppq = 0.0, ppqOfBarStart = 0.0;
        juce::int64 barCount = 0;
        juce::int64 timeInSamples = 0;
        double sampleRate = 48000.0;
    };

    /** Computes the position of the block starting at `blockStart` (absolute clock samples). */
    static Snapshot compute (const MetronomeProcessor& metronome, juce::int64 blockStart, double sampleRate)
    {
        Snapshot s;
        s.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;

        if (! metronome.isClockRunning())
        {
            s.bpm = metronome.startBpm.load();
            s.numerator = juce::jmax (1, metronome.beatsPerBar.load());
            return s;
        }

        const auto beatSamples = juce::jmax (1.0, metronome.getClockBeatSamples());
        const auto barStart = metronome.getClockBarStart();
        const auto beats = juce::jmax (1, metronome.getClockBeatsPerBar());
        auto position = ((double) blockStart - barStart) / beatSamples;

        if (position < 0.0)     // a bar line falls in this block: the part before it is in the previous bar's tempo
            position = ((double) blockStart - barStart) / juce::jmax (1.0, metronome.getClockPreviousBeatSamples());

        s.playing = true;
        s.bpm = 60.0 * s.sampleRate / beatSamples;
        s.numerator = beats;
        s.ppq = juce::jmax (0.0, metronome.getClockBarPpq() + position);
        s.ppqOfBarStart = metronome.getClockBarPpq();

        if (position < 0.0)
        {
            s.bpm = 60.0 * s.sampleRate / juce::jmax (1.0, metronome.getClockPreviousBeatSamples());
            s.ppqOfBarStart = juce::jmax (0.0, s.ppqOfBarStart - metronome.getClockPreviousBarBeats());
            s.numerator = juce::jmax (1, metronome.getClockPreviousBarBeats());
        }

        s.barCount = (juce::int64) std::floor (s.ppqOfBarStart / s.numerator + 1.0e-9);
        s.timeInSamples = (juce::int64) std::llround (s.ppq * beatSamples);
        return s;
    }

    /** Audio thread, once per device callback. */
    void update (const MetronomeProcessor& metronome, juce::int64 blockStart)
    {
        snapshot = compute (metronome, blockStart, metronome.getSampleRate());
    }

    juce::Optional<PositionInfo> getPosition() const override
    {
        const auto s = snapshot;

        PositionInfo info;
        info.setIsPlaying (s.playing);
        info.setIsRecording (false);
        info.setIsLooping (false);
        info.setBpm (s.bpm);
        info.setTimeSignature (TimeSignature { s.numerator, 4 });
        info.setPpqPosition (s.ppq);
        info.setPpqPositionOfLastBarStart (s.ppqOfBarStart);
        info.setBarCount (s.barCount);
        info.setTimeInSamples (s.timeInSamples);
        info.setTimeInSeconds ((double) s.timeInSamples / s.sampleRate);
        return info;
    }

private:
    Snapshot snapshot;
};

} // namespace pv
