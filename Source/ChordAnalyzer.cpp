#include "ChordAnalyzer.h"
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>

namespace pv
{

// Tuning parameters (the developer tool can override them).
bool chordAnalyzerDebug = false;   // developer tool only: prints per-beat details
float slashBassRatio = 1.25f;      // how much stronger than the root a bass note must be to make a slash chord
float meterMargin = 0.02f;         // per-beat score advantage a meter needs over 4/4
float riffWeight = 2.0f;           // weight of "the music repeats after one bar" evidence for the meter
float meterSwitchCost = 2.0f;      // cost of changing meter between bars


//==============================================================================
int ChordTrack::chordIndexAt (double seconds) const
{
    auto it = std::upper_bound (chords.begin(), chords.end(), seconds,
                                [] (double t, const Chord& c) { return t < c.start; });

    if (it == chords.begin())
        return -1;

    const auto index = (int) std::distance (chords.begin(), it) - 1;
    return seconds < chords[(size_t) index].end ? index : -1;
}

namespace
{
    std::vector<double> gridLines (const ChordTrack& track, ChordTrack::GridUnit unit)
    {
        std::vector<double> lines { 0.0 };

        for (size_t i = 0; i < track.beats.size(); ++i)
            if (track.beats[i] > 0.0 && (unit == ChordTrack::GridUnit::beat || track.isDownbeat ((int) i)))
                lines.push_back (track.beats[i]);

        return lines;
    }
}

double ChordTrack::snapToGrid (double seconds, GridUnit unit) const
{
    if (beats.empty())
        return seconds;

    const auto lines = gridLines (*this, unit);
    auto it = std::lower_bound (lines.begin(), lines.end(), seconds);

    if (it == lines.end())
        return lines.back();

    if (it != lines.begin() && seconds - *(it - 1) < *it - seconds)
        --it;

    return *it;
}

double ChordTrack::neighbourGridLine (double seconds, GridUnit unit, int direction) const
{
    const auto lines = gridLines (*this, unit);

    if (direction > 0)
    {
        auto it = std::upper_bound (lines.begin(), lines.end(), seconds + 1.0e-6);
        return it != lines.end() ? *it : -1.0;
    }

    auto it = std::lower_bound (lines.begin(), lines.end(), seconds - 1.0e-6);
    return it != lines.begin() ? *(it - 1) : -1.0;
}

juce::String ChordTrack::noteName (int pitchClass, bool flats)
{
    static const char* sharpNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* flatNames[]  = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    pitchClass = ((pitchClass % 12) + 12) % 12;
    return flats ? flatNames[pitchClass] : sharpNames[pitchClass];
}

const char* ChordTrack::qualitySuffix (Quality q)
{
    switch (q)
    {
        case major:     return "";
        case minor:     return "m";
        case dominant7: return "7";
        case minor7:    return "m7";
        case major7:    return "maj7";
        case power:     return "5";
        case numQualities: break;
    }

    return "";
}

namespace
{
    bool keyUsesFlats (int keyRoot, bool minor)
    {
        const auto relativeMajor = ((keyRoot + (minor ? 3 : 0)) % 12 + 12) % 12;
        return relativeMajor == 5 || relativeMajor == 10 || relativeMajor == 3 || relativeMajor == 8 || relativeMajor == 1;
    }
}

juce::String ChordTrack::chordName (const Chord& chord, int semitones) const
{
    if (chord.root < 0)
        return "N.C.";

    const auto flats = keyUsesFlats (keyRoot + semitones, keyIsMinor);
    auto name = noteName (chord.root + semitones, flats) + qualitySuffix (chord.quality);

    if (chord.bass >= 0 && chord.bass != chord.root)
        name << "/" << noteName (chord.bass + semitones, flats);

    return name;
}

juce::String ChordTrack::keyName (int semitones) const
{
    return noteName (keyRoot + semitones, keyUsesFlats (keyRoot + semitones, keyIsMinor)) + (keyIsMinor ? " minor" : " major");
}

//==============================================================================
namespace
{
    constexpr double analysisRate = 11025.0;

    bool cancelled (const std::atomic<bool>* flag)   { return flag != nullptr && flag->load(); }

    /** Low-pass filters and decimates to roughly 11 kHz (all we need for pitch content). */
    std::vector<float> downsample (const float* input, int numSamples, double sampleRate, double& outRate)
    {
        const auto factor = juce::jmax (1, (int) std::lround (sampleRate / analysisRate));
        outRate = sampleRate / factor;

        if (factor == 1)
            return { input, input + numSamples };

        const auto half = 8 * factor;
        const auto cutoff = 0.45 / factor;      // cycles per input sample
        std::vector<float> taps ((size_t) (2 * half + 1));

        for (int k = -half; k <= half; ++k)
        {
            const auto x = 2.0 * cutoff * k;
            const auto sinc = k == 0 ? 1.0 : std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
            const auto n = (double) (k + half) / (2.0 * half);
            const auto blackman = 0.42 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * n) + 0.08 * std::cos (2.0 * juce::MathConstants<double>::twoPi * n);
            taps[(size_t) (k + half)] = (float) (2.0 * cutoff * sinc * blackman);
        }

        const auto outLength = numSamples / factor;
        std::vector<float> output ((size_t) outLength);

        for (int j = 0; j < outLength; ++j)
        {
            const auto centre = j * factor;
            const auto first = juce::jmax (0, centre - half);
            const auto last = juce::jmin (numSamples - 1, centre + half);
            auto sum = 0.0f;

            for (int i = first; i <= last; ++i)
                sum += taps[(size_t) (i - centre + half)] * input[i];

            output[(size_t) j] = sum;
        }

        return output;
    }

    /** Magnitude spectrogram with centred frames: frame k is centred on sample k * hop. */
    struct Spectrogram
    {
        int fftSize = 0, hop = 0, numFrames = 0, numBins = 0;
        std::vector<float> magnitudes;

        const float* frame (int k) const    { return magnitudes.data() + (size_t) k * (size_t) numBins; }
    };

    Spectrogram computeSpectrogram (const std::vector<float>& signal, int order, int hop, const std::atomic<bool>* cancel)
    {
        Spectrogram s;
        s.fftSize = 1 << order;
        s.hop = hop;
        s.numBins = s.fftSize / 2 + 1;
        s.numFrames = (int) signal.size() / hop + 1;
        s.magnitudes.resize ((size_t) s.numFrames * (size_t) s.numBins);

        juce::dsp::FFT fft (order);
        std::vector<float> window ((size_t) s.fftSize), buffer ((size_t) s.fftSize * 2);

        for (int i = 0; i < s.fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * (float) std::cos (juce::MathConstants<double>::twoPi * i / s.fftSize);

        const auto length = (int) signal.size();

        for (int k = 0; k < s.numFrames; ++k)
        {
            if ((k & 255) == 0 && cancelled (cancel))
                return {};

            std::fill (buffer.begin(), buffer.end(), 0.0f);
            const auto start = k * hop - s.fftSize / 2;

            for (int i = 0; i < s.fftSize; ++i)
            {
                const auto index = start + i;

                if (index >= 0 && index < length)
                    buffer[(size_t) i] = signal[(size_t) index] * window[(size_t) i];
            }

            fft.performFrequencyOnlyForwardTransform (buffer.data(), true);
            std::copy (buffer.begin(), buffer.begin() + s.numBins, s.magnitudes.begin() + (ptrdiff_t) ((size_t) k * (size_t) s.numBins));
        }

        return s;
    }

    struct Peak { float frequency, magnitude; };

    /** Spectral peaks of one frame between minHz and maxHz, frequency refined by parabolic interpolation. */
    void findPeaks (const float* mags, int numBins, double binHz, double minHz, double maxHz, float relativeThreshold, std::vector<Peak>& peaks)
    {
        peaks.clear();
        const auto first = juce::jmax (2, (int) (minHz / binHz));
        const auto last = juce::jmin (numBins - 2, (int) (maxHz / binHz));
        auto maxMag = 0.0f;

        for (int b = first; b <= last; ++b)
            maxMag = juce::jmax (maxMag, mags[b]);

        if (maxMag <= 1.0e-9f)
            return;

        const auto threshold = maxMag * relativeThreshold;

        for (int b = first; b <= last; ++b)
        {
            const auto m = mags[b];

            if (m < threshold || m <= mags[b - 1] || m < mags[b + 1])
                continue;

            const auto a = std::log (mags[b - 1] + 1.0e-9f), c = std::log (mags[b + 1] + 1.0e-9f), l = std::log (m + 1.0e-9f);
            const auto denominator = a - 2.0f * l + c;
            const auto offset = std::abs (denominator) > 1.0e-9f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / denominator) : 0.0f;
            peaks.push_back ({ (float) ((b + offset) * binHz), m });
        }
    }

    double frequencyToMidi (double hz)   { return 69.0 + 12.0 * std::log2 (hz / 440.0); }

    //==========================================================================
    // Beat tracking (onset envelope -> tempo -> dynamic programming, after Ellis 2007)

    struct BeatGrid
    {
        std::vector<double> beats;
        double bpm = 120.0;
    };

    BeatGrid trackBeats (const std::vector<float>& signal, double rate, const std::atomic<bool>* cancel)
    {
        constexpr int order = 10, hop = 128;
        const auto spec = computeSpectrogram (signal, order, hop, cancel);
        BeatGrid grid;
        const auto length = (double) signal.size() / rate;

        if (spec.numFrames < 16)
            return grid;

        const auto frameRate = rate / hop;
        const auto frames = spec.numFrames;

        // Onset strength: positive log-spectral flux.
        std::vector<float> onset ((size_t) frames, 0.0f), previous ((size_t) spec.numBins, 0.0f), current ((size_t) spec.numBins);

        for (int k = 0; k < frames; ++k)
        {
            const auto* mags = spec.frame (k);
            auto flux = 0.0f;

            for (int b = 1; b < spec.numBins; ++b)
            {
                current[(size_t) b] = std::log1p (10.0f * mags[b]);

                if (k > 0)
                    flux += juce::jmax (0.0f, current[(size_t) b] - previous[(size_t) b]);
            }

            onset[(size_t) k] = flux;
            std::swap (current, previous);
        }

        // Remove the slowly varying part, rectify, normalise.
        {
            const auto half = (int) (frameRate * 0.5);
            std::vector<double> prefix ((size_t) frames + 1, 0.0);

            for (int k = 0; k < frames; ++k)
                prefix[(size_t) k + 1] = prefix[(size_t) k] + onset[(size_t) k];

            std::vector<float> detrended ((size_t) frames);

            for (int k = 0; k < frames; ++k)
            {
                const auto a = juce::jmax (0, k - half), b = juce::jmin (frames, k + half + 1);
                const auto mean = (prefix[(size_t) b] - prefix[(size_t) a]) / (b - a);
                detrended[(size_t) k] = (float) juce::jmax (0.0, onset[(size_t) k] - mean);
            }

            auto sumSquares = 0.0;

            for (auto v : detrended)
                sumSquares += (double) v * v;

            const auto rms = std::sqrt (sumSquares / frames);

            if (rms <= 1.0e-9)
                return grid;

            for (int k = 0; k < frames; ++k)
                onset[(size_t) k] = (float) (detrended[(size_t) k] / rms);
        }

        // Tempo: weighted autocorrelation, preferring ~110 bpm.
        const auto minLag = (int) std::floor (frameRate * 60.0 / 220.0);
        const auto maxLag = (int) std::ceil (frameRate * 60.0 / 45.0);
        std::vector<double> acf ((size_t) maxLag + 2, 0.0);

        for (int lag = minLag - 1; lag <= maxLag + 1; ++lag)
        {
            auto sum = 0.0;

            for (int k = lag; k < frames; ++k)
                sum += (double) onset[(size_t) k] * onset[(size_t) (k - lag)];

            acf[(size_t) lag] = sum / (frames - lag);
        }

        auto bestLag = minLag;
        auto bestScore = -1.0;

        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            const auto bpm = 60.0 * frameRate / lag;
            const auto octaves = std::log2 (bpm / 110.0);
            const auto weight = std::exp (-0.5 * octaves * octaves / (0.8 * 0.8));
            const auto score = acf[(size_t) lag] * weight;

            if (score > bestScore)
            {
                bestScore = score;
                bestLag = lag;
            }
        }

        auto period = (double) bestLag;
        {
            const auto a = acf[(size_t) bestLag - 1], b = acf[(size_t) bestLag], c = acf[(size_t) bestLag + 1];
            const auto denominator = a - 2.0 * b + c;

            if (std::abs (denominator) > 1.0e-12)
                period += juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / denominator);
        }

        grid.bpm = 60.0 * frameRate / period;

        // Dynamic programming beat placement.
        std::vector<float> local ((size_t) frames, 0.0f);
        {
            const auto sigma = juce::jmax (1.0, period / 32.0);
            const auto radius = (int) std::ceil (3.0 * sigma);

            for (int k = 0; k < frames; ++k)
            {
                auto sum = 0.0;

                for (int j = -radius; j <= radius; ++j)
                    if (k + j >= 0 && k + j < frames)
                        sum += onset[(size_t) (k + j)] * std::exp (-0.5 * j * j / (sigma * sigma));

                local[(size_t) k] = (float) sum;
            }
        }

        std::vector<double> score ((size_t) frames);
        std::vector<int> backlink ((size_t) frames, -1);
        constexpr double tightness = 100.0;
        const auto searchMin = (int) std::round (period * 0.5), searchMax = (int) std::round (period * 2.0);

        for (int k = 0; k < frames; ++k)
        {
            auto best = -1.0e30;
            auto bestPrevious = -1;

            for (int previousFrame = k - searchMax; previousFrame <= k - searchMin; ++previousFrame)
            {
                if (previousFrame < 0)
                    continue;

                const auto ratio = std::log ((k - previousFrame) / period);
                const auto candidate = score[(size_t) previousFrame] - tightness * ratio * ratio;

                if (candidate > best)
                {
                    best = candidate;
                    bestPrevious = previousFrame;
                }
            }

            score[(size_t) k] = local[(size_t) k] + (bestPrevious >= 0 ? juce::jmax (0.0, best) : 0.0);
            backlink[(size_t) k] = bestPrevious >= 0 && best > 0.0 ? bestPrevious : -1;
        }

        auto last = frames - 1;

        for (int k = juce::jmax (0, frames - (int) std::ceil (period)); k < frames; ++k)
            if (score[(size_t) k] > score[(size_t) last])
                last = k;

        std::vector<double> beats;

        for (int k = last; k >= 0; k = backlink[(size_t) k])
            beats.push_back (k / frameRate);

        std::reverse (beats.begin(), beats.end());

        // Extend the grid over the whole song so every part gets a chord slot.
        const auto beatSeconds = period / frameRate;

        while (! beats.empty() && beats.front() - beatSeconds > -0.5 * beatSeconds)
            beats.insert (beats.begin(), juce::jmax (0.0, beats.front() - beatSeconds));

        while (! beats.empty() && beats.back() + beatSeconds < length)
            beats.push_back (beats.back() + beatSeconds);

        grid.beats = std::move (beats);
        return grid;
    }

    //==========================================================================
    // Chord templates

    /** Subtracts the mean and scales to unit length, so a dot product is a correlation. Returns the length before scaling. */
    float centreAndNormalise (std::array<float, 12>& v)
    {
        const auto mean = std::accumulate (v.begin(), v.end(), 0.0f) / 12.0f;
        auto norm = 0.0f;

        for (auto& x : v)
        {
            x -= mean;
            norm += x * x;
        }

        norm = std::sqrt (norm);

        if (norm > 1.0e-9f)
            for (auto& x : v)
                x /= norm;

        return norm;
    }

    /** Semitones above the root, per ChordTrack::Quality. */
    const std::array<std::vector<int>, ChordTrack::numQualities>& chordIntervals()
    {
        static const std::array<std::vector<int>, ChordTrack::numQualities> intervals {{
            { 0, 4, 7 }, { 0, 3, 7 }, { 0, 4, 7, 10 }, { 0, 3, 7, 10 }, { 0, 4, 7, 11 }, { 0, 7 }
        }};
        return intervals;
    }

    struct ChordModel
    {
        static constexpr int numQualities = ChordTrack::numQualities;
        static constexpr int numStates = 12 * numQualities + 1;   // last = no chord

        std::array<std::array<float, 12>, numStates - 1> templates {};
        std::array<float, numQualities> qualityPenalty { 0.0f, 0.0f, 0.04f, 0.04f, 0.05f, 0.05f };

        ChordModel()
        {
            const auto& intervals = chordIntervals();

            // Each chord tone contributes its first harmonics (octave, fifth, ...), decaying.
            static const int harmonicOffsets[] = { 0, 12, 19, 24, 28, 31 };

            for (int q = 0; q < numQualities; ++q)
            {
                for (int root = 0; root < 12; ++root)
                {
                    auto& t = templates[(size_t) (q * 12 + root)];
                    t.fill (0.0f);

                    for (auto interval : intervals[(size_t) q])
                    {
                        auto weight = 1.0f;

                        for (auto offset : harmonicOffsets)
                        {
                            t[(size_t) ((root + interval + offset) % 12)] += weight;
                            weight *= 0.6f;
                        }
                    }

                    centreAndNormalise (t);
                }
            }
        }

        static int rootOf (int state)                       { return state == numStates - 1 ? -1 : state % 12; }
        static ChordTrack::Quality qualityOf (int state)    { return (ChordTrack::Quality) (state == numStates - 1 ? 0 : state / 12); }
    };

    /** Like barViterbi, but the meter is part of the state and may change at a bar line
        (at a cost), so songs that switch e.g. between 7 and 4 get both right.
        `meterEvidence[i][k]` is added for beat i while in meters[k].
    */
    template <size_t F>
    void multiMeterViterbi (const std::vector<std::array<float, F>>& emission, const std::vector<int>& meters,
                            const std::vector<std::vector<float>>& meterEvidence,
                            std::vector<int>& familyPath, std::vector<int>& positionPath, std::vector<int>& meterPath)
    {
        const auto numBeats = (int) emission.size();
        familyPath.assign ((size_t) numBeats, (int) F - 1);
        positionPath.assign ((size_t) numBeats, 0);
        meterPath.assign ((size_t) numBeats, meters.empty() ? 4 : meters.front());

        if (numBeats == 0 || meters.empty())
            return;

        // Slots enumerate (meter, position).
        struct Slot { int meterIndex, position, previous; float changePenalty; };
        std::vector<Slot> slots;
        std::vector<int> firstSlot;

        for (int k = 0; k < (int) meters.size(); ++k)
        {
            const auto m = meters[(size_t) k];
            firstSlot.push_back ((int) slots.size());

            for (int position = 0; position < m; ++position)
            {
                auto penalty = position == 0 ? 0.35f : 1.1f;

                if (m % 2 == 0 && position == m / 2)
                    penalty = 0.6f;

                slots.push_back ({ k, position, 0, penalty });
            }
        }

        for (auto& slot : slots)
        {
            const auto m = meters[(size_t) slot.meterIndex];
            slot.previous = firstSlot[(size_t) slot.meterIndex] + (slot.position + m - 1) % m;
        }

        const auto numSlots = (int) slots.size();
        const auto numStates = (int) F * numSlots;
        const auto numMeters = (int) meters.size();
        constexpr float phaseJump = 0.8f;
        const auto meterSwitch = meterSwitchCost;

        std::vector<double> delta ((size_t) numStates), next ((size_t) numStates);
        std::vector<int> from ((size_t) numBeats * (size_t) numStates, -1);

        for (int f = 0; f < (int) F; ++f)
            for (int sl = 0; sl < numSlots; ++sl)
                delta[(size_t) (f * numSlots + sl)] = emission[0][(size_t) f] + meterEvidence[0][(size_t) slots[(size_t) sl].meterIndex]
                                                       - (slots[(size_t) sl].position == 0 ? 0.0 : 0.01);

        std::vector<double> bestAtSlot ((size_t) numSlots), bestFamilyMeter ((size_t) F * (size_t) numMeters), bestMeter ((size_t) numMeters),
                            bestBarEndFamily (F);
        std::vector<int> argAtSlot ((size_t) numSlots), argFamilyMeter ((size_t) F * (size_t) numMeters), argMeter ((size_t) numMeters),
                         argBarEndFamily (F);

        for (int i = 1; i < numBeats; ++i)
        {
            std::fill (bestAtSlot.begin(), bestAtSlot.end(), -1.0e30);
            std::fill (bestFamilyMeter.begin(), bestFamilyMeter.end(), -1.0e30);
            std::fill (bestMeter.begin(), bestMeter.end(), -1.0e30);
            std::fill (bestBarEndFamily.begin(), bestBarEndFamily.end(), -1.0e30);
            auto bestBarEnd = -1.0e30;
            auto argBarEnd = 0;

            for (int f = 0; f < (int) F; ++f)
            {
                for (int sl = 0; sl < numSlots; ++sl)
                {
                    const auto state = f * numSlots + sl;
                    const auto d = delta[(size_t) state];
                    const auto& slot = slots[(size_t) sl];
                    const auto fm = (size_t) (f * numMeters + slot.meterIndex);

                    if (d > bestAtSlot[(size_t) sl])                    { bestAtSlot[(size_t) sl] = d; argAtSlot[(size_t) sl] = state; }
                    if (d > bestFamilyMeter[fm])                         { bestFamilyMeter[fm] = d; argFamilyMeter[fm] = state; }
                    if (d > bestMeter[(size_t) slot.meterIndex])         { bestMeter[(size_t) slot.meterIndex] = d; argMeter[(size_t) slot.meterIndex] = state; }

                    if (slot.position == meters[(size_t) slot.meterIndex] - 1)
                    {
                        if (d > bestBarEndFamily[(size_t) f])            { bestBarEndFamily[(size_t) f] = d; argBarEndFamily[(size_t) f] = state; }
                        if (d > bestBarEnd)                              { bestBarEnd = d; argBarEnd = state; }
                    }
                }
            }

            auto* back = from.data() + (size_t) i * (size_t) numStates;

            for (int f = 0; f < (int) F; ++f)
            {
                for (int sl = 0; sl < numSlots; ++sl)
                {
                    const auto& slot = slots[(size_t) sl];
                    const auto stay = f * numSlots + slot.previous;
                    const auto fm = (size_t) (f * numMeters + slot.meterIndex);

                    auto best = delta[(size_t) stay];
                    auto arg = stay;

                    auto consider = [&best, &arg] (double value, int origin)
                    {
                        if (value > best)
                        {
                            best = value;
                            arg = origin;
                        }
                    };

                    consider (bestAtSlot[(size_t) slot.previous] - slot.changePenalty, argAtSlot[(size_t) slot.previous]);
                    consider (bestFamilyMeter[fm] - phaseJump, argFamilyMeter[fm]);
                    consider (bestMeter[(size_t) slot.meterIndex] - phaseJump - slot.changePenalty, argMeter[(size_t) slot.meterIndex]);

                    if (slot.position == 0)
                    {
                        consider (bestBarEndFamily[(size_t) f] - meterSwitch, argBarEndFamily[(size_t) f]);
                        consider (bestBarEnd - meterSwitch - slot.changePenalty, argBarEnd);
                    }

                    next[(size_t) (f * numSlots + sl)] = best + emission[(size_t) i][(size_t) f]
                                                         + meterEvidence[(size_t) i][(size_t) slot.meterIndex];
                    back[f * numSlots + sl] = arg;
                }
            }

            std::swap (delta, next);
        }

        auto state = (int) std::distance (delta.begin(), std::max_element (delta.begin(), delta.end()));

        for (int i = numBeats - 1; i >= 0; --i)
        {
            const auto& slot = slots[(size_t) (state % numSlots)];
            familyPath[(size_t) i] = state / numSlots;
            positionPath[(size_t) i] = slot.position;
            meterPath[(size_t) i] = meters[(size_t) slot.meterIndex];

            if (i > 0)
                state = from[(size_t) i * (size_t) numStates + (size_t) state];
        }
    }

    /** How strongly the music repeats itself after `lag` beats, compared with the
        neighbouring lags, averaged over a window around each beat. Positive only. */
    std::vector<std::vector<float>> riffEvidence (const std::vector<std::array<float, 12>>& treble,
                                                  const std::vector<std::array<float, 12>>& bass,
                                                  const std::vector<int>& meters, int halfWindow)
    {
        const auto n = (int) treble.size();
        const auto maxLag = *std::max_element (meters.begin(), meters.end()) + 1;
        std::vector<std::vector<double>> prefix ((size_t) maxLag + 1, std::vector<double> ((size_t) n + 1, 0.0));

        for (int lag = 1; lag <= maxLag; ++lag)
        {
            for (int i = 0; i < n; ++i)
            {
                auto similarity = 0.0;

                if (i + lag < n)
                {
                    auto dot = 0.0, na = 0.0, nb = 0.0;

                    for (size_t c = 0; c < 12; ++c)
                    {
                        const double a1 = bass[(size_t) i][c], b1 = bass[(size_t) (i + lag)][c];
                        const double a2 = treble[(size_t) i][c], b2 = treble[(size_t) (i + lag)][c];
                        dot += a1 * b1 + a2 * b2;
                        na += a1 * a1 + a2 * a2;
                        nb += b1 * b1 + b2 * b2;
                    }

                    similarity = dot / std::sqrt (na * nb + 1.0e-12);
                }

                prefix[(size_t) lag][(size_t) i + 1] = prefix[(size_t) lag][(size_t) i] + similarity;
            }
        }

        std::vector<std::vector<float>> evidence ((size_t) n, std::vector<float> (meters.size(), 0.0f));

        for (int i = 0; i < n; ++i)
        {
            auto windowMean = [&] (int lag)
            {
                const auto first = juce::jmax (0, i - halfWindow);
                const auto last = juce::jmin (n - lag, i + halfWindow + 1);   // pairs (j, j + lag) must exist
                return last > first ? (prefix[(size_t) lag][(size_t) last] - prefix[(size_t) lag][(size_t) first]) / (last - first) : 0.0;
            };

            for (size_t k = 0; k < meters.size(); ++k)
            {
                const auto m = meters[k];
                const auto peak = windowMean (m) - juce::jmax (windowMean (m - 1), windowMean (m + 1));
                evidence[(size_t) i][k] = (float) juce::jmax (0.0, peak);
            }
        }

        return evidence;
    }

    /** Viterbi over (chord family, position in bar). The position advances by one each beat;
        changing chord costs least on the downbeat. A costly "phase jump" lets the bar grid
        recover where the beat tracker added or dropped a beat.
    */
    template <size_t F>
    double barViterbi (const std::vector<std::array<float, F>>& emission, int beatsPerBar,
                       std::vector<int>& familyPath, std::vector<int>& positionPath)
    {
        const auto numBeats = (int) emission.size();
        const auto P = beatsPerBar;
        const auto numStates = (int) F * P;
        familyPath.assign ((size_t) numBeats, (int) F - 1);
        positionPath.assign ((size_t) numBeats, 0);

        if (numBeats == 0)
            return 0.0;

        std::vector<float> changePenalty ((size_t) P, 1.1f);
        changePenalty[0] = 0.35f;

        if (P % 2 == 0)
            changePenalty[(size_t) (P / 2)] = 0.6f;

        constexpr float phaseJump = 0.8f;

        std::vector<double> delta ((size_t) numStates), next ((size_t) numStates);
        std::vector<int> from ((size_t) numBeats * (size_t) numStates, -1);

        for (int f = 0; f < (int) F; ++f)
            for (int p = 0; p < P; ++p)
                delta[(size_t) (f * P + p)] = emission[0][(size_t) f] - (p == 0 ? 0.0 : 0.01);

        std::vector<double> bestAtPosition ((size_t) P), bestForFamily (F);
        std::vector<int> argAtPosition ((size_t) P), argForFamily (F);

        for (int i = 1; i < numBeats; ++i)
        {
            std::fill (bestAtPosition.begin(), bestAtPosition.end(), -1.0e30);
            std::fill (bestForFamily.begin(), bestForFamily.end(), -1.0e30);
            auto globalBest = -1.0e30;
            auto globalArg = 0;

            for (int f = 0; f < (int) F; ++f)
            {
                for (int p = 0; p < P; ++p)
                {
                    const auto state = f * P + p;
                    const auto d = delta[(size_t) state];

                    if (d > bestAtPosition[(size_t) p])  { bestAtPosition[(size_t) p] = d; argAtPosition[(size_t) p] = state; }
                    if (d > bestForFamily[(size_t) f])   { bestForFamily[(size_t) f] = d; argForFamily[(size_t) f] = state; }
                    if (d > globalBest)                  { globalBest = d; globalArg = state; }
                }
            }

            auto* back = from.data() + (size_t) i * (size_t) numStates;

            for (int f = 0; f < (int) F; ++f)
            {
                for (int p = 0; p < P; ++p)
                {
                    const auto previousPosition = (p + P - 1) % P;
                    const auto stay = f * P + previousPosition;

                    auto best = delta[(size_t) stay];
                    auto arg = stay;

                    if (const auto change = bestAtPosition[(size_t) previousPosition] - changePenalty[(size_t) p]; change > best)
                    {
                        best = change;
                        arg = argAtPosition[(size_t) previousPosition];
                    }

                    if (const auto jump = bestForFamily[(size_t) f] - phaseJump; jump > best)
                    {
                        best = jump;
                        arg = argForFamily[(size_t) f];
                    }

                    if (const auto jumpAndChange = globalBest - phaseJump - changePenalty[(size_t) p]; jumpAndChange > best)
                    {
                        best = jumpAndChange;
                        arg = globalArg;
                    }

                    next[(size_t) (f * P + p)] = best + emission[(size_t) i][(size_t) f];
                    back[f * P + p] = arg;
                }
            }

            std::swap (delta, next);
        }

        auto state = (int) std::distance (delta.begin(), std::max_element (delta.begin(), delta.end()));
        const auto total = delta[(size_t) state];

        for (int i = numBeats - 1; i >= 0; --i)
        {
            familyPath[(size_t) i] = state / P;
            positionPath[(size_t) i] = state % P;

            if (i > 0)
                state = from[(size_t) i * (size_t) numStates + (size_t) state];
        }

        return total;
    }

    int estimateKey (const std::array<double, 12>& chroma, bool& isMinor)
    {
        static const double majorProfile[] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static const double minorProfile[] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

        auto correlate = [&chroma] (const double* profile, int root)
        {
            auto meanC = 0.0, meanP = 0.0;

            for (int i = 0; i < 12; ++i)
            {
                meanC += chroma[(size_t) i] / 12.0;
                meanP += profile[i] / 12.0;
            }

            auto num = 0.0, dc = 0.0, dp = 0.0;

            for (int i = 0; i < 12; ++i)
            {
                const auto c = chroma[(size_t) ((i + root) % 12)] - meanC, p = profile[i] - meanP;
                num += c * p;
                dc += c * c;
                dp += p * p;
            }

            return num / std::sqrt (dc * dp + 1.0e-12);
        };

        auto best = -2.0;
        auto bestRoot = 0;
        isMinor = false;

        for (int root = 0; root < 12; ++root)
        {
            for (int minor = 0; minor < 2; ++minor)
            {
                const auto r = correlate (minor ? minorProfile : majorProfile, root);

                if (r > best)
                {
                    best = r;
                    bestRoot = root;
                    isMinor = minor != 0;
                }
            }
        }

        return bestRoot;
    }
}

//==============================================================================

ChordTrack analyseChords (const float* mono, int numSamples, double sampleRate, const std::atomic<bool>* cancel)
{
    ChordTrack track;

    if (mono == nullptr || numSamples < (int) sampleRate || sampleRate <= 0.0)
        return track;

    auto rate = analysisRate;
    const auto signal = downsample (mono, numSamples, sampleRate, rate);
    const auto lengthSeconds = (double) signal.size() / rate;

    if (cancelled (cancel))
        return {};

    // --- Beats ------------------------------------------------------------------
    const auto grid = trackBeats (signal, rate, cancel);

    if (cancelled (cancel) || grid.beats.size() < 2)
        return {};

    track.beats = grid.beats;
    track.bpm = grid.bpm;

    // --- Pitch spectrogram, tuning --------------------------------------------
    constexpr int order = 12, hop = 512;
    const auto spec = computeSpectrogram (signal, order, hop, cancel);

    if (cancelled (cancel) || spec.numFrames == 0)
        return {};

    const auto binHz = rate / spec.fftSize;
    std::vector<Peak> peaks;

    auto tuning = 0.0;   // semitones
    {
        auto re = 0.0, im = 0.0;

        for (int k = 0; k < spec.numFrames; k += 2)
        {
            findPeaks (spec.frame (k), spec.numBins, binHz, 100.0, 2000.0, 0.1f, peaks);

            for (const auto& p : peaks)
            {
                const auto midi = frequencyToMidi (p.frequency);
                const auto deviation = midi - std::round (midi);
                re += p.magnitude * std::cos (juce::MathConstants<double>::twoPi * deviation);
                im += p.magnitude * std::sin (juce::MathConstants<double>::twoPi * deviation);
            }
        }

        tuning = std::atan2 (im, re) / juce::MathConstants<double>::twoPi;
        track.tuningCents = tuning * 100.0;
    }

    // --- Note salience -> treble and bass chroma per frame ------------------
    constexpr int lowNote = 28, highNote = 96, numNotes = highNote - lowNote;
    std::vector<std::array<float, 12>> treble ((size_t) spec.numFrames), bass ((size_t) spec.numFrames);
    std::vector<float> frameEnergy ((size_t) spec.numFrames);
    {
        std::vector<float> salience ((size_t) spec.numFrames * numNotes, 0.0f);
        auto maxSalience = 1.0e-9f;

        for (int k = 0; k < spec.numFrames; ++k)
        {
            if ((k & 255) == 0 && cancelled (cancel))
                return {};

            findPeaks (spec.frame (k), spec.numBins, binHz, 38.0, 4200.0, 0.01f, peaks);
            auto* s = salience.data() + (size_t) k * numNotes;
            auto energy = 0.0f;

            for (const auto& p : peaks)
            {
                const auto midi = frequencyToMidi (p.frequency) - tuning;
                const auto note = (int) std::lround (midi);

                if (note < lowNote || note >= highNote)
                    continue;

                const auto c = std::cos (juce::MathConstants<float>::pi * (float) (midi - note));
                s[note - lowNote] += p.magnitude * c * c;
                energy += p.magnitude;
            }

            frameEnergy[(size_t) k] = energy;

            for (int n = 0; n < numNotes; ++n)
                maxSalience = juce::jmax (maxSalience, s[n]);
        }

        const auto scale = 1000.0f / maxSalience;

        for (int k = 0; k < spec.numFrames; ++k)
        {
            const auto* s = salience.data() + (size_t) k * numNotes;
            treble[(size_t) k].fill (0.0f);
            bass[(size_t) k].fill (0.0f);

            for (int n = 0; n < numNotes; ++n)
            {
                const auto note = n + lowNote;
                const auto value = std::log1p (scale * s[n]);
                const auto pc = (size_t) (note % 12);

                // Treble: roughly G2..C7, tapered at the ends. Bass: roughly E1..A3.
                const auto t = std::exp (-0.5f * juce::square ((float) (note - 64) / 14.0f));
                const auto b = std::exp (-0.5f * juce::square ((float) (note - 40) / 7.0f));

                if (note >= 43)
                    treble[(size_t) k][pc] += value * t;

                if (note <= 57)
                    bass[(size_t) k][pc] += value * b;
            }
        }
    }

    // --- Beat-synchronous features and chord scores, for a given beat grid ----
    static const ChordModel model;
    constexpr auto S = ChordModel::numStates;
    constexpr size_t F = 25;

    auto familyMembers = [] (int family)
    {
        using Q = ChordTrack::Quality;
        if (family < 12)  return std::vector<Q> { ChordTrack::major, ChordTrack::dominant7, ChordTrack::major7, ChordTrack::power };
        if (family < 24)  return std::vector<Q> { ChordTrack::minor, ChordTrack::minor7, ChordTrack::power };
        return std::vector<Q>();
    };

    struct BeatFeatures
    {
        std::vector<std::array<float, 12>> treble, bass;
        std::vector<float> energy;
        float medianEnergy = 1.0f;
        std::vector<std::array<float, S>> emission;
        std::vector<std::array<float, F>> family;
        std::array<double, 12> keyChroma {};
    };

    auto computeFeatures = [&] (const std::vector<double>& beats)
    {
        BeatFeatures f;
        const auto numBeats = (int) beats.size();
        auto& beatTreble = f.treble;
        auto& beatBass = f.bass;
        beatTreble.resize ((size_t) numBeats);
        beatBass.resize ((size_t) numBeats);
        auto& beatEnergy = f.energy;
        beatEnergy.resize ((size_t) numBeats);
        const auto framesPerSecond = rate / hop;

        for (int i = 0; i < numBeats; ++i)
        {
            const auto start = beats[(size_t) i];
            const auto end = i + 1 < numBeats ? beats[(size_t) i + 1] : lengthSeconds;
            auto first = (int) std::ceil (start * framesPerSecond);
            auto last = (int) std::ceil (end * framesPerSecond);   // exclusive
            first = juce::jlimit (0, spec.numFrames - 1, first);
            last = juce::jlimit (first + 1, spec.numFrames, last);

            beatTreble[(size_t) i].fill (0.0f);
            beatBass[(size_t) i].fill (0.0f);
            auto energy = 0.0f;

            for (int k = first; k < last; ++k)
            {
                for (size_t c = 0; c < 12; ++c)
                {
                    beatTreble[(size_t) i][c] += treble[(size_t) k][c];
                    beatBass[(size_t) i][c] += bass[(size_t) k][c];
                }

                energy += frameEnergy[(size_t) k];
            }

            beatEnergy[(size_t) i] = energy / (float) (last - first);
        }

        std::vector<float> sortedEnergy (beatEnergy);
        std::sort (sortedEnergy.begin(), sortedEnergy.end());
        f.medianEnergy = juce::jmax (1.0e-9f, sortedEnergy[sortedEnergy.size() / 2]);
        const auto medianEnergy = f.medianEnergy;

        auto& emission = f.emission;
        auto& keyChroma = f.keyChroma;
        emission.resize ((size_t) numBeats);

        for (int i = 0; i < numBeats; ++i)
        {
            auto t = beatTreble[(size_t) i];
            const auto norm = centreAndNormalise (t);
            const auto silent = norm <= 1.0e-6f || beatEnergy[(size_t) i] < 0.05f * medianEnergy;

            const auto& b = beatBass[(size_t) i];
            const auto bassMax = juce::jmax (1.0e-6f, *std::max_element (b.begin(), b.end()));

            for (int s = 0; s < S - 1; ++s)
            {
                if (silent)
                {
                    emission[(size_t) i][(size_t) s] = 0.0f;
                    continue;
                }

                const auto& tpl = model.templates[(size_t) s];
                auto similarity = 0.0f;

                for (size_t c = 0; c < 12; ++c)
                    similarity += t[c] * tpl[c];

                // Bass on the root supports a chord; bass on its third or fifth (an inversion) a little.
                const auto root = ChordModel::rootOf (s);
                auto bassTerm = b[(size_t) root];

                for (auto interval : chordIntervals()[(size_t) ChordModel::qualityOf (s)])
                    if (interval == 3 || interval == 4 || interval == 7)
                        bassTerm = juce::jmax (bassTerm, 0.35f * b[(size_t) ((root + interval) % 12)]);

                bassTerm /= bassMax;
                emission[(size_t) i][(size_t) s] = similarity + 0.12f * bassTerm - model.qualityPenalty[(size_t) ChordModel::qualityOf (s)];
            }

            emission[(size_t) i][(size_t) (S - 1)] = silent ? 1.0f : 0.25f;

            if (! silent)
                for (size_t c = 0; c < 12; ++c)
                    keyChroma[c] += beatTreble[(size_t) i][c] / norm;
        }

        // Smoothing runs over chord families (root + major/minor, or no chord); the exact
        // quality (7, maj7, 5...) is picked afterwards per segment, so it can't fragment chords.
        auto& familyEmission = f.family;
        familyEmission.resize ((size_t) numBeats);

        for (int i = 0; i < numBeats; ++i)
        {
            for (int fam = 0; fam < (int) F - 1; ++fam)
            {
                auto best = -1.0e9f;

                for (auto q : familyMembers (fam))
                    best = juce::jmax (best, emission[(size_t) i][(size_t) (q * 12 + fam % 12)]);

                familyEmission[(size_t) i][(size_t) fam] = best;
            }

            familyEmission[(size_t) i][F - 1] = emission[(size_t) i][(size_t) (S - 1)];
        }

        return f;
    };

    auto features = computeFeatures (track.beats);

    if (cancelled (cancel))
        return {};

    // --- Meter -----------------------------------------------------------------
    // The meter is the bar length whose grid explains the chord changes best; anything
    // other than 4 must win clearly, as shorter bars offer more cheap change points.
    // Odd meters in eighths (5/8, 7/8) are looked for on a grid at twice the tempo,
    // against 8 eighths per bar.
    std::vector<int> path;
    {
        juce::String scores = "meter scores:";

        auto tryMeters = [&] (const BeatFeatures& f, int reference, std::initializer_list<int> meters,
                              std::vector<int>& bestPath, std::vector<int>& bestPositions, int& bestMeter)
        {
            const auto beatCount = (int) f.family.size();
            const auto referenceScore = barViterbi (f.family, reference, bestPath, bestPositions);
            bestMeter = reference;
            auto bestGain = 0.0;
            scores << "  " << reference << "=" << juce::String (referenceScore, 1);

            for (auto meter : meters)
            {
                std::vector<int> candidatePath, candidatePositions;
                const auto score = barViterbi (f.family, meter, candidatePath, candidatePositions);
                const auto gain = (score - referenceScore) / juce::jmax (1, beatCount);   // per beat
                scores << "  " << meter << "=" << juce::String (gain, 4) << "/beat";

                if (gain > meterMargin && gain > bestGain)
                {
                    bestGain = gain;
                    bestMeter = meter;
                    bestPath = std::move (candidatePath);
                    bestPositions = std::move (candidatePositions);
                }
            }
        };

        {
            const std::vector<int> meters { 4, 3, 5, 7 };
            auto evidence = riffEvidence (features.treble, features.bass, meters, 12);

            for (auto& beat : evidence)
                for (size_t k = 0; k < meters.size(); ++k)
                    beat[k] = riffWeight * beat[k] - (meters[k] == 4 ? 0.0f : meterMargin);

            multiMeterViterbi (features.family, meters, evidence, path, track.beatPositions, track.beatMeters);

            // The song's main meter: the one most beats are in.
            std::map<int, int> counts;

            for (auto m : track.beatMeters)
                ++counts[m];

            track.beatsPerBar = 4;

            for (auto [m, count] : counts)
                if (count > counts[track.beatsPerBar])
                    track.beatsPerBar = m;

            scores << " beats per meter:";

            for (auto [m, count] : counts)
                scores << " " << m << "=" << count;
        }

        const auto allFour = std::all_of (track.beatMeters.begin(), track.beatMeters.end(), [] (int m) { return m == 4; });

        if (allFour && track.beats.size() > 8)
        {
            std::vector<double> eighths;

            for (size_t i = 0; i < track.beats.size(); ++i)
            {
                const auto next = i + 1 < track.beats.size() ? track.beats[i + 1] : 2.0 * track.beats[i] - track.beats[i - 1];
                eighths.push_back (track.beats[i]);

                if (0.5 * (track.beats[i] + next) < lengthSeconds)
                    eighths.push_back (0.5 * (track.beats[i] + next));
            }

            auto eighthFeatures = computeFeatures (eighths);
            std::vector<int> eighthPath, eighthPositions;
            auto eighthMeter = 8;
            scores << "  | eighths:";
            tryMeters (eighthFeatures, 8, { 5, 7 }, eighthPath, eighthPositions, eighthMeter);

            if (eighthMeter != 8)
            {
                track.beats = std::move (eighths);
                track.bpm *= 2.0;
                track.beatUnit = 8;
                track.beatsPerBar = eighthMeter;
                track.beatPositions = std::move (eighthPositions);
                track.beatMeters.assign (track.beats.size(), eighthMeter);
                path = std::move (eighthPath);
                features = std::move (eighthFeatures);
            }
        }

        if (chordAnalyzerDebug)
            std::puts (scores.toRawUTF8());
    }

    const auto numBeats = (int) track.beats.size();
    const auto& beatTreble = features.treble;
    const auto& beatBass = features.bass;
    const auto& beatEnergy = features.energy;
    const auto medianEnergy = features.medianEnergy;
    const auto& emission = features.emission;
    const auto& keyChroma = features.keyChroma;

    if (chordAnalyzerDebug)
    {
        for (int i = 0; i < numBeats; ++i)
        {
            const auto& e = emission[(size_t) i];
            std::vector<int> ranking ((size_t) S);
            std::iota (ranking.begin(), ranking.end(), 0);
            std::partial_sort (ranking.begin(), ranking.begin() + 3, ranking.end(), [&e] (int a, int b) { return e[(size_t) a] > e[(size_t) b]; });
            juce::String line = juce::String (track.beats[(size_t) i], 2) + "  E=" + juce::String (beatEnergy[(size_t) i] / medianEnergy, 2) + "  chroma";

            for (auto v : beatTreble[(size_t) i])
                line << " " << juce::String (v, 1);

            for (int r = 0; r < 3; ++r)
            {
                ChordTrack::Chord c;
                c.root = ChordModel::rootOf (ranking[(size_t) r]);
                c.quality = ChordModel::qualityOf (ranking[(size_t) r]);
                line << "   " << track.chordName (c) << "=" << juce::String (e[(size_t) ranking[(size_t) r]], 3);
            }

            std::puts (line.toRawUTF8());
        }
    }

    for (int i = 0; i < numBeats;)
    {
        auto j = i + 1;

        while (j < numBeats && path[(size_t) j] == path[(size_t) i])
            ++j;

        ChordTrack::Chord chord;
        chord.start = track.beats[(size_t) i];
        chord.end = j < numBeats ? track.beats[(size_t) j] : lengthSeconds;
        const auto family = path[(size_t) i];
        chord.root = family < (int) F - 1 ? family % 12 : -1;

        auto bestScore = -1.0e9f;

        for (auto q : familyMembers (family))
        {
            auto sum = 0.0f;

            for (int b = i; b < j; ++b)
                sum += emission[(size_t) b][(size_t) (q * 12 + chord.root)];

            if (sum > bestScore)
            {
                bestScore = sum;
                chord.quality = q;
            }
        }
        // Slash chord: in each bar of the chord, does the bass clearly sit on another chord tone
        // (or a 7th) than the root? Bars with the same bass are then joined again.
        auto slashBass = [&] (int first, int last)
        {
            if (chord.root < 0)
                return -1;

            std::array<float, 12> bassSum {};

            for (int b = first; b < last; ++b)
            {
                const auto& bb = beatBass[(size_t) b];
                const auto peak = juce::jmax (1.0e-6f, *std::max_element (bb.begin(), bb.end()));

                for (size_t c = 0; c < 12; ++c)
                    bassSum[c] += bb[c] / peak;
            }

            const auto bassNote = (int) std::distance (bassSum.begin(), std::max_element (bassSum.begin(), bassSum.end()));
            const auto interval = (bassNote - chord.root + 12) % 12;
            const auto& tones = chordIntervals()[(size_t) chord.quality];
            const auto isChordTone = std::find (tones.begin(), tones.end(), interval) != tones.end();
            const auto isSeventh = interval == 10 || interval == 11;
            const auto needed = isChordTone ? slashBassRatio : slashBassRatio * 1.2f;

            return (bassNote != chord.root && (isChordTone || isSeventh)
                    && bassSum[(size_t) bassNote] >= needed * bassSum[(size_t) chord.root]) ? bassNote : -1;
        };

        for (int first = i; first < j;)
        {
            auto last = first + 1;

            while (last < j && ! track.isDownbeat (last))
                ++last;

            const auto pieceBass = slashBass (first, last);

            if (first > i && track.chords.back().bass == pieceBass)
            {
                track.chords.back().numBeats += last - first;
            }
            else
            {
                auto piece = chord;
                piece.startBeat = first;
                piece.numBeats = last - first;
                piece.bass = pieceBass;
                track.chords.push_back (piece);
            }

            first = last;
        }

        for (auto k = track.chords.size(); k-- > 0 && track.chords[k].startBeat >= i;)
        {
            auto& c = track.chords[k];
            c.start = track.beats[(size_t) c.startBeat];
            c.end = c.startBeat + c.numBeats < numBeats ? track.beats[(size_t) (c.startBeat + c.numBeats)] : lengthSeconds;
        }

        i = j;
    }

    if (! track.chords.empty())
        track.chords.front().start = 0.0;

    track.keyRoot = estimateKey (keyChroma, track.keyIsMinor);
    return track;
}

} // namespace pv
