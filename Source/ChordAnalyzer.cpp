#include "ChordAnalyzer.h"
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace pv
{

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

    return noteName (chord.root + semitones, keyUsesFlats (keyRoot + semitones, keyIsMinor)) + qualitySuffix (chord.quality);
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

    struct ChordModel
    {
        static constexpr int numQualities = ChordTrack::numQualities;
        static constexpr int numStates = 12 * numQualities + 1;   // last = no chord

        std::array<std::array<float, 12>, numStates - 1> templates {};
        std::array<float, numQualities> qualityPenalty { 0.0f, 0.0f, 0.04f, 0.04f, 0.05f, 0.05f };

        ChordModel()
        {
            static const std::vector<int> intervals[numQualities] = {
                { 0, 4, 7 }, { 0, 3, 7 }, { 0, 4, 7, 10 }, { 0, 3, 7, 10 }, { 0, 4, 7, 11 }, { 0, 7 }
            };

            // Each chord tone contributes its first harmonics (octave, fifth, ...), decaying.
            static const int harmonicOffsets[] = { 0, 12, 19, 24, 28, 31 };

            for (int q = 0; q < numQualities; ++q)
            {
                for (int root = 0; root < 12; ++root)
                {
                    auto& t = templates[(size_t) (q * 12 + root)];
                    t.fill (0.0f);

                    for (auto interval : intervals[q])
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

    /** Viterbi over (chord family, position in bar). The position advances by one each beat;
        changing chord costs least on the downbeat. A costly "phase jump" lets the bar grid
        recover where the beat tracker added or dropped a beat.
    */
    template <size_t F>
    void barViterbi (const std::vector<std::array<float, F>>& emission, int beatsPerBar,
                     std::vector<int>& familyPath, std::vector<int>& positionPath)
    {
        const auto numBeats = (int) emission.size();
        const auto P = beatsPerBar;
        const auto numStates = (int) F * P;
        familyPath.assign ((size_t) numBeats, (int) F - 1);
        positionPath.assign ((size_t) numBeats, 0);

        if (numBeats == 0)
            return;

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

        for (int i = numBeats - 1; i >= 0; --i)
        {
            familyPath[(size_t) i] = state / P;
            positionPath[(size_t) i] = state % P;

            if (i > 0)
                state = from[(size_t) i * (size_t) numStates + (size_t) state];
        }
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
bool chordAnalyzerDebug = false;   // developer tool only: prints per-beat details

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

    // --- Beat-synchronous features ---------------------------------------------
    const auto numBeats = (int) track.beats.size();
    std::vector<std::array<float, 12>> beatTreble ((size_t) numBeats), beatBass ((size_t) numBeats);
    std::vector<float> beatEnergy ((size_t) numBeats);
    const auto framesPerSecond = rate / hop;

    for (int i = 0; i < numBeats; ++i)
    {
        const auto start = track.beats[(size_t) i];
        const auto end = i + 1 < numBeats ? track.beats[(size_t) i + 1] : lengthSeconds;
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
    const auto medianEnergy = juce::jmax (1.0e-9f, sortedEnergy[sortedEnergy.size() / 2]);

    // --- Emissions ------------------------------------------------------------
    static const ChordModel model;
    constexpr auto S = ChordModel::numStates;
    std::vector<std::array<float, S>> emission ((size_t) numBeats);
    std::array<double, 12> keyChroma {};

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

            const auto root = (size_t) ChordModel::rootOf (s);
            const auto bassTerm = b[root] / bassMax;
            emission[(size_t) i][(size_t) s] = similarity + 0.12f * bassTerm - model.qualityPenalty[(size_t) ChordModel::qualityOf (s)];
        }

        emission[(size_t) i][(size_t) (S - 1)] = silent ? 1.0f : 0.25f;

        if (! silent)
            for (size_t c = 0; c < 12; ++c)
                keyChroma[c] += beatTreble[(size_t) i][c] / norm;
    }

    // Smoothing runs over chord families (root + major/minor, or no chord); the exact
    // quality (7, maj7, 5...) is picked afterwards per segment, so it can't fragment chords.
    constexpr size_t F = 25;
    std::vector<std::array<float, F>> familyEmission ((size_t) numBeats);

    auto familyMembers = [] (int family)
    {
        using Q = ChordTrack::Quality;
        if (family < 12)  return std::vector<Q> { ChordTrack::major, ChordTrack::dominant7, ChordTrack::major7, ChordTrack::power };
        if (family < 24)  return std::vector<Q> { ChordTrack::minor, ChordTrack::minor7, ChordTrack::power };
        return std::vector<Q>();
    };

    for (int i = 0; i < numBeats; ++i)
    {
        for (int f = 0; f < (int) F - 1; ++f)
        {
            auto best = -1.0e9f;

            for (auto q : familyMembers (f))
                best = juce::jmax (best, emission[(size_t) i][(size_t) (q * 12 + f % 12)]);

            familyEmission[(size_t) i][(size_t) f] = best;
        }

        familyEmission[(size_t) i][F - 1] = emission[(size_t) i][(size_t) (S - 1)];
    }

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

    if (cancelled (cancel))
        return {};

    // --- Smoothing: changes are cheap on the downbeat, dearer mid-bar. ---------
    track.beatsPerBar = 4;
    std::vector<int> path;
    barViterbi (familyEmission, track.beatsPerBar, path, track.beatPositions);

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
        chord.startBeat = i;
        chord.numBeats = j - i;
        track.chords.push_back (chord);
        i = j;
    }

    if (! track.chords.empty())
        track.chords.front().start = 0.0;

    track.keyRoot = estimateKey (keyChroma, track.keyIsMinor);
    return track;
}

} // namespace pv
