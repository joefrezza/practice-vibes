#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "AppEngine.h"

namespace pv
{

/** Chromatic tuner shown in the overlay: note name, a cents needle, the six strings of standard
    tuning, the A4 reference and "mute the guitar while tuning". The tuner listens only while
    this panel exists.
*/
class TunerPanel final : public juce::Component,
                         private juce::Timer
{
public:
    explicit TunerPanel (AppEngine&);
    ~TunerPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void applyMute();
    void paintMeter (juce::Graphics&, juce::Rectangle<float>);
    void paintStrings (juce::Graphics&, juce::Rectangle<float>);

    AppEngine& engine;
    Tuner& tuner;
    PitchSmoother smoother;

    float shownFrequency = 0.0f;
    float needleCents = 0.0f;       // eased towards the measured value
    float inputLevel = 0.0f;
    NoteInfo note;

    juce::Rectangle<int> noteArea, meterArea, stringsArea;
    juce::Label referenceLabel, hintLabel;
    juce::Slider referenceSlider;
    juce::ToggleButton muteToggle { "Mute the guitar while tuning" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TunerPanel)
};

} // namespace pv
