#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Library.h"
#include "PracticeLog.h"
#include "PracticePlanner.h"

namespace pv
{

/** The "Today" tab: what you're practising now, a running session, the rotation topics you've neglected longest
    (with the dice and the session planner), and what you practised recently. Everything is recorded
    automatically by the PracticeTracker.
*/
class TodayPanel final : public juce::Component,
                         private juce::Timer
{
public:
    TodayPanel (PracticeTracker&, const PracticeSession&);
    ~TodayPanel() override;

    std::function<Library*()> getLibrary;
    std::function<void (const juce::String& topicId)> onPractise;
    std::function<void (const juce::String& topicId)> onShowTopic;
    std::function<void()> onBecameVisible;          // re-read the library and the log (other devices)
    std::function<void (const juce::String& topicId)> onShowHistory;

    // Dice and sessions
    std::function<std::vector<PlanCandidate> (const juce::String& tag)> getCandidates;
    std::function<juce::StringArray()> getRotationTags;
    std::function<void (const juce::String& tag)> onPlanSession;
    std::function<void()> onSessionNext, onSessionExtend, onSessionEnd, onSessionDismiss;

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
    void updateSession();
    void rollDice();
    juce::String focusTag() const;
    bool sessionVisible() const noexcept    { return session.isRunning() || session.isFinished(); }

    PracticeTracker& tracker;
    const PracticeSession& session;
    juce::Random random;

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

    juce::TextButton diceButton { "Roll the dice" }, planButton { "Plan a session..." };
    juce::ComboBox focusBox;

    juce::String diceTopicId;
    juce::Label diceLabel;
    juce::TextButton dicePractiseButton { "Practise" }, diceAgainButton { "Roll again" }, diceCloseButton { "X" };
    juce::Rectangle<int> diceStrip;

    juce::Rectangle<int> sessionCard, sessionLines;
    juce::Label sessionTitle;
    juce::TextButton nextButton { "Next topic" }, extendButton { "+5 min" }, endButton { "End" };
    bool sessionShown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TodayPanel)
};

} // namespace pv
