#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PlayerProcessor.h"

namespace pv
{

/** Zoomable song waveform with playhead and loop region, plus an overview strip.

    Main area:
      - tap/click: seek
      - drag: select a loop; drag near a loop edge: move that edge
      - pinch (two fingers, iPad): zoom around the fingers and pan
      - Ctrl/Cmd + mouse wheel or trackpad pinch: zoom around the pointer; wheel alone: pan
    Overview strip (bottom): whole song; tap/drag to move the visible window.
*/
class WaveformView final : public juce::Component
{
public:
    explicit WaveformView (PlayerProcessor&);

    /** Call after a new song was loaded: resets the zoom. */
    void songChanged();

    /** Call regularly (e.g. 30 Hz): follows the playhead and repaints. */
    void refresh();

    void zoomBy (double factor);                    // > 1 zooms in, around the view centre
    void zoomToFit();
    void zoomToRange (double startSeconds, double endSeconds);
    bool isZoomed() const;

    std::function<void (double seconds)> onSeek;
    std::function<void (double startSeconds, double endSeconds)> onLoopSelected;   // new loop, enable it
    std::function<void (double startSeconds, double endSeconds)> onLoopEdited;     // edge moved

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;

private:
    enum class Gesture { none, tapOrSelect, selecting, movingLoopStart, movingLoopEnd, pinching, overview, consumed };

    double xToSeconds (float x) const;
    float secondsToX (double seconds) const;
    void setView (double start, double length);
    void zoomAround (double factor, float x);
    double getMinViewLength() const;

    void paintWaveform (juce::Graphics&, juce::Rectangle<int> area);
    void paintTimeRuler (juce::Graphics&, juce::Rectangle<int> area);
    void paintOverview (juce::Graphics&, juce::Rectangle<int> area);
    void startPinch();

    PlayerProcessor& player;
    std::shared_ptr<const LoadedSong> song;

    juce::Rectangle<int> mainArea, overviewArea;
    double viewStart = 0.0, viewLength = 1.0;

    Gesture gesture = Gesture::none;
    float dragStartX = 0.0f, dragEndX = 0.0f;
    bool playheadWasVisible = true;

    // Multi-touch: active finger positions by touch index.
    std::map<int, juce::Point<float>> touches;
    float pinchStartDistance = 1.0f;
    double pinchStartLength = 1.0, pinchAnchorSeconds = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};

} // namespace pv
