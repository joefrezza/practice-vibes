#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PracticeLog.h"
#include "PracticePlanner.h"

namespace pv
{

/** Overlay: "I have 45 minutes" (+ an optional focus tag) -> a few rotation topics with time slots.
    Any slot can be re-rolled, retimed or removed before starting. Nothing is saved as a routine.
*/
class SessionPlanner final : public juce::Component
{
public:
    using CandidatesFn = std::function<std::vector<PlanCandidate> (const juce::String& tag)>;

    SessionPlanner (CandidatesFn, const juce::StringArray& tags, int minutes, const juce::String& tag);
    ~SessionPlanner() override;

    std::function<void (std::vector<SessionSlot>, int minutes, const juce::String& tag)> onStart;
    std::function<void()> onCancel;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class SlotRow;

    void replan();
    void rebuildRows();
    void updateTotal();
    juce::String currentTag() const;

    CandidatesFn candidatesFor;
    std::vector<PlanCandidate> candidates;
    std::vector<SessionSlot> slots;
    juce::Random random;

    juce::Label timeLabel, focusLabel, totalLabel, emptyLabel;
    juce::ComboBox timeBox, focusBox;
    juce::TextButton shuffleButton { "Shuffle" }, addButton { "+ Add a topic" },
                     startButton { "Start session" }, cancelButton { "Cancel" };
    std::vector<std::unique_ptr<SlotRow>> rows;
};

/** Overlay: one topic's practice history - tempo and song speed over time, minutes per week, every session. */
class TopicHistory final : public juce::Component
{
public:
    TopicHistory (const PracticeLog&, const juce::String& topicId);
    ~TopicHistory() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class LineChart;
    class WeekBars;
    class SessionList;

    juce::Label statsLabel, emptyLabel, sessionsHeader;
    std::unique_ptr<LineChart> tempoChart, speedChart;
    std::unique_ptr<WeekBars> weekBars;
    std::unique_ptr<SessionList> sessionList;
    juce::Viewport sessionViewport;
};

} // namespace pv
