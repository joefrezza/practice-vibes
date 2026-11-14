#include "Tuner.h"
#include <complex>

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
const std::vector<Tuning>& tuningPresets()
{
    static const std::vector<Tuning> presets {
        { "Standard",         { 40, 45, 50, 55, 59, 64 } },     // E A D G B E
        { "Half step down",   { 39, 44, 49, 54, 58, 63 } },     // Eb Ab Db Gb Bb Eb
        { "Full step down",   { 38, 43, 48, 53, 57, 62 } },     // D G C F A D
        { "Drop D",           { 38, 45, 50, 55, 59, 64 } },     // D A D G B E
        { "Drop C",           { 36, 43, 48, 53, 57, 62 } },     // C G C F A D
        { "DADGAD",           { 38, 45, 50, 55, 57, 62 } },
        { "Open G",           { 38, 43, 50, 55, 59, 62 } },     // D G D G B D
        { "Open D",           { 38, 45, 50, 54, 57, 62 } },     // D A D F# A D
        { "Open E",           { 40, 47, 52, 56, 59, 64 } },     // E B E G# B E
        { "Open C",           { 36, 43, 48, 55, 60, 64 } },     // C G C G C E
    };
    return presets;
}

juce::String tuningName (const StringNotes& strings)
{
    for (auto& t : tuningPresets())
        if (t.strings == strings)
            return t.name;

    return "Custom";
}

float noteFrequency (int midiNote, float referenceA4) noexcept
{
    return referenceA4 * std::pow (2.0f, (float) (midiNote - 69) / 12.0f);
}

int nearestString (const StringNotes& strings, float frequency, float referenceA4, float& centsAway, float maxCents)
{
    int best = -1;
    centsAway = 0.0f;

    if (frequency <= 0.0f)
        return best;

    for (int i = 0; i < (int) strings.size(); ++i)
    {
        const auto cents = 1200.0f * std::log2 (frequency / noteFrequency (strings[(size_t) i], referenceA4));

        if (std::abs (cents) <= maxCents && (best < 0 || std::abs (cents) < std::abs (centsAway)))
        {
            best = i;
            centsAway = cents;
        }
    }

    return best;
}

//==============================================================================
namespace
{
    constexpr int strumFftOrder = 17;   // 131072 points: 0.37 Hz bins at 48 kHz, zero-padded window
    constexpr int strumFftSize = 1 << strumFftOrder;
}

StrumAnalyser::StrumAnalyser()
    : fft (strumFftOrder),
      window ((size_t) strumFftSize),
      spectrum ((size_t) strumFftSize * 2),
      magnitude ((size_t) strumFftSize / 2)
{
}

int StrumAnalyser::samplesNeeded (double sampleRate) noexcept
{
    const auto factor = PitchDetector::decimationFor (sampleRate);
    return (int) std::round (windowSeconds * sampleRate / factor) * factor;
}

StrumAnalyser::Peak StrumAnalyser::findPeak (float target, double binHz) const
{
    Peak peak;
    const auto spread = std::pow (2.0f, searchCents / 1200.0f);
    const auto lo = juce::jmax (1, (int) std::floor (target / spread / binHz));
    const auto hi = juce::jmin ((int) magnitude.size() - 2, (int) std::ceil (target * spread / binHz));

    if (hi - lo < 2)
        return peak;

    int best = lo;

    for (int k = lo; k <= hi; ++k)
        if (magnitude[(size_t) k] > magnitude[(size_t) best])
            best = k;

    // A maximum on the edge of the range is the flank of something outside it.
    if (best == lo || best == hi || magnitude[(size_t) best] <= 0.0f)
        return peak;

    // A string stands far above the noise floor; the biggest noise bump only ~3x its median.
    // The floor is taken over +-4 semitones, so the peak's own main lobe hardly moves the median.
    const auto floorSpread = std::pow (2.0f, 4.0f / 12.0f);
    const auto floorLo = juce::jmax (1, juce::jmin (lo, best - 60, (int) std::floor (target / floorSpread / binHz)));
    const auto floorHi = juce::jmin ((int) magnitude.size() - 1, juce::jmax (hi, best + 60, (int) std::ceil (target * floorSpread / binHz)));
    std::vector<float> band (magnitude.begin() + floorLo, magnitude.begin() + floorHi + 1);
    std::nth_element (band.begin(), band.begin() + (long) band.size() / 2, band.end());

    if (magnitude[(size_t) best] < minProminence * band[band.size() / 2])
        return peak;

    // Parabola through the log magnitudes (close to exact for a Hann window).
    const auto a = std::log (magnitude[(size_t) (best - 1)] + 1.0e-12f);
    const auto b = std::log (magnitude[(size_t) best] + 1.0e-12f);
    const auto c = std::log (magnitude[(size_t) (best + 1)] + 1.0e-12f);
    const auto denom = a - 2.0f * b + c;
    const auto shift = std::abs (denom) > 1.0e-9f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / denom) : 0.0f;

    peak.frequency = (float) (((double) best + shift) * binHz);
    peak.magnitude = magnitude[(size_t) best];
    return peak;
}

namespace
{
    /** Spectrum (at bin k of an fftSize-point FFT) of a Hann-windowed complex exponential of
        frequency f (in cycles per sample), n samples long. */
    std::complex<double> hannKernel (double k, double f, int n, int points)
    {
        const auto dirichlet = [n] (double theta)
        {
            const auto half = std::sin (0.5 * theta);
            const auto ratio = std::abs (half) < 1.0e-12 ? (double) n : std::sin (0.5 * n * theta) / half;
            return std::polar (ratio, -0.5 * theta * (n - 1));
        };

        const auto theta = juce::MathConstants<double>::twoPi * (k / points - f);
        const auto step = juce::MathConstants<double>::twoPi / (n - 1);
        return 0.5 * dirichlet (theta) - 0.25 * dirichlet (theta - step) - 0.25 * dirichlet (theta + step);
    }

    /** Least squares: complex amplitudes c minimising |x - M c|; returns the residual energy. */
    double solveLeastSquares (const std::vector<std::vector<std::complex<double>>>& columns,
                              const std::vector<std::complex<double>>& x,
                              std::vector<std::complex<double>>& c)
    {
        const auto m = columns.size();
        std::vector<std::vector<std::complex<double>>> a (m, std::vector<std::complex<double>> (m + 1));

        for (size_t r = 0; r < m; ++r)
        {
            for (size_t q = 0; q < m; ++q)
                for (size_t b = 0; b < x.size(); ++b)
                    a[r][q] += std::conj (columns[r][b]) * columns[q][b];

            for (size_t b = 0; b < x.size(); ++b)
                a[r][m] += std::conj (columns[r][b]) * x[b];
        }

        // Gauss-Jordan with partial pivoting (m is 2..5).
        for (size_t p = 0; p < m; ++p)
        {
            auto pivot = p;

            for (size_t r = p + 1; r < m; ++r)
                if (std::abs (a[r][p]) > std::abs (a[pivot][p]))
                    pivot = r;

            std::swap (a[p], a[pivot]);

            if (std::abs (a[p][p]) < 1.0e-30)
                return std::numeric_limits<double>::max();

            for (size_t r = 0; r < m; ++r)
                if (r != p)
                {
                    const auto factor = a[r][p] / a[p][p];

                    for (size_t q = p; q <= m; ++q)
                        a[r][q] -= factor * a[p][q];
                }
        }

        c.resize (m);
        double residual = 0.0;

        for (size_t r = 0; r < m; ++r)
            c[r] = a[r][m] / a[r][r];

        for (size_t b = 0; b < x.size(); ++b)
        {
            auto model = std::complex<double>();

            for (size_t r = 0; r < m; ++r)
                model += c[r] * columns[r][b];

            residual += std::norm (x[b] - model);
        }

        return residual;
    }
}

StrumAnalyser::Peak StrumAnalyser::fitPartial (int n, double rate, float target, double start,
                                               const std::vector<double>& others) const
{
    const auto spread = std::pow (2.0, searchCents / 1200.0);
    const auto binHz = rate / strumFftSize;
    const auto lo = juce::jmax (1, (int) std::floor (target / spread / binHz));
    const auto hi = juce::jmin ((int) magnitude.size() - 2, (int) std::ceil (target * spread / binHz));

    std::vector<std::complex<double>> x;

    for (int k = lo; k <= hi; ++k)
        x.emplace_back (spectrum[(size_t) (2 * k)], spectrum[(size_t) (2 * k + 1)]);

    std::vector<std::vector<std::complex<double>>> columns (others.size() + 1);

    for (size_t o = 0; o < others.size(); ++o)
        for (int k = lo; k <= hi; ++k)
            columns[o + 1].push_back (hannKernel (k, others[o] / rate, n, strumFftSize));

    std::vector<std::complex<double>> c;

    const auto residualAt = [&] (double cents)
    {
        const auto f = start * std::pow (2.0, cents / 1200.0) / rate;
        columns[0].clear();

        for (int k = lo; k <= hi; ++k)
            columns[0].push_back (hannKernel (k, f, n, strumFftSize));

        return solveLeastSquares (columns, x, c);
    };

    // Coarse scan around the first-pass estimate, then a parabola through the best three.
    constexpr double range = 20.0, step = 0.25;
    double bestCents = 0.0, best = std::numeric_limits<double>::max();

    for (double cents = -range; cents <= range; cents += step)
        if (const auto r = residualAt (cents); r < best)
        {
            best = r;
            bestCents = cents;
        }

    const auto left = residualAt (bestCents - step), right = residualAt (bestCents + step);
    const auto denom = left - 2.0 * best + right;

    if (denom > 0.0)
        bestCents += juce::jlimit (-step, step, 0.5 * step * (left - right) / denom);

    residualAt (bestCents);

    Peak peak;
    peak.frequency = (float) (start * std::pow (2.0, bestCents / 1200.0));

    // Same scale as a peak in `magnitude`: amplitude x window sum.
    peak.magnitude = (float) (std::abs (c[0]) * 0.5 * (n - 1));
    return peak;
}

StrumResult StrumAnalyser::analyse (const float* samples, int numSamples, double sampleRate,
                                    const StringNotes& strings, float referenceA4)
{
    StrumResult result;
    const auto factor = PitchDetector::decimationFor (sampleRate);
    const auto needed = samplesNeeded (sampleRate);

    if (sampleRate <= 0.0 || numSamples < needed)
        return result;

    const auto rate = sampleRate / factor;
    const auto n = juce::jmin (strumFftSize, needed / factor);
    const auto* src = samples + (numSamples - needed);

    double mean = 0.0;

    for (int i = 0; i < n; ++i)
    {
        float sum = 0.0f;

        for (int k = 0; k < factor; ++k)
            sum += src[i * factor + k];

        window[(size_t) i] = sum / (float) factor;
        mean += window[(size_t) i];
    }

    mean /= n;
    double energy = 0.0;

    for (int i = 0; i < n; ++i)
    {
        window[(size_t) i] -= (float) mean;
        energy += (double) window[(size_t) i] * window[(size_t) i];
    }

    if (std::sqrt (energy / n) < PitchDetector::minLevel)
        return result;

    std::fill (spectrum.begin(), spectrum.end(), 0.0f);

    for (int i = 0; i < n; ++i)
        spectrum[(size_t) i] = window[(size_t) i]
                                 * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (n - 1)));

    fft.performRealOnlyForwardTransform (spectrum.data());

    for (size_t k = 0; k < magnitude.size(); ++k)
        magnitude[k] = std::hypot (spectrum[2 * k], spectrum[2 * k + 1]);

    const auto binHz = rate / strumFftSize;
    std::array<Peak, 6> chosen;
    std::array<int, 6> harmonicUsed {};
    float loudest = 0.0f;

    for (size_t i = 0; i < strings.size(); ++i)
    {
        const auto target = noteFrequency (strings[i], referenceA4);

        // Is the 2nd harmonic shared with a harmonic of another string? Then it can't be trusted.
        bool shared = false;

        for (size_t j = 0; j < strings.size() && ! shared; ++j)
            for (int h = 1; h <= 8 && ! shared; ++h)
                if (j != i && std::abs (1200.0f * std::log2 ((float) h * noteFrequency (strings[j], referenceA4) / (2.0f * target))) < 25.0f)
                    shared = true;

        const auto first = findPeak (target, binHz);
        const auto second = shared ? Peak {} : findPeak (2.0f * target, binHz);

        if (first.magnitude > 0.0f && (second.magnitude <= 0.0f || first.magnitude >= 0.25f * second.magnitude))
        {
            chosen[i] = first;
            harmonicUsed[i] = 1;
        }
        else if (second.magnitude > 0.0f)
        {
            chosen[i] = { 0.5f * second.frequency, second.magnitude };
            harmonicUsed[i] = 2;
        }

        loudest = juce::jmax (loudest, chosen[i].magnitude);
    }

    const auto floorLevel = loudest * juce::Decibels::decibelsToGain (relativeFloorDb);
    std::array<bool, 6> heard {};

    for (size_t i = 0; i < strings.size(); ++i)
        heard[i] = chosen[i].magnitude > 0.0f && chosen[i].magnitude >= floorLevel;

    // Second pass, low strings first: where a string's partial shares its frequency with another
    // string's harmonic (B3 / E2 x 3, E4 / E2 x 4, D3 / D2 x 2...), the single peak is a blend of
    // both. Fit the band as the sum of all of them instead and keep this string's part.
    for (size_t i = 0; i < strings.size(); ++i)
    {
        if (! heard[i])
            continue;

        const auto h = harmonicUsed[i];
        const auto target = (float) h * noteFrequency (strings[i], referenceA4);
        std::vector<double> others;
        const auto spread = std::pow (2.0f, (searchCents + 15.0f) / 1200.0f);

        for (size_t j = 0; j < strings.size(); ++j)
            for (int k = 1; k <= 12; ++k)
                if (j != i && heard[j])
                {
                    const auto f = (float) k * chosen[j].frequency;

                    if (f > target / spread && f < target * spread)
                        others.push_back (f);
                }

        if (others.empty())
            continue;

        const auto fit = fitPartial (n, rate, target, (double) h * chosen[i].frequency, others);

        if (fit.magnitude < floorLevel)
            heard[i] = false;
        else
            chosen[i].frequency = fit.frequency / (float) h;
    }

    // Fewer than three strings: not a strum (a knock, a single note, noise).
    if (std::count (heard.begin(), heard.end(), true) < 3)
        return result;

    for (size_t i = 0; i < strings.size(); ++i)
    {
        const auto cents = 1200.0f * std::log2 (chosen[i].frequency / noteFrequency (strings[i], referenceA4));

        if (! heard[i] || std::abs (cents) > searchCents)
            continue;

        result[i].heard = true;
        result[i].frequency = chosen[i].frequency;
        result[i].cents = cents;
    }

    return result;
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

std::optional<StrumResult> Tuner::readStrum()
{
    const auto sampleRate = rate.load (std::memory_order_relaxed);
    const auto end = written.load (std::memory_order_acquire);

    if (sampleRate <= 0.0)
        return {};

    const auto needed = StrumAnalyser::samplesNeeded (sampleRate);
    const auto settle = (juce::int64) (strumSettleSeconds * sampleRate);
    const auto chunk = 256 * PitchDetector::decimationFor (sampleRate);
    const auto retrigger = (juce::int64) (0.3 * sampleRate);   // a strum's own rise is not a new strum
    constexpr float gate = 0.01f;                                // about -40 dBFS: a strum, not a hum

    // Don't scan audio that the ring has already overwritten.
    strumScanned = juce::jmax (strumScanned, clearedAt, end - ringSize / 2);

    for (; strumScanned + chunk <= end; strumScanned += chunk)
    {
        double sum = 0.0;

        for (int i = 0; i < chunk; ++i)
        {
            const auto v = ring[(size_t) ((strumScanned + i) & (ringSize - 1))];
            sum += (double) v * v;
        }

        // Level over the last 4 chunks (~21 ms): longer than a low E period, so it doesn't
        // swing with the waveform.
        chunkEnergies[(size_t) energyNext] = (float) (sum / chunk);
        energyNext = (energyNext + 1) % (int) chunkEnergies.size();
        energyCount = juce::jmin (energyCount + 1, (int) chunkEnergies.size());
        float energy = 0.0f;

        for (int i = 0; i < energyCount; ++i)
            energy += chunkEnergies[(size_t) i];

        const auto level = std::sqrt (energy / (float) energyCount);

        // A strum spreads over up to ~100 ms, so compare with the quietest moment of the last
        // ~170 ms, not just the previous chunk: that also catches re-strumming ringing strings.
        auto before = chunkCount > 0 ? chunkLevels[0] : 0.0f;

        for (int i = 1; i < chunkCount; ++i)
            before = juce::jmin (before, chunkLevels[(size_t) i]);

        if (level > gate && level > 1.6f * before && (strumOnset < 0 || strumScanned - strumOnset > retrigger))
            strumOnset = strumScanned;

        // Recent chunk levels, in no particular order (only their minimum is used).
        chunkLevels[(size_t) chunkNext] = level;
        chunkNext = (chunkNext + 1) % (int) chunkLevels.size();
        chunkCount = juce::jmin (chunkCount + 1, (int) chunkLevels.size());
    }

    if (strumOnset < 0 || end - strumOnset < settle + needed)
        return {};

    const auto start = strumOnset + settle;
    strumOnset = -1;

    if (end - start > ringSize || needed > (int) scratch.size())
        return {};

    for (int i = 0; i < needed; ++i)
        scratch[(size_t) i] = ring[(size_t) ((start + i) & (ringSize - 1))];

    auto result = strumAnalyser.analyse (scratch.data(), needed, sampleRate, strings, referenceA4);

    if (std::none_of (result.begin(), result.end(), [] (const StringReading& r) { return r.heard; }))
        return {};      // not a strum: keep waiting

    return result;
}

void Tuner::clear()
{
    // `written` belongs to the audio thread: remember where we are instead, and read()
    // waits for a full new window after this point.
    clearedAt = written.load (std::memory_order_acquire);
    strumScanned = clearedAt;
    strumOnset = -1;
    chunkCount = chunkNext = energyCount = energyNext = 0;
}

std::unique_ptr<juce::XmlElement> Tuner::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("TUNER");
    xml->setAttribute ("referenceA4", (double) referenceA4);
    xml->setAttribute ("muteWhileTuning", muteWhileTuning);

    juce::StringArray notes;

    for (auto n : strings)
        notes.add (juce::String (n));

    xml->setAttribute ("strings", notes.joinIntoString (" "));
    xml->setAttribute ("strobe", strobe);
    xml->setAttribute ("strumMode", strumMode);
    return xml;
}

void Tuner::fromXml (const juce::XmlElement& xml)
{
    referenceA4 = juce::jlimit (minReference, maxReference, (float) xml.getDoubleAttribute ("referenceA4", 440.0));
    muteWhileTuning = xml.getBoolAttribute ("muteWhileTuning", true);
    strobe = xml.getBoolAttribute ("strobe", false);
    strumMode = xml.getBoolAttribute ("strumMode", false);

    // Six notes, each in a sensible range for a guitar string; anything else -> standard.
    strings = tuningPresets().front().strings;
    const auto notes = juce::StringArray::fromTokens (xml.getStringAttribute ("strings"), " ", {});

    if (notes.size() == (int) strings.size())
    {
        StringNotes parsed {};
        bool ok = true;

        for (int i = 0; i < notes.size(); ++i)
        {
            parsed[(size_t) i] = notes[i].getIntValue();
            ok = ok && notes[i].containsOnly ("0123456789")
                    && parsed[(size_t) i] >= minStringNote && parsed[(size_t) i] <= maxStringNote;
        }

        if (ok)
            strings = parsed;
    }
}

} // namespace pv
