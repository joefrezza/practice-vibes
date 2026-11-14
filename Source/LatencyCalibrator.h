#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace pv
{

/** Measures the real round-trip latency: the player strums (muted) on the metronome clicks,
    or a cable loops the output back to the input. Every onset in the dry input is compared with
    the nearest beat of the bar clock (both in graph time); the typical lag is how late audio
    recorded "on the beat" arrives, which is what the looper must compensate.

    process() runs on the audio thread and posts lags through a lock-free FIFO;
    start()/stop()/readLags()/analyse() belong to the message thread.
*/
class LatencyCalibrator final
{
public:
    struct Result
    {
        bool ok = false;
        double lagMs = 0.0;         // typical onset lag behind the beat
        double spreadMs = 0.0;      // standard deviation of the hits used
        int used = 0, total = 0;
        juce::String problem;       // why it failed
    };

    // Message thread ------------------------------------------------------------
    void start()        { resetRequested = true; active = true; }
    void stop()         { active = false; }
    bool isActive() const noexcept  { return active.load(); }

    /** Appends the lags (ms) found since the last call. */
    void readLags (std::vector<double>& lagsMs)
    {
        const auto scope = fifo.read (fifo.getNumReady());

        for (int i = 0; i < scope.blockSize1; ++i)  lagsMs.push_back (lags[(size_t) (scope.startIndex1 + i)]);
        for (int i = 0; i < scope.blockSize2; ++i)  lagsMs.push_back (lags[(size_t) (scope.startIndex2 + i)]);
    }

    /** Typical lag of a set of hits: hits more than `toleranceMs` away from the median are
        ignored (a missed or extra strum); at least `minHits` and 60 % of the hits must agree. */
    static Result analyse (std::vector<double> lagsMs, int minHits = 6, double toleranceMs = 15.0)
    {
        Result r;
        r.total = (int) lagsMs.size();

        if (r.total < minHits)
        {
            r.problem = "too few hits (" + juce::String (r.total) + ")";
            return r;
        }

        std::sort (lagsMs.begin(), lagsMs.end());
        const auto median = lagsMs[lagsMs.size() / 2];
        double sum = 0.0, sumSquares = 0.0;

        for (auto lag : lagsMs)
            if (std::abs (lag - median) <= toleranceMs)
            {
                sum += lag;
                sumSquares += lag * lag;
                ++r.used;
            }

        r.lagMs = sum / juce::jmax (1, r.used);
        r.spreadMs = std::sqrt (juce::jmax (0.0, sumSquares / juce::jmax (1, r.used) - r.lagMs * r.lagMs));
        r.ok = r.used >= minHits && r.used * 10 >= r.total * 6;

        if (! r.ok)
            r.problem = "the hits were too uneven (" + juce::String (r.used) + " of " + juce::String (r.total) + " agreed)";

        return r;
    }

    // Audio thread ---------------------------------------------------------------
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        releaseCoefficient = (float) std::exp (-1.0 / (0.02 * sampleRate));
        noiseCoefficient = (float) std::exp (-1.0 / (0.5 * sampleRate));
        resetRequested = true;
    }

    /** `input` starts at graph time `start`; beats fall at barStart + k * beatSamples. */
    void process (const float* input, int numSamples, juce::int64 start, double barStart, double beatSamples)
    {
        if (! active.load() || beatSamples <= 0.0)
            return;

        if (resetRequested.exchange (false))
        {
            envelope = 0.0f;
            noise = 0.001f;
            armed = true;
            sinceOnset = 1 << 30;
        }

        const auto refractory = (int) juce::jmax (0.15 * sampleRate, 0.5 * beatSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto level = std::abs (input[i]);
            envelope = juce::jmax (level, envelope * releaseCoefficient);
            const auto threshold = juce::jmax (0.02f, noise * 8.0f);
            ++sinceOnset;

            if (armed && level > threshold && sinceOnset > refractory)
            {
                const auto t = (double) (start + i);
                const auto beat = std::round ((t - barStart) / beatSamples);
                post ((t - (barStart + beat * beatSamples)) * 1000.0 / sampleRate);
                armed = false;
                sinceOnset = 0;
            }
            else if (! armed && sinceOnset > refractory && envelope < threshold * 0.5f)
            {
                armed = true;
            }

            if (armed && envelope < threshold)
                noise = noise * noiseCoefficient + level * (1.0f - noiseCoefficient);
        }
    }

private:
    void post (double lagMs)
    {
        const auto scope = fifo.write (1);

        if (scope.blockSize1 > 0)
            lags[(size_t) scope.startIndex1] = lagMs;
    }

    std::atomic<bool> active { false }, resetRequested { true };
    juce::AbstractFifo fifo { 64 };
    std::array<double, 64> lags {};

    double sampleRate = 48000.0;
    float releaseCoefficient = 0.999f, noiseCoefficient = 0.9999f;
    float envelope = 0.0f, noise = 0.001f;
    bool armed = true;
    int sinceOnset = 1 << 30;
};

} // namespace pv
