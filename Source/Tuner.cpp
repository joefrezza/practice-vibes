#include "Tuner.h"

namespace pv
{

namespace
{
    constexpr int fftOrder = 13;        // 8192 = 2 * windowSize: linear (not circular) autocorrelation
    constexpr int fftSize = 1 << fftOrder;
    static_assert (fftSize == 2 * PitchDetector::windowSize, "FFT must be twice the window");
}

//==============================================================================
PitchDetector::PitchDetector()
    : fft (fftOrder),
      window ((size_t) windowSize),
      spectrum ((size_t) fftSize * 2),
      nsdf ((size_t) windowSize / 2 + 1)
{
}

PitchReading PitchDetector::analyse (const float* samples, int numSamples, double sampleRate)
{
    PitchReading result;
    const auto factor = decimationFor (sampleRate);
    const auto needed = windowSize * factor;

    if (sampleRate <= 0.0 || numSamples < needed)
        return result;

    const auto rate = sampleRate / factor;
    const auto* src = samples + (numSamples - needed);

    // Decimate (box average is enough to keep the fundamental) and remove DC.
    double mean = 0.0;

    for (int i = 0; i < windowSize; ++i)
    {
        float sum = 0.0f;

        for (int k = 0; k < factor; ++k)
            sum += src[i * factor + k];

        window[(size_t) i] = sum / (float) factor;
        mean += window[(size_t) i];
    }

    mean /= windowSize;
    double energy = 0.0;

    for (auto& s : window)
    {
        s -= (float) mean;
        energy += (double) s * s;
    }

    result.level = (float) std::sqrt (energy / windowSize);

    if (result.level < minLevel)
        return result;

    // Autocorrelation r(tau) via |FFT|^2.
    std::fill (spectrum.begin(), spectrum.end(), 0.0f);
    std::copy (window.begin(), window.end(), spectrum.begin());
    fft.performRealOnlyForwardTransform (spectrum.data());

    for (int k = 0; k < fftSize; ++k)
    {
        const auto re = spectrum[(size_t) (2 * k)], im = spectrum[(size_t) (2 * k + 1)];
        spectrum[(size_t) (2 * k)] = re * re + im * im;
        spectrum[(size_t) (2 * k + 1)] = 0.0f;
    }

    fft.performRealOnlyInverseTransform (spectrum.data());

    if (spectrum[0] <= 0.0f)
        return result;

    // The FFT engines scale differently: normalise with r(0), which we know exactly.
    const auto scale = energy / spectrum[0];
    const auto maxLag = windowSize / 2;

    // m(tau) = sum of x[j]^2 + x[j+tau]^2 over the overlap; nsdf = 2 r / m.
    double m = 2.0 * energy;
    nsdf[0] = 1.0f;

    for (int tau = 1; tau <= maxLag; ++tau)
    {
        const auto a = window[(size_t) (tau - 1)], b = window[(size_t) (windowSize - tau)];
        m -= (double) a * a + (double) b * b;
        nsdf[(size_t) tau] = m > 1.0e-12 ? (float) (2.0 * spectrum[(size_t) tau] * scale / m) : 0.0f;
    }

    const auto minLag = juce::jmax (2, (int) std::floor (rate / maxFrequency));
    const auto lastLag = juce::jmin (maxLag - 1, (int) std::ceil (rate / minFrequency));

    // Key maxima: the highest point of each positive lobe after the first negative-going zero crossing.
    std::vector<int> peaks;
    int tau = 1;

    while (tau < lastLag && nsdf[(size_t) tau] > 0.0f)
        ++tau;

    int best = -1;

    for (; tau < lastLag; ++tau)
    {
        if (nsdf[(size_t) tau] > 0.0f)
        {
            if (best < 0 || nsdf[(size_t) tau] > nsdf[(size_t) best])
                best = tau;
        }
        else if (best >= 0)
        {
            peaks.push_back (best);
            best = -1;
        }
    }

    if (best >= 0)
        peaks.push_back (best);

    float highest = 0.0f;

    for (auto p : peaks)
        if (p >= minLag)
            highest = juce::jmax (highest, nsdf[(size_t) p]);

    if (highest < minClarity)
    {
        result.clarity = highest;
        return result;
    }

    // The first peak close to the highest one: the fundamental period, not a multiple of it.
    const auto threshold = 0.93f * highest;

    for (auto p : peaks)
    {
        if (p < minLag || nsdf[(size_t) p] < threshold)
            continue;

        // Parabolic interpolation around the peak.
        const auto y0 = nsdf[(size_t) (p - 1)], y1 = nsdf[(size_t) p], y2 = nsdf[(size_t) (p + 1)];
        const auto denom = y0 - 2.0f * y1 + y2;
        const auto shift = std::abs (denom) > 1.0e-9f ? 0.5f * (y0 - y2) / denom : 0.0f;
        const auto period = (float) p + juce::jlimit (-0.5f, 0.5f, shift);

        result.clarity = y1 - 0.25f * (y0 - y2) * shift;
        result.frequency = (float) (rate / period);

        if (result.frequency < minFrequency || result.frequency > maxFrequency)
            result.frequency = 0.0f;

        break;
    }

    return result;
}

//==============================================================================
NoteInfo NoteInfo::fromFrequency (float frequency, float referenceA4)
{
    NoteInfo info;

    if (frequency <= 0.0f || referenceA4 <= 0.0f)
        return info;

    const auto exact = 69.0 + 12.0 * std::log2 ((double) frequency / referenceA4);
    info.midiNote = (int) std::round (exact);
    info.cents = (float) ((exact - info.midiNote) * 100.0);
    info.name = juce::MidiMessage::getMidiNoteName (info.midiNote, true, false, 4);
    info.octave = info.midiNote / 12 - 1;
    return info;
}

//==============================================================================
float PitchSmoother::add (const PitchReading& reading, double now)
{
    if (! reading.isValid())
    {
        if (now - lastValidTime > holdSeconds)
        {
            count = 0;
            shown = 0.0f;
        }

        return shown;
    }

    // A different note (more than a semitone away from the history): start over.
    if (count > 0 && std::abs (12.0f * std::log2 (reading.frequency / shown)) > 1.0f)
        count = 0;

    if (count == 0)
        next = 0;

    history[(size_t) next] = reading.frequency;
    next = (next + 1) % historySize;
    count = juce::jmin (count + 1, historySize);
    lastValidTime = now;

    std::array<float, historySize> sorted {};
    std::copy (history.begin(), history.begin() + count, sorted.begin());
    std::sort (sorted.begin(), sorted.begin() + count);
    shown = count % 2 == 1 ? sorted[(size_t) (count / 2)]
                           : 0.5f * (sorted[(size_t) (count / 2 - 1)] + sorted[(size_t) (count / 2)]);
    return shown;
}

//==============================================================================
Tuner::Tuner()
    : ring ((size_t) ringSize), scratch ((size_t) ringSize)
{
}

void Tuner::push (const float* samples, int numSamples, double sampleRate) noexcept
{
    if (! active.load (std::memory_order_relaxed) || numSamples <= 0)
        return;

    rate.store (sampleRate, std::memory_order_relaxed);
    auto pos = written.load (std::memory_order_relaxed);

    for (int i = 0; i < numSamples; ++i)
        ring[(size_t) ((pos + i) & (ringSize - 1))] = samples[i];

    written.store (pos + numSamples, std::memory_order_release);
}

PitchReading Tuner::read()
{
    const auto sampleRate = rate.load (std::memory_order_relaxed);
    const auto end = written.load (std::memory_order_acquire);

    if (sampleRate <= 0.0)
        return {};

    const auto needed = PitchDetector::samplesNeeded (sampleRate);

    if (end - clearedAt < needed || needed > ringSize)
        return {};

    for (int i = 0; i < needed; ++i)
        scratch[(size_t) i] = ring[(size_t) ((end - needed + i) & (ringSize - 1))];

    return detector.analyse (scratch.data(), needed, sampleRate);
}

void Tuner::clear()
{
    // `written` belongs to the audio thread: remember where we are instead, and read()
    // waits for a full new window after this point.
    clearedAt = written.load (std::memory_order_acquire);
}

std::unique_ptr<juce::XmlElement> Tuner::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("TUNER");
    xml->setAttribute ("referenceA4", (double) referenceA4);
    xml->setAttribute ("muteWhileTuning", muteWhileTuning);
    return xml;
}

void Tuner::fromXml (const juce::XmlElement& xml)
{
    referenceA4 = juce::jlimit (minReference, maxReference, (float) xml.getDoubleAttribute ("referenceA4", 440.0));
    muteWhileTuning = xml.getBoolAttribute ("muteWhileTuning", true);
}

} // namespace pv
