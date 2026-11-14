#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "AudioTap.h"

namespace pv
{

/** A tiny internal processor used at both ends of the plugin chain:
    applies a (smoothed) gain, can mute, and records the peak level for meters.
*/
class GainMeterProcessor final : public juce::AudioProcessor
{
public:
    GainMeterProcessor (const juce::String& nameIn, int numChannels)
        : AudioProcessor (BusesProperties()
                              .withInput  ("Input",  juce::AudioChannelSet::canonicalChannelSet (numChannels), true)
                              .withOutput ("Output", juce::AudioChannelSet::canonicalChannelSet (numChannels), true)),
          name (nameIn)
    {
    }

    /** Linear gain, settable from any thread. */
    std::atomic<float> gain { 1.0f };

    /** When true the output is silent. */
    std::atomic<bool> muted { false };

    /** A second, independent mute (the tuner silences the guitar without touching "Monitor"). */
    std::atomic<bool> silenced { false };

    /** If set, gets a copy of channel 0 after the gain (the tuner listens to the input stage). */
    std::atomic<AudioTap*> tap { nullptr };

    /** Returns the peak level since the last call, and resets it. */
    float takePeak() noexcept { return peak.exchange (0.0f); }

    /** Same peak, for a second reader (the practice log listens for playing; the meter keeps its own). */
    float takeActivityPeak() noexcept { return activityPeak.exchange (0.0f); }

    //==============================================================================
    const juce::String getName() const override { return name; }

    void prepareToPlay (double rate, int) override  { sampleRate = rate; lastGain = targetGain(); }
    void releaseResources() override            {}

    bool isBusesLayoutSupported (const BusesLayout& layout) const override
    {
        return layout.getMainInputChannelSet() == layout.getMainOutputChannelSet()
            && ! layout.getMainInputChannelSet().isDisabled();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const auto numSamples = buffer.getNumSamples();
        const auto newGain = targetGain();

        buffer.applyGainRamp (0, numSamples, lastGain, newGain);
        lastGain = newGain;

        if (auto* t = tap.load (std::memory_order_acquire))
            t->push (buffer.getReadPointer (0), numSamples, sampleRate);

        const auto blockPeak = buffer.getMagnitude (0, numSamples);

        if (blockPeak > peak.load (std::memory_order_relaxed))
            peak.store (blockPeak, std::memory_order_relaxed);

        if (blockPeak > activityPeak.load (std::memory_order_relaxed))
            activityPeak.store (blockPeak, std::memory_order_relaxed);
    }

    using AudioProcessor::processBlock;

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
    float targetGain() const noexcept { return muted.load() || silenced.load() ? 0.0f : gain.load(); }

    juce::String name;
    float lastGain = 1.0f;
    double sampleRate = 44100.0;
    std::atomic<float> peak { 0.0f }, activityPeak { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GainMeterProcessor)
};

} // namespace pv
