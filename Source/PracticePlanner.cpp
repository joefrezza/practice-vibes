#include "PracticePlanner.h"

namespace pv
{

double neglectWeight (juce::Time lastPractised, juce::Time now)
{
    if (lastPractised == juce::Time())
        return 30.0;

    return juce::jlimit (0.5, 30.0, (now - lastPractised).inDays());
}

int pickWeighted (const std::vector<PlanCandidate>& candidates, juce::Random& random, const juce::StringArray& exclude)
{
    double total = 0.0;

    for (auto& c : candidates)
        if (! exclude.contains (c.topicId))
            total += juce::jmax (0.0, c.weight);

    if (total <= 0.0)
    {
        // Every weight is zero: pick evenly among what's left.
        std::vector<int> left;

        for (int i = 0; i < (int) candidates.size(); ++i)
            if (! exclude.contains (candidates[(size_t) i].topicId))
                left.push_back (i);

        return left.empty() ? -1 : left[(size_t) random.nextInt ((int) left.size())];
    }

    auto r = random.nextDouble() * total;
    int last = -1;

    for (int i = 0; i < (int) candidates.size(); ++i)
    {
        const auto& c = candidates[(size_t) i];

        if (exclude.contains (c.topicId))
            continue;

        last = i;
        r -= juce::jmax (0.0, c.weight);

        if (r < 0.0)
            return i;
    }

    return last;   // rounding at the very end
}

std::vector<SessionSlot> planSession (const std::vector<PlanCandidate>& candidates, int minutes, juce::Random& random)
{
    std::vector<SessionSlot> slots;
    minutes = juce::jmax (5, minutes);

    const auto count = juce::jlimit (1, juce::jmin (6, juce::jmax (1, (int) candidates.size())), juce::roundToInt (minutes / 12.0));
    juce::StringArray used;

    for (int i = 0; i < count; ++i)
    {
        const auto pick = pickWeighted (candidates, random, used);

        if (pick < 0)
            break;

        used.add (candidates[(size_t) pick].topicId);
        slots.push_back ({ candidates[(size_t) pick].topicId, candidates[(size_t) pick].title, 0 });
    }

    if (slots.empty())
        return slots;

    const auto each = juce::jmax (5, (minutes / (int) slots.size()) / 5 * 5);

    for (auto& s : slots)
        s.minutes = each;

    slots.front().minutes = juce::jmax (5, minutes - each * ((int) slots.size() - 1));
    return slots;
}

bool rerollSlot (std::vector<SessionSlot>& slots, size_t index, const std::vector<PlanCandidate>& candidates, juce::Random& random)
{
    if (index >= slots.size())
        return false;

    juce::StringArray used;

    for (auto& s : slots)
        used.add (s.topicId);

    const auto pick = pickWeighted (candidates, random, used);

    if (pick < 0)
        return false;

    slots[index].topicId = candidates[(size_t) pick].topicId;
    slots[index].title = candidates[(size_t) pick].title;
    return true;
}

int totalMinutes (const std::vector<SessionSlot>& slots)
{
    int total = 0;

    for (auto& s : slots)
        total += s.minutes;

    return total;
}

//==============================================================================
void PracticeSession::start (std::vector<SessionSlot> newSlots)
{
    slots = std::move (newSlots);
    index = 0;
    slotElapsed = extraSeconds = totalElapsed = 0.0;
    announced = false;
    finished = false;
    running = ! slots.empty();
}

void PracticeSession::stop()
{
    finished = running;
    running = false;
}

const SessionSlot* PracticeSession::getCurrent() const noexcept
{
    return running && juce::isPositiveAndBelow (index, (int) slots.size()) ? &slots[(size_t) index] : nullptr;
}

const SessionSlot* PracticeSession::getNext() const noexcept
{
    return running && juce::isPositiveAndBelow (index + 1, (int) slots.size()) ? &slots[(size_t) index + 1] : nullptr;
}

bool PracticeSession::tick (double seconds)
{
    if (! running)
        return false;

    seconds = juce::jlimit (0.0, 5.0, seconds);
    slotElapsed += seconds;
    totalElapsed += seconds;

    if (! announced && isTimeUp())
    {
        announced = true;
        return true;
    }

    return false;
}

double PracticeSession::getRemainingSeconds() const noexcept
{
    const auto* s = getCurrent();
    return s == nullptr ? 0.0 : s->minutes * 60.0 + extraSeconds - slotElapsed;
}

bool PracticeSession::next()
{
    if (! running)
        return false;

    if (index + 1 >= (int) slots.size())
    {
        running = false;
        finished = true;
        return false;
    }

    ++index;
    slotElapsed = extraSeconds = 0.0;
    announced = false;
    return true;
}

void PracticeSession::extend (int minutes)
{
    if (! running)
        return;

    // From now if already over time, so "+5 min" always gives five more minutes.
    if (isTimeUp())
        extraSeconds += -getRemainingSeconds();

    extraSeconds += minutes * 60.0;
    announced = false;
}

juce::String formatClock (double seconds)
{
    const auto negative = seconds < 0.0;
    const auto total = (int) std::ceil (std::abs (seconds) - 1.0e-9);
    const auto s = total % 60;
    return (negative ? "-" : "") + juce::String (total / 60) + ":" + (s < 10 ? "0" : "") + juce::String (s);
}

} // namespace pv
