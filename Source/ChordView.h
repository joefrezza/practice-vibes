#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ChordAnalyzer.h"

namespace pv
{

/** Shows the chord playing now (large), the next chord with a beat countdown,
    and a strip of the chords after that. Fed by PracticePanel's timer.
*/
class ChordView final : public juce::Component
{
public:
    ChordView() = default;

    /** New analysis result (or nullptr), plus a status line shown when there is no result. */
    void setTrack (std::shared_ptr<const ChordTrack>, const juce::String& status);

    /** Call regularly: song position (seconds) and the player's pitch shift, for transposed names. */
    void update (double positionSeconds, int semitones);

    void paint (juce::Graphics&) override;

private:
    std::shared_ptr<const ChordTrack> track;
    juce::String status;
    int chordIndex = -1, beatIndex = -1, transpose = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordView)
};

} // namespace pv
