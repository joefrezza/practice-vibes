#include "TodayPanel.h"
#include "Theme.h"

namespace pv
{

namespace
{
    juce::Time startOfToday()
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::Time (now.getYear(), now.getMonth(), now.getDayOfMonth(), 0, 0, 0, 0, true);
    }

    juce::String progressText (const PracticeLog::TopicSummary& s)
    {
        juce::StringArray parts;

        if (s.sessions > 0)
            parts.add (formatPracticeDuration (s.totalSeconds) + " in total");

        if (s.lastBpm > 0.0)
            parts.add ("last " + juce::String (juce::roundToInt (s.lastBpm)) + " bpm");

        if (s.lastSpeed > 0.0)
            parts.add ("song at " + juce::String (juce::roundToInt (s.lastSpeed * 100.0)) + " %");

        return parts.joinIntoString ("  -  ");
    }
}

//==============================================================================
/** One rotation topic: title, when you last practised it, progress, and a Practise button. */
class TodayPanel::RotationRow final : public juce::Component,
                                      public juce::SettableTooltipClient
{
public:
    RotationRow (const Topic& topic, const PracticeLog::TopicSummary& s, bool isActive)
        : topicId (topic.id), title (topic.title.isNotEmpty() ? topic.title : juce::String ("(untitled)")),
          tags (topic.tags), summary (s), active (isActive)
    {
        practiseButton.setColour (juce::TextButton::buttonColourId, theme::accent);
        practiseButton.onClick = [this] { if (onPractise) onPractise (topicId); };
        practiseButton.setButtonText (active ? "Practising" : "Practise");
        practiseButton.setEnabled (! active);
        addAndMakeVisible (practiseButton);
        historyButton.onClick = [this] { if (onHistory) onHistory (topicId); };
        historyButton.setTooltip ("Tempo, song speed and minutes over the weeks, and every session");
        historyButton.setEnabled (s.sessions > 0);
        addAndMakeVisible (historyButton);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("Click to open the topic; Practise brings back last time's song, loop, speed and tempo");
    }

    std::function<void (const juce::String&)> onPractise, onShow, onHistory;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::panel);
        g.fillRoundedRectangle (r, 6.0f);

        if (active)
        {
            g.setColour (theme::accent.brighter (0.3f));
            g.drawRoundedRectangle (r.reduced (1.0f), 6.0f, 2.0f);
        }

        auto inner = getLocalBounds().reduced (12, 8);
        inner.removeFromRight (getWidth() - historyButton.getX() + 12);

        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (16.0f).withStyle ("Bold"));
        g.drawText (title, inner.removeFromTop (22), juce::Justification::centredLeft, true);

        const auto now = juce::Time::getCurrentTime();
        const auto days = summary.last == juce::Time() ? 1000.0 : (now - summary.last).inDays();
        const auto ago = summary.last == juce::Time() ? juce::String ("Not practised yet")
                                                      : "Last practised " + formatPracticeAgo (summary.last, now);
        const juce::Font agoFont (juce::FontOptions (13.5f).withStyle ("Bold"));
        g.setFont (agoFont);
        g.setColour (days >= 7.0 ? theme::highlight : juce::Colours::white.withAlpha (0.75f));
        const auto agoWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (agoFont, ago)) + 16;
        auto line = inner.removeFromTop (20);
        g.drawText (ago, line.removeFromLeft (agoWidth), juce::Justification::centredLeft, true);

        g.setFont (juce::FontOptions (13.5f));
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        auto detail = progressText (summary);

        if (! tags.isEmpty())
            detail << (detail.isNotEmpty() ? "     " : "") << "#" << tags.joinIntoString ("  #");

        g.drawText (detail, line, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        practiseButton.setBounds (r.removeFromRight (110).withSizeKeepingCentre (110, 34));
        r.removeFromRight (6);
        historyButton.setBounds (r.removeFromRight (84).withSizeKeepingCentre (84, 34));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked() && onShow)
            onShow (topicId);
    }

private:
    juce::String topicId, title;
    juce::StringArray tags;
    PracticeLog::TopicSummary summary;
    bool active;
    juce::TextButton practiseButton, historyButton { "History" };
};

//==============================================================================
/** The last two weeks, newest first, grouped by day. */
class TodayPanel::RecentView final : public juce::Component
{
public:
    void setEntries (const std::vector<PracticeEntry>& all)
    {
        lines.clear();
        const auto today = startOfToday();
        const auto from = today - juce::RelativeTime::days (13);
        juce::Time shownDay;

        for (auto it = all.rbegin(); it != all.rend(); ++it)
        {
            if (it->start < from)
                break;

            const auto day = juce::Time (it->start.getYear(), it->start.getMonth(), it->start.getDayOfMonth(), 0, 0, 0, 0, true);

            if (day != shownDay)
            {
                shownDay = day;
                double total = 0.0;

                for (auto& e : all)
                    if (e.start >= day && e.start < day + juce::RelativeTime::days (1))
                        total += e.playedSeconds;

                const auto name = day == today ? juce::String ("Today")
                                : day == today - juce::RelativeTime::days (1) ? juce::String ("Yesterday")
                                : day.formatted ("%a %d %b");
                lines.push_back ({ Line::day, name, formatPracticeDuration (total) });
            }

            auto what = it->isFree() ? juce::String ("Free practice") : it->topicTitle;

            if (it->isFree() && it->songName.isNotEmpty())
                what << ": " << it->songName;

            juce::StringArray detail { formatPracticeDuration (it->playedSeconds) };

            if (it->maxBpm > 0.0)    detail.add (juce::String (juce::roundToInt (it->maxBpm)) + " bpm");
            if (it->maxSpeed > 0.0)  detail.add (juce::String (juce::roundToInt (it->maxSpeed * 100.0)) + " %");
            if (it->looper)          detail.add ("looper");

            lines.push_back ({ Line::entry, what, detail.joinIntoString ("  -  ") });

            if (it->note.isNotEmpty())
                lines.push_back ({ Line::note, it->note.replaceCharacters ("\r\n", "  "), {} });
        }

        setSize (getWidth(), getContentHeight());
        repaint();
    }

    int getContentHeight() const
    {
        int h = 4;

        for (auto& l : lines)
            h += heightOf (l);

        return juce::jmax (40, h);
    }

    void paint (juce::Graphics& g) override
    {
        if (lines.empty())
        {
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.setFont (juce::FontOptions (14.0f));
            g.drawFittedText ("Nothing yet. Play, and the time you spend on each topic is recorded here by itself.",
                              getLocalBounds().reduced (4), juce::Justification::topLeft, 3);
            return;
        }

        int y = 4;

        for (auto& l : lines)
        {
            const auto h = heightOf (l);
            auto r = juce::Rectangle<int> (0, y, getWidth(), h).reduced (4, 0);
            y += h;

            switch (l.kind)
            {
                case Line::day:
                    r.removeFromTop (8);
                    g.setColour (juce::Colours::white.withAlpha (0.12f));
                    g.fillRect (r.getX(), r.getBottom() - 1, r.getWidth(), 1);
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::FontOptions (14.5f).withStyle ("Bold"));
                    g.drawText (l.text, r, juce::Justification::centredLeft, true);
                    g.setColour (juce::Colours::white.withAlpha (0.7f));
                    g.drawText (l.detail, r, juce::Justification::centredRight, true);
                    break;

                case Line::entry:
                {
                    g.setFont (juce::FontOptions (14.0f));
                    const auto detailWidth = juce::jmin (r.getWidth() / 2,
                                                         juce::roundToInt (juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions (14.0f)), l.detail)) + 8);
                    g.setColour (juce::Colours::white.withAlpha (0.6f));
                    g.drawText (l.detail, r.removeFromRight (detailWidth), juce::Justification::centredRight, true);
                    g.setColour (juce::Colours::white.withAlpha (0.9f));
                    g.drawText (l.text, r.withTrimmedLeft (10), juce::Justification::centredLeft, true);
                    break;
                }

                case Line::note:
                    g.setColour (theme::highlight.withAlpha (0.8f));
                    g.setFont (juce::FontOptions (13.0f).withStyle ("Italic"));
                    g.drawText (l.text, r.withTrimmedLeft (22), juce::Justification::centredLeft, true);
                    break;
            }
        }
    }

private:
    struct Line
    {
        enum Kind { day, entry, note } kind;
        juce::String text, detail;
    };

    static int heightOf (const Line& l)     { return l.kind == Line::day ? 34 : (l.kind == Line::entry ? 24 : 20); }

    std::vector<Line> lines;
};

//==============================================================================
TodayPanel::TodayPanel (PracticeTracker& t, const PracticeSession& s) : tracker (t), session (s)
{
    titleLabel.setFont (juce::FontOptions (22.0f).withStyle ("Bold"));
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (titleLabel);

    totalsLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.75f));
    totalsLabel.setJustificationType (juce::Justification::centredRight);
    totalsLabel.setFont (juce::FontOptions (15.0f));
    addAndMakeVisible (totalsLabel);

    nowTitle.setFont (juce::FontOptions (19.0f).withStyle ("Bold"));
    nowTitle.setColour (juce::Label::textColourId, juce::Colours::white);
    nowTitle.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (nowTitle);

    nowStatus.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
    nowStatus.setFont (juce::FontOptions (14.0f));
    nowStatus.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (nowStatus);

    noteEditor.setTextToShowWhenEmpty ("Note for this session (optional): what went well, what to fix next time",
                                       juce::Colours::white.withAlpha (0.35f));
    noteEditor.setFont (juce::FontOptions (14.5f));
    noteEditor.setColour (juce::TextEditor::backgroundColourId, theme::background);
    noteEditor.setColour (juce::TextEditor::outlineColourId, juce::Colours::white.withAlpha (0.15f));
    noteEditor.setColour (juce::TextEditor::focusedOutlineColourId, theme::accent);
    noteEditor.setJustification (juce::Justification::centredLeft);
    noteEditor.onTextChange = [this] { tracker.setNote (noteEditor.getText().trim()); };
    noteEditor.onReturnKey = [this] { unfocusAllComponents(); };
    addAndMakeVisible (noteEditor);

    stopButton.setTooltip ("Stop practising this topic (playing is then logged as free practice)");
    stopButton.onClick = [this]
    {
        tracker.setActiveTopic ({}, {});
        refresh();
    };
    addAndMakeVisible (stopButton);

    for (auto* l : { &rotationHeader, &recentHeader })
    {
        l->setFont (theme::headerFont());
        l->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.85f));
        addAndMakeVisible (*l);
    }

    rotationHeader.setText ("Practice rotation", juce::dontSendNotification);
    rotationHeader.setTooltip ("Longest neglected first");

    diceButton.setTooltip ("Pick one rotation topic at random - the longer you've left one, the likelier it comes up");
    diceButton.onClick = [this] { rollDice(); };
    planButton.setTooltip ("Say how long you have: a few rotation topics with time slots, ready to start");
    planButton.setColour (juce::TextButton::buttonColourId, theme::accent);
    planButton.onClick = [this] { if (onPlanSession) onPlanSession (focusTag()); };
    focusBox.setTooltip ("Only topics with this tag, for the dice and the session planner");
    focusBox.addItem ("All tags", 1);
    focusBox.setSelectedId (1, juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*> { &diceButton, &planButton, &focusBox })
        addAndMakeVisible (*c);

    diceLabel.setFont (juce::FontOptions (16.0f).withStyle ("Bold"));
    diceLabel.setColour (juce::Label::textColourId, juce::Colours::black);
    diceLabel.setMinimumHorizontalScale (0.7f);
    dicePractiseButton.onClick = [this]
    {
        const auto id = diceTopicId;
        diceTopicId = {};
        resized();

        if (onPractise)
            onPractise (id);
    };
    diceAgainButton.onClick = [this] { rollDice(); };
    diceCloseButton.onClick = [this] { diceTopicId = {}; resized(); repaint(); };

    for (auto* c : std::initializer_list<juce::Component*> { &diceLabel, &dicePractiseButton, &diceAgainButton, &diceCloseButton })
        addChildComponent (*c);

    sessionTitle.setFont (juce::FontOptions (17.0f).withStyle ("Bold"));
    sessionTitle.setColour (juce::Label::textColourId, juce::Colours::white);
    addChildComponent (sessionTitle);
    nextButton.setColour (juce::TextButton::buttonColourId, theme::accent);
    nextButton.onClick = [this] { if (onSessionNext) onSessionNext(); };
    extendButton.onClick = [this] { if (onSessionExtend) onSessionExtend(); };
    endButton.onClick = [this]
    {
        if (session.isRunning()) { if (onSessionEnd) onSessionEnd(); }
        else if (onSessionDismiss) onSessionDismiss();
    };

    for (auto* c : { &nextButton, &extendButton, &endButton })
        addChildComponent (*c);
    recentHeader.setText ("Last two weeks", juce::dontSendNotification);

    rotationEmpty.setJustificationType (juce::Justification::topLeft);
    rotationEmpty.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.55f));
    rotationEmpty.setFont (juce::FontOptions (15.0f));
    addChildComponent (rotationEmpty);

    rotationViewport.setViewedComponent (&rotationHolder, false);
    rotationViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (rotationViewport);

    recentView = std::make_unique<RecentView>();
    recentViewport.setViewedComponent (recentView.get(), false);
    recentViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (recentViewport);

    startTimerHz (2);
}

TodayPanel::~TodayPanel()
{
    stopTimer();
}

//==============================================================================
void TodayPanel::refresh()
{
    auto* library = getLibrary ? getLibrary() : nullptr;
    auto* log = tracker.getLog();
    rows.clear();

    std::vector<std::pair<const Topic*, PracticeLog::TopicSummary>> rotation;

    if (library != nullptr)
        for (auto& t : library->getTopics())
            if (t.inRotation)
                rotation.emplace_back (&t, log != nullptr ? log->summarise (t.id) : PracticeLog::TopicSummary {});

    // Never practised first (newest topic first), then the longest ago.
    std::stable_sort (rotation.begin(), rotation.end(), [] (const auto& a, const auto& b)
    {
        const auto na = a.second.last == juce::Time(), nb = b.second.last == juce::Time();

        if (na != nb)
            return na;

        return na ? a.first->created > b.first->created : a.second.last < b.second.last;
    });

    for (auto& [topic, summary] : rotation)
    {
        auto row = std::make_unique<RotationRow> (*topic, summary, topic->id == tracker.getActiveTopicId());
        row->onPractise = [this] (const juce::String& id) { if (onPractise) onPractise (id); };
        row->onShow = [this] (const juce::String& id) { if (onShowTopic) onShowTopic (id); };
        row->onHistory = [this] (const juce::String& id) { if (onShowHistory) onShowHistory (id); };
        rotationHolder.addAndMakeVisible (*row);
        rows.push_back (std::move (row));
    }

    rotationEmpty.setText (library == nullptr
                               ? "Set up your topics in the Topics tab first."
                               : "No topics in the rotation yet.\n\nOpen a topic in the Topics tab and tick \"In practice rotation\": "
                                 "it shows up here, with how long ago you last practised it.",
                           juce::dontSendNotification);
    rotationEmpty.setVisible (rows.empty());

    recentView->setEntries (log != nullptr ? log->getEntries() : std::vector<PracticeEntry> {});
    shownActiveId = tracker.getActiveTopicId();

    // Focus tags: keep the choice if the tag still exists.
    const auto tags = getRotationTags ? getRotationTags() : juce::StringArray();
    const auto chosen = focusTag();
    focusBox.clear (juce::dontSendNotification);
    focusBox.addItem ("All tags", 1);

    for (int i = 0; i < tags.size(); ++i)
        focusBox.addItem ("#" + tags[i], i + 2);

    const auto index = tags.indexOf (chosen, true);
    focusBox.setSelectedId (index >= 0 ? index + 2 : 1, juce::dontSendNotification);

    diceButton.setEnabled (! rows.empty());
    planButton.setEnabled (! rows.empty());

    if (diceTopicId.isNotEmpty() && (library == nullptr || library->findTopic (diceTopicId) == nullptr))
        diceTopicId = {};

    updateNow();
    updateSession();
    resized();
}

juce::String TodayPanel::focusTag() const
{
    return focusBox.getSelectedId() > 1 ? focusBox.getText().substring (1) : juce::String();
}

void TodayPanel::rollDice()
{
    auto candidates = getCandidates ? getCandidates (focusTag()) : std::vector<PlanCandidate> {};

    // Not what you're on now, nor the same pick twice in a row (when there's a choice).
    juce::StringArray exclude { tracker.getActiveTopicId(), diceTopicId };
    auto pick = pickWeighted (candidates, random, exclude);

    if (pick < 0)
        pick = pickWeighted (candidates, random);

    if (pick < 0)
    {
        diceTopicId = {};
        diceLabel.setText ({}, juce::dontSendNotification);
        resized();
        return;
    }

    const auto& c = candidates[(size_t) pick];
    diceTopicId = c.topicId;

    juce::String when = "not practised yet";

    if (auto* log = tracker.getLog())
        if (const auto last = log->summarise (c.topicId).last; last != juce::Time())
            when = "last practised " + formatPracticeAgo (last, juce::Time::getCurrentTime());

    diceLabel.setText ("The dice says: " + c.title + "   (" + when + ")", juce::dontSendNotification);
    resized();
    repaint();
}

void TodayPanel::updateSession()
{
    const auto visible = sessionVisible();

    for (juce::Component* c : { (juce::Component*) &sessionTitle, (juce::Component*) &nextButton,
                                (juce::Component*) &extendButton, (juce::Component*) &endButton })
        c->setVisible (visible);

    if (visible)
    {
        if (session.isRunning())
        {
            const auto n = (int) session.getSlots().size();
            sessionTitle.setText (session.isTimeUp() ? juce::String ("Time's up!")
                                                     : "Session: topic " + juce::String (session.getIndex() + 1) + " of " + juce::String (n),
                                  juce::dontSendNotification);
            sessionTitle.setColour (juce::Label::textColourId, session.isTimeUp() ? theme::highlight : juce::Colours::white);
            nextButton.setButtonText (session.getNext() != nullptr ? "Next topic" : "Finish");
            nextButton.setVisible (true);
            extendButton.setVisible (true);
            endButton.setButtonText ("End");
        }
        else
        {
            const auto reached = session.getTopicsReached();
            sessionTitle.setText ("Session over: " + formatPracticeDuration (session.getElapsedSeconds()) + ", "
                                      + juce::String (reached) + (reached == 1 ? " topic" : " topics"),
                                  juce::dontSendNotification);
            sessionTitle.setColour (juce::Label::textColourId, juce::Colours::white);
            nextButton.setVisible (false);
            extendButton.setVisible (false);
            endButton.setButtonText ("Close");
        }
    }

    if (visible != sessionShown)
    {
        sessionShown = visible;
        resized();
    }

    repaint (sessionCard);
}

void TodayPanel::updateNow()
{
    const auto now = juce::Time::getCurrentTime();
    titleLabel.setText ("Today, " + now.formatted ("%A %d %B"), juce::dontSendNotification);

    double today = 0.0, week = 0.0;

    if (auto* log = tracker.getLog())
    {
        const auto start = startOfToday();
        today = log->playedBetween (start, start + juce::RelativeTime::days (1));
        week = log->playedBetween (start - juce::RelativeTime::days (6), start + juce::RelativeTime::days (1));
    }

    // The open entry may not be written yet.
    if (auto* current = tracker.getCurrent(); current != nullptr && current->playedSeconds < PracticeTracker::minimumSeconds)
    {
        today += current->playedSeconds;
        week += current->playedSeconds;
    }
    else if (current != nullptr && tracker.getLog() != nullptr)
    {
        const auto& entries = tracker.getLog()->getEntries();
        const auto inLog = std::find_if (entries.begin(), entries.end(), [&] (const PracticeEntry& e) { return e.id == current->id; });
        const auto extra = current->playedSeconds - (inLog != entries.end() ? inLog->playedSeconds : 0.0);
        today += extra;
        week += extra;
    }

    totalsLabel.setText ("Played today: " + formatPracticeDuration (today) + "     Last 7 days: " + formatPracticeDuration (week),
                         juce::dontSendNotification);

    const auto active = tracker.getActiveTopicId().isNotEmpty();
    nowTitle.setText (active ? "Practising: " + tracker.getActiveTopicTitle() : juce::String ("Free practice"), juce::dontSendNotification);

    juce::String status = tracker.isPlaying() ? "Playing now" : "Not playing";

    if (active)
        status << "  -  " << formatPracticeDuration (tracker.getActiveSeconds()) << " on this topic so far";
    else
        status << "  -  pick a topic below, or just play: it's logged as free practice";

    nowStatus.setText (status, juce::dontSendNotification);
    nowStatus.setColour (juce::Label::textColourId, tracker.isPlaying() ? juce::Colour (0xff7bd88f) : juce::Colours::white.withAlpha (0.65f));
    stopButton.setVisible (active);

    if (! noteEditor.hasKeyboardFocus (true) && noteEditor.getText().trim() != tracker.getNote())
        noteEditor.setText (tracker.getNote(), false);
}

void TodayPanel::timerCallback()
{
    if (! isShowing())
        return;

    if (shownActiveId != tracker.getActiveTopicId())
        refresh();
    else
        updateNow();

    updateSession();
}

void TodayPanel::visibilityChanged()
{
    if (isVisible())
    {
        if (onBecameVisible)
            onBecameVisible();

        refresh();
    }
}

//==============================================================================
void TodayPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (nowCard.toFloat(), 8.0f);

    if (! diceStrip.isEmpty())
    {
        g.setColour (theme::highlight);
        g.fillRoundedRectangle (diceStrip.toFloat(), 6.0f);
    }

    if (sessionShown && ! sessionCard.isEmpty())
    {
        g.setColour (theme::panel);
        g.fillRoundedRectangle (sessionCard.toFloat(), 8.0f);

        if (session.isRunning() && session.isTimeUp())
        {
            g.setColour (theme::highlight);
            g.drawRoundedRectangle (sessionCard.toFloat().reduced (1.0f), 8.0f, 2.0f);
        }

        const auto& slots = session.getSlots();
        auto r = sessionLines;

        for (int i = 0; i < (int) slots.size(); ++i)
        {
            auto line = r.removeFromTop (24);
            const auto current = session.isRunning() && i == session.getIndex();
            const auto done = i < session.getIndex() || (! session.isRunning() && i <= session.getIndex());

            juce::String right = juce::String (slots[(size_t) i].minutes) + " min";

            if (current)
                right = session.isTimeUp() ? "over by " + formatClock (-session.getRemainingSeconds())
                                           : formatClock (session.getRemainingSeconds()) + " left";
            else if (done)
                right = "done";

            g.setFont (current ? juce::FontOptions (15.0f).withStyle ("Bold") : juce::FontOptions (15.0f));
            g.setColour (current ? theme::highlight : juce::Colours::white.withAlpha (done ? 0.45f : 0.8f));
            g.drawText (right, line, juce::Justification::centredRight, false);
            g.drawText (juce::String (i + 1) + ".  " + slots[(size_t) i].title,
                        line.withTrimmedRight (120), juce::Justification::centredLeft, true);
        }
    }
}

void TodayPanel::resized()
{
    auto area = getLocalBounds().reduced (12, 8);

    auto header = area.removeFromTop (44);
    totalsLabel.setBounds (header.removeFromRight (juce::jmin (420, header.getWidth() / 2)));
    titleLabel.setBounds (header);
    area.removeFromTop (6);

    const auto wide = area.getWidth() >= 900;
    juce::Rectangle<int> left, right;

    if (wide)
    {
        left = area.removeFromLeft ((int) (area.getWidth() * 0.58f));
        area.removeFromLeft (16);
        right = area;
    }
    else
    {
        left = area;
    }

    // Now card (and the session card): on top of the right column (or of everything when narrow).
    auto& cardColumn = wide ? right : left;
    nowCard = cardColumn.removeFromTop (124);
    cardColumn.removeFromTop (12);

    sessionCard = {};

    if (sessionShown)
    {
        const auto lines = (int) session.getSlots().size();
        sessionCard = cardColumn.removeFromTop (44 + 24 * lines + 52);
        cardColumn.removeFromTop (12);

        auto sc = sessionCard.reduced (14, 10);
        sessionTitle.setBounds (sc.removeFromTop (28));
        auto buttons = sc.removeFromBottom (38);
        endButton.setBounds (buttons.removeFromRight (90).reduced (0, 2));
        buttons.removeFromRight (6);
        extendButton.setBounds (buttons.removeFromRight (90).reduced (0, 2));
        buttons.removeFromRight (6);
        nextButton.setBounds (buttons.removeFromRight (120).reduced (0, 2));
        sc.removeFromTop (4);
        sessionLines = sc;
    }

    auto card = nowCard.reduced (14, 10);
    auto cardTop = card.removeFromTop (28);
    stopButton.setBounds (cardTop.removeFromRight (90).reduced (0, 1));
    nowTitle.setBounds (cardTop);
    nowStatus.setBounds (card.removeFromTop (26));
    card.removeFromTop (8);
    noteEditor.setBounds (card.removeFromTop (34));

    if (! wide)
    {
        auto recentPart = left.removeFromBottom (juce::jmax (160, left.getHeight() / 3));
        right = recentPart;
        left.removeFromBottom (12);
    }

    auto headerRow = left.removeFromTop (36);
    planButton.setBounds (headerRow.removeFromRight (150).reduced (0, 2));
    headerRow.removeFromRight (6);
    diceButton.setBounds (headerRow.removeFromRight (130).reduced (0, 2));
    headerRow.removeFromRight (6);
    focusBox.setBounds (headerRow.removeFromRight (juce::jmin (150, juce::jmax (90, headerRow.getWidth() - 160))).reduced (0, 3));
    rotationHeader.setBounds (headerRow);
    left.removeFromTop (6);

    const auto showDice = diceTopicId.isNotEmpty();
    diceStrip = {};

    for (juce::Component* c : { (juce::Component*) &diceLabel, (juce::Component*) &dicePractiseButton,
                                (juce::Component*) &diceAgainButton, (juce::Component*) &diceCloseButton })
        c->setVisible (showDice);

    if (showDice)
    {
        diceStrip = left.removeFromTop (50);
        left.removeFromTop (8);
        auto d = diceStrip.reduced (10, 7);
        diceCloseButton.setBounds (d.removeFromRight (36));
        d.removeFromRight (6);
        diceAgainButton.setBounds (d.removeFromRight (110));
        d.removeFromRight (6);
        dicePractiseButton.setBounds (d.removeFromRight (100));
        d.removeFromRight (8);
        diceLabel.setBounds (d);
    }

    rotationViewport.setBounds (left);
    rotationEmpty.setBounds (left.withHeight (juce::jmin (left.getHeight(), 120)));

    constexpr int rowHeight = 66, gap = 6;
    const auto width = rotationViewport.getMaximumVisibleWidth() - (rotationViewport.canScrollVertically() ? 4 : 0);
    rotationHolder.setSize (juce::jmax (1, width), juce::jmax (1, (int) rows.size() * (rowHeight + gap)));

    for (size_t i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (0, (int) i * (rowHeight + gap), rotationHolder.getWidth(), rowHeight);

    recentHeader.setBounds (right.removeFromTop (30));
    recentViewport.setBounds (right);
    recentView->setSize (juce::jmax (1, recentViewport.getMaximumVisibleWidth()), recentView->getContentHeight());
}

} // namespace pv
