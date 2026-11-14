#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "AppEngine.h"

namespace pv
{

/** Footswitch setup (shown in the overlay): learn each switch of a MIDI pedal board and
    choose what a tap and a hold do. Opened from the Looper and Guitar rig tabs.
*/
class FootswitchPanel final : public juce::Component,
                              private juce::Timer
{
public:
    explicit FootswitchPanel (AppEngine&);
    ~FootswitchPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class SwitchRow;

    void timerCallback() override;

    AppEngine& engine;
    Footswitch& footswitch;

    juce::Label intro, devicesLabel;
    juce::TextButton learnAllButton { "Learn A-D in a row" }, defaultsButton { "Default actions" };
    std::array<std::unique_ptr<SwitchRow>, Footswitch::numSwitches> rows;
    bool learningAll = false;
    int learnAllIndex = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FootswitchPanel)
};

} // namespace pv
