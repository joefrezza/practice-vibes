#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace pv
{

/** A rotation topic the dice or the session planner may pick, with how much it "wants" to be picked. */
struct PlanCandidate
{
    juce::String topicId, title;
    double weight = 1.0;
};

/** Neglected topics weigh more: days since last practised (0.5 if today, at most 30); never practised = 30. */
double neglectWeight (juce::Time lastPractised, juce::Time now);

/** Index of a weighted random pick, skipping the excluded ids; -1 if nothing is left. */
int pickWeighted (const std::vector<PlanCandidate>&, juce::Random&, const juce::StringArray& exclude = {});

struct SessionSlot
{
    juce::String topicId, title;
    int minutes = 10;
};

/** About one topic per 12 minutes (1 to 6, never more than there are topics), each picked at most once,
    minutes in steps of 5 (the first slot takes what's left over). */
std::vector<SessionSlot> planSession (const std::vector<PlanCandidate>&, int totalMinutes, juce::Random&);

/** Replaces one slot's topic with another one not already in the session; false if there's none. */
bool rerollSlot (std::vector<SessionSlot>&, size_t index, const std::vector<PlanCandidate>&, juce::Random&);

int totalMinutes (const std::vector<SessionSlot>&);

/** A running session: the current slot's clock, "time's up" once per slot, and moving on when you say so. */
class PracticeSession
{
public:
    void start (std::vector<SessionSlot>);
    void stop();                                            // ends early (the summary stays until dismissed)

    bool isRunning() const noexcept                         { return running; }
    bool isFinished() const noexcept                        { return finished; }   // ended, summary not dismissed yet
    void dismiss() noexcept                                 { finished = false; }
    int getTopicsReached() const noexcept                   { return slots.empty() ? 0 : index + 1; }
    const std::vector<SessionSlot>& getSlots() const noexcept { return slots; }
    int getIndex() const noexcept                           { return index; }
    const SessionSlot* getCurrent() const noexcept;
    const SessionSlot* getNext() const noexcept;

    /** Real time passing; true once, when the current slot's time runs out. */
    bool tick (double seconds);

    double getRemainingSeconds() const noexcept;            // negative when over time
    bool isTimeUp() const noexcept                          { return getRemainingSeconds() <= 0.0; }
    double getElapsedSeconds() const noexcept               { return totalElapsed; }

    /** Moves to the next slot; false when that was the last one (the session is over). */
    bool next();

    void extend (int minutes);

private:
    std::vector<SessionSlot> slots;
    int index = 0;
    double slotElapsed = 0.0, extraSeconds = 0.0, totalElapsed = 0.0;
    bool running = false, announced = false, finished = false;
};

/** "6:05", "-1:30". */
juce::String formatClock (double seconds);

} // namespace pv
