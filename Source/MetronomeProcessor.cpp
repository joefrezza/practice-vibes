#include "MetronomeProcessor.h"

namespace pv
{

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

void MetronomeProcessor::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0 ? newSampleRate : 48000.0;
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

void MetronomeProcessor::onTick (int beatInBar, int tickInBeat)
{
    const auto isBeat = tickInBeat == 0;
    const auto isDownbeat = isBeat && beatInBar == 0 && accentFirstBeat.load() && beatsPerBar.load() > 1;
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

    if (! isBeat)
        return;

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
        tickIndex = 0;
        samplesUntilNextTick = 0.0;
        barsSinceStep = -1;     // becomes 0 on the first downbeat: counts *completed* bars
        secondsSinceStep = 0.0;

        for (auto& v : voices)
            v.active = false;
    }

    if (resetTempoRequested.exchange (false))
    {
        bpm = juce::jlimit (20.0, 400.0, startBpm.load());
        currentBpm = bpm;
        barsSinceStep = -1;
        secondsSinceStep = 0.0;
    }

    const auto numSamples = buffer.getNumSamples();
    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
    const auto targetGain = gain.load();
    const auto isRunning = running.load();

    for (int i = 0; i < numSamples; ++i)
    {
        if (isRunning && samplesUntilNextTick <= 0.0)
        {
            const auto barLength = juce::jmax (1, beatsPerBar.load());
            const auto ticks = ticksPerBeat ((Subdivision) subdivision.load());

            if (tickIndex >= ticks)
            {
                tickIndex = 0;
                ++beatIndex;
            }

            if (beatIndex >= barLength)
                beatIndex = 0;

            if (tickIndex == 0 && beatIndex == 0)
                ++barsSinceStep;

            onTick (beatIndex, tickIndex);
            ++tickIndex;
            samplesUntilNextTick += sampleRate * 60.0 / bpm / ticks;
        }

        if (isRunning)
            samplesUntilNextTick -= 1.0;

        // Voices keep ringing briefly after stop, so stopping never clicks.
        const auto g = lastGain + (targetGain - lastGain) * ((float) i / (float) numSamples);
        const auto sample = juce::jlimit (-1.0f, 1.0f, renderVoices() * g);
        left[i] = sample;

        if (right != nullptr)
            right[i] = sample;
    }

    lastGain = targetGain;

    if (isRunning)
        secondsSinceStep += numSamples / sampleRate;
}

} // namespace pv
