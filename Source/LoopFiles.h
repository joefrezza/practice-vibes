#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "Looper.h"

namespace pv
{

/** Saving and loading looper sessions.

    A session is a folder holding one WAV per track (32-bit float, every loop starting on the
    same bar line, so they line up when dropped into a DAW), "mix.wav" (all unmuted tracks at
    their volumes, long enough for every loop to come round), and "session.xml" (tempo, meter,
    bars, volume, mute, record mode).
*/
namespace LoopFiles
{
    /** Desktop: Documents/Practice Vibes/Loops. iOS: Loops in the app's Documents (Files app). */
    juce::File getLoopsFolder();

    /** Saved sessions, newest first. */
    juce::Array<juce::File> listSessions();

    /** A new, unused folder name for a session, e.g. "2026-11-14 18.05 - 100 bpm 4-4". */
    juce::File createSessionFolder (const LoopSet&, const juce::File& parent = getLoopsFolder());

    juce::Result save (const LoopSet&, const juce::File& folder);
    juce::Result load (const juce::File& folder, LoopSet&);

    /** All unmuted loops at their volumes, for the least common multiple of their lengths
        (capped at 10 minutes). Stereo. */
    juce::AudioBuffer<float> renderMix (const LoopSet&);

    /** Display text for a session folder, e.g. "2026-11-14 18.05 - 100 bpm 4-4 (3 loops)". */
    juce::String describe (const juce::File& folder);
}

} // namespace pv
