#include "MetronomeProcessor.h"

namespace pv
{

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

void MetronomeProcessor::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0 ? newSampleRate : 48000.0;
    clickDecay = (float) std::exp (-1.0 / (0.012 * sampleRate));   // ~12 ms decay constant
}

void MetronomeProcessor::onBeat (int beatInBar)
{
    const auto accent = beatInBar == 0 && accentFirstBeat.load() && beatsPerBar.load() > 1;

    clickPhase = 0.0;
    clickPhaseDelta = juce::MathConstants<double>::twoPi * (accent ? 1760.0 : 1100.0) / sampleRate;
    clickEnvelope = 1.0f;
    clickLevel = accent ? 1.0f : 0.6f;
    currentBeat = beatInBar;

    // Speed trainer: tempo steps happen on bar lines.
    if (beatInBar == 0 && trainerEnabled.load())
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
            barsSinceStep = 0;
            secondsSinceStep = 0.0;
        }
    }
}

void MetronomeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    buffer.clear();

    if (startRequested.exchange (false))
    {
        bpm = juce::jlimit (20.0, 400.0, startBpm.load());
        currentBpm = bpm;
        beatIndex = 0;
        samplesUntilNextBeat = 0.0;
        barsSinceStep = -1;     // becomes 0 on the first downbeat: counts *completed* bars
        secondsSinceStep = 0.0;
        clickEnvelope = 0.0f;
    }

    if (resetTempoRequested.exchange (false))
    {
        bpm = juce::jlimit (20.0, 400.0, startBpm.load());
        currentBpm = bpm;
        barsSinceStep = -1;
        secondsSinceStep = 0.0;
    }

    if (! running.load())
        return;

    const auto numSamples = buffer.getNumSamples();
    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
    const auto targetGain = gain.load();

    for (int i = 0; i < numSamples; ++i)
    {
        if (samplesUntilNextBeat <= 0.0)
        {
            const auto barLength = juce::jmax (1, beatsPerBar.load());

            if (beatIndex >= barLength)
                beatIndex = 0;

            if (beatIndex == 0)
                ++barsSinceStep;

            onBeat (beatIndex);
            ++beatIndex;
            samplesUntilNextBeat += sampleRate * 60.0 / bpm;
        }

        samplesUntilNextBeat -= 1.0;

        float sample = 0.0f;

        if (clickEnvelope > 0.0005f)
        {
            sample = (float) std::sin (clickPhase) * clickEnvelope * clickLevel;
            clickPhase += clickPhaseDelta;
            clickEnvelope *= clickDecay;
        }

        const auto g = lastGain + (targetGain - lastGain) * ((float) i / (float) numSamples);
        left[i] = sample * g;

        if (right != nullptr)
            right[i] = sample * g;
    }

    lastGain = targetGain;
    secondsSinceStep += numSamples / sampleRate;
}

} // namespace pv
