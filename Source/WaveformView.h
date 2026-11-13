#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PlayerProcessor.h"

namespace pv
{

/** Song overview with playhead and loop region.
    Tap/click to seek; drag horizontally to select a loop.
*/
class WaveformView final : public juce::Component
{
public:
    explicit WaveformView (PlayerProcessor&);

    /** Call after a new song was loaded. */
    void songChanged();

    std::function<void (double seconds)> onSeek;
    std::function<void (double startSeconds, double endSeconds)> onLoopSelected;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    double xToSeconds (float x) const;
    float secondsToX (double seconds) const;

    PlayerProcessor& player;
    std::shared_ptr<const LoadedSong> song;

    bool dragging = false;
    float dragStartX = 0.0f, dragEndX = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};

} // namespace pv
