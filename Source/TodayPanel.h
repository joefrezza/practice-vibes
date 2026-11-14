#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Library.h"
#include "PracticeLog.h"

namespace pv
{

/** The "Today" tab: what you're practising now, the rotation topics you've neglected longest,
    and what you practised recently. Everything here is recorded automatically by the PracticeTracker.
*/
class TodayPanel final : public juce::Component,
                         private juce::Timer
{
public:
    explicit TodayPanel (PracticeTracker&);
    ~TodayPanel() override;

    std::function<Library*()> getLibrary;
    std::function<void (const juce::String& topicId)> onPractise;
    std::function<void (const juce::String& topicId)> onShowTopic;
    std::function<void()> onBecameVisible;          // re-read the library and the log (other devices)

    /** Rebuilds the lists (topics or log changed). */
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

private:
    class RotationRow;
    class RecentView;

    void timerCallback() override;
    void updateNow();

    PracticeTracker& tracker;

    juce::Label titleLabel, totalsLabel;
    juce::Label nowTitle, nowStatus;
    juce::TextEditor noteEditor;
    juce::TextButton stopButton { "Stop" };
    juce::Rectangle<int> nowCard;

    juce::Label rotationHeader, rotationEmpty, recentHeader;
    juce::Viewport rotationViewport, recentViewport;
    juce::Component rotationHolder;
    std::vector<std::unique_ptr<RotationRow>> rows;
    std::unique_ptr<RecentView> recentView;
    juce::String shownActiveId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TodayPanel)
};

} // namespace pv
