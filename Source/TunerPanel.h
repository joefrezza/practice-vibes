#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "AppEngine.h"

namespace pv
{

/** Tuner shown in the overlay. Single-string mode: note name, a cents needle or a strobe band,
    and the strings of the chosen tuning (the nearest one says how far to retune). Strum mode:
    all six open strings checked from one strum. Click a string to change its note (Custom
    tuning). The tuner listens only while this panel exists.
*/
class TunerPanel final : public juce::Component,
                         private juce::Timer
{
public:
    explicit TunerPanel (AppEngine&);
    ~TunerPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void applyMute();
    void applyMode();
    void updateTuningCombo();
    void showStringMenu (int stringIndex);
    void tuningChanged();

    void paintNote (juce::Graphics&, juce::Rectangle<float>);
    void paintMeter (juce::Graphics&, juce::Rectangle<float>);
    void paintStrobe (juce::Graphics&, juce::Rectangle<float>);
    void paintStrum (juce::Graphics&, juce::Rectangle<float>);
    void paintStrings (juce::Graphics&, juce::Rectangle<float>);
    juce::Rectangle<float> stringCell (int index) const;

    AppEngine& engine;
    Tuner& tuner;
    PitchSmoother smoother;

    float shownFrequency = 0.0f;
    float needleCents = 0.0f;       // eased towards the measured value
    float inputLevel = 0.0f;
    NoteInfo note;

    double lastTick = 0.0;
    float strobePhase = 0.0f;       // pixels, fine band (the coarse band moves at a quarter of it)

    std::optional<StrumResult> strum;
    StringNotes strumStrings {};    // tuning the strum result was measured against

    juce::Rectangle<int> noteArea, meterArea, stringsArea;
    juce::Label referenceLabel, hintLabel, tuningLabel;
    juce::Slider referenceSlider;
    juce::ComboBox tuningCombo;
    juce::TextButton singleButton { "Single string" }, strumButton { "Strum all six" },
                     needleButton { "Needle" }, strobeButton { "Strobe" };
    juce::ToggleButton muteToggle { "Mute the guitar while tuning" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TunerPanel)
};

} // namespace pv
