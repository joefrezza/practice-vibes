#pragma once

namespace pv
{

/** Receives a copy of a mono signal on the audio thread (see GainMeterProcessor::tap). */
struct AudioTap
{
    virtual ~AudioTap() = default;

    /** Audio thread: must be lock-free and not allocate. */
    virtual void push (const float* samples, int numSamples, double sampleRate) noexcept = 0;
};

} // namespace pv
