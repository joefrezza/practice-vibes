#include "PracticeViews.h"
#include "Theme.h"

namespace pv
{

namespace
{
    const juce::Colour seriesColour { 0xff5b8fd0 };              // the accent, lifted for marks on the dark surface
    const juce::Colour gridColour = juce::Colours::white.withAlpha (0.08f);
    const juce::Colour axisText = juce::Colours::white.withAlpha (0.55f);
    const juce::Colour primaryText = juce::Colours::white.withAlpha (0.92f);

    juce::Label& styleCaption (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (theme::headerFont());
        l.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.85f));
        return l;
    }

    /** Tick step of 1, 2 or 5 x 10^n giving about `count` intervals over `range`. */
    double niceStep (double range, int count)
    {
        const auto raw = juce::jmax (1.0e-9, range / juce::jmax (1, count));
        const auto magnitude = std::pow (10.0, std::floor (std::log10 (raw)));
        const auto f = raw / magnitude;
        return magnitude * (f <= 1.0 ? 1.0 : f <= 2.0 ? 2.0 : f <= 5.0 ? 5.0 : 10.0);
    }

    float textWidth (const juce::Font& f, const juce::String& s)
    {
        return juce::GlyphArrangement::getStringWidth (f, s);
    }

    /** Small dark box with one line of text near `anchor`, kept inside `bounds`. */
    void drawTooltip (juce::Graphics& g, juce::Point<float> anchor, const juce::String& text, juce::Rectangle<int> bounds)
    {
        const juce::Font font (juce::FontOptions (13.0f));
        const auto w = textWidth (font, text) + 16.0f, h = 24.0f;
        auto box = juce::Rectangle<float> (anchor.x - w * 0.5f, anchor.y - h - 12.0f, w, h);

        if (box.getY() < (float) bounds.getY())
            box.setY (anchor.y + 12.0f);

        box = box.constrainedWithin (bounds.toFloat());
        g.setColour (juce::Colour (0xff111216));
        g.fillRoundedRectangle (box, 5.0f);
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.drawRoundedRectangle (box, 5.0f, 1.0f);
        g.setColour (primaryText);
        g.setFont (font);
        g.drawText (text, box, juce::Justification::centred, false);
    }
}

//==============================================================================
/** One planned slot: number, topic, minutes, Re-roll, Remove. */
class SessionPlanner::SlotRow final : public juce::Component
{
public:
    SlotRow (int number, const SessionSlot& slot) : index (number), title (slot.title)
    {
        for (int m = 5; m <= 60; m += 5)
            minutesBox.addItem (juce::String (m) + " min", m);

        if (minutesBox.indexOfItemId (slot.minutes) < 0)
            minutesBox.addItem (juce::String (slot.minutes) + " min", slot.minutes);

        minutesBox.setSelectedId (slot.minutes, juce::dontSendNotification);
        minutesBox.onChange = [this] { if (onMinutes) onMinutes (minutesBox.getSelectedId()); };
        rerollButton.onClick = [this] { if (onReroll) onReroll(); };
        removeButton.onClick = [this] { if (onRemove) onRemove(); };
        rerollButton.setTooltip ("Another rotation topic (neglected ones come up more often)");

        for (auto* c : std::initializer_list<juce::Component*> { &minutesBox, &rerollButton, &removeButton })
            addAndMakeVisible (*c);
    }

    std::function<void (int)> onMinutes;
    std::function<void()> onReroll, onRemove;

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        auto r = getLocalBounds().reduced (12, 0).withRight (minutesBox.getX() - 8);
        g.setColour (axisText);
        g.setFont (juce::FontOptions (15.0f).withStyle ("Bold"));
        g.drawText (juce::String (index) + ".", r.removeFromLeft (28), juce::Justification::centredLeft);
        g.setColour (primaryText);
        g.drawText (title, r, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 6);
        removeButton.setBounds (r.removeFromRight (40));
        r.removeFromRight (6);
        rerollButton.setBounds (r.removeFromRight (90));
        r.removeFromRight (6);
        minutesBox.setBounds (r.removeFromRight (100));
    }

private:
    int index;
    juce::String title;
    juce::ComboBox minutesBox;
    juce::TextButton rerollButton { "Re-roll" }, removeButton { "X" };
};

SessionPlanner::SessionPlanner (CandidatesFn fn, const juce::StringArray& tags, int minutes, const juce::String& tag)
    : candidatesFor (std::move (fn))
{
    addAndMakeVisible (styleCaption (timeLabel, "I have"));
    addAndMakeVisible (styleCaption (focusLabel, "Focus"));

    for (int m : { 15, 20, 30, 45, 60, 75, 90, 120 })
        timeBox.addItem (juce::String (m) + " minutes", m);

    timeBox.setSelectedId (timeBox.indexOfItemId (minutes) >= 0 ? minutes : 45, juce::dontSendNotification);
    timeBox.onChange = [this] { replan(); };
    addAndMakeVisible (timeBox);

    focusBox.addItem ("All rotation topics", 1);

    for (int i = 0; i < tags.size(); ++i)
        focusBox.addItem ("#" + tags[i], i + 2);

    const auto tagIndex = tags.indexOf (tag, true);
    focusBox.setSelectedId (tagIndex >= 0 ? tagIndex + 2 : 1, juce::dontSendNotification);
    focusBox.onChange = [this] { replan(); };
    focusBox.setTooltip ("Only topics with this tag (on the topic or one of its resources)");
    addAndMakeVisible (focusBox);

    shuffleButton.onClick = [this] { replan(); };
    shuffleButton.setTooltip ("Pick again (neglected topics come up more often)");
    addAndMakeVisible (shuffleButton);

    addButton.onClick = [this]
    {
        juce::StringArray used;

        for (auto& s : slots)
            used.add (s.topicId);

        const auto pick = pickWeighted (candidates, random, used);

        if (pick >= 0)
        {
            slots.push_back ({ candidates[(size_t) pick].topicId, candidates[(size_t) pick].title, 10 });
            rebuildRows();
        }
    };
    addAndMakeVisible (addButton);

    totalLabel.setColour (juce::Label::textColourId, primaryText);
    totalLabel.setFont (juce::FontOptions (15.0f));
    addAndMakeVisible (totalLabel);

    emptyLabel.setColour (juce::Label::textColourId, axisText);
    emptyLabel.setJustificationType (juce::Justification::centred);
    emptyLabel.setText ("No rotation topics to pick from. Tick \"In practice rotation\" on topics in the Topics tab.",
                        juce::dontSendNotification);
    addChildComponent (emptyLabel);

    startButton.setColour (juce::TextButton::buttonColourId, theme::accent);
    startButton.onClick = [this]
    {
        if (onStart && ! slots.empty())
            onStart (slots, timeBox.getSelectedId(), currentTag());
    };
    cancelButton.onClick = [this] { if (onCancel) onCancel(); };
    addAndMakeVisible (startButton);
    addAndMakeVisible (cancelButton);

    replan();
}

SessionPlanner::~SessionPlanner() = default;

juce::String SessionPlanner::currentTag() const
{
    return focusBox.getSelectedId() > 1 ? focusBox.getText().substring (1) : juce::String();
}

void SessionPlanner::replan()
{
    candidates = candidatesFor ? candidatesFor (currentTag()) : std::vector<PlanCandidate> {};
    slots = planSession (candidates, timeBox.getSelectedId(), random);
    rebuildRows();
}

void SessionPlanner::rebuildRows()
{
    rows.clear();

    for (size_t i = 0; i < slots.size(); ++i)
    {
        auto row = std::make_unique<SlotRow> ((int) i + 1, slots[i]);
        row->onMinutes = [this, i] (int m) { if (i < slots.size()) { slots[i].minutes = m; updateTotal(); } };
        row->onReroll = [this, i]
        {
            if (rerollSlot (slots, i, candidates, random))
                rebuildRows();
        };
        row->onRemove = [this, i]
        {
            if (i < slots.size())
            {
                slots.erase (slots.begin() + (std::ptrdiff_t) i);
                rebuildRows();
            }
        };
        addAndMakeVisible (*row);
        rows.push_back (std::move (row));
    }

    emptyLabel.setVisible (candidates.empty());
    updateTotal();
    resized();
}

void SessionPlanner::updateTotal()
{
    totalLabel.setText (slots.empty() ? juce::String() : "Total: " + juce::String (totalMinutes (slots)) + " min, "
                                                           + juce::String ((int) slots.size()) + (slots.size() == 1 ? " topic" : " topics"),
                        juce::dontSendNotification);
    startButton.setEnabled (! slots.empty());
    juce::StringArray used;

    for (auto& s : slots)
        used.add (s.topicId);

    addButton.setEnabled (pickWeighted (candidates, random, used) >= 0 && slots.size() < 8);
}

void SessionPlanner::paint (juce::Graphics& g)
{
    g.setColour (theme::background);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
}

void SessionPlanner::resized()
{
    auto area = getLocalBounds().reduced (16);

    auto top = area.removeFromTop (36);
    timeLabel.setBounds (top.removeFromLeft (70));
    timeBox.setBounds (top.removeFromLeft (150).reduced (0, 2));
    top.removeFromLeft (20);
    focusLabel.setBounds (top.removeFromLeft (60));
    shuffleButton.setBounds (top.removeFromRight (100).reduced (0, 2));
    top.removeFromRight (10);
    focusBox.setBounds (top.reduced (0, 2));

    auto bottom = area.removeFromBottom (44);
    startButton.setBounds (bottom.removeFromRight (150).reduced (3));
    cancelButton.setBounds (bottom.removeFromRight (110).reduced (3));
    totalLabel.setBounds (bottom);

    area.removeFromTop (14);
    emptyLabel.setBounds (area);

    constexpr int rowHeight = 48, gap = 6;

    for (auto& row : rows)
    {
        row->setBounds (area.removeFromTop (rowHeight));
        area.removeFromTop (gap);
    }

    addButton.setBounds (area.removeFromTop (36).removeFromLeft (160));
}

//==============================================================================
/** One measure over time: thin line, 8 px dots, latest (and best) value labelled, hover shows a session. */
class TopicHistory::LineChart final : public juce::Component
{
public:
    struct Point { juce::Time when; double value; double minutes; };

    LineChart (const juce::String& titleText, const juce::String& unitText, std::vector<Point> pts)
        : title (titleText), unit (unitText), points (std::move (pts))
    {
        setRepaintsOnMouseActivity (true);
    }

    bool hasData() const noexcept    { return ! points.empty(); }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds();
        g.setColour (primaryText);
        g.setFont (juce::FontOptions (15.0f).withStyle ("Bold"));
        g.drawText (title, area.removeFromTop (24), juce::Justification::centredLeft);

        if (points.empty())
            return;

        layout();
        const juce::Font small (juce::FontOptions (12.0f));
        g.setFont (small);

        // Recessive grid with y labels.
        for (auto v = yMin; v <= yMax + yStep * 0.5; v += yStep)
        {
            const auto y = yFor (v);
            g.setColour (gridColour);
            g.fillRect (plot.getX(), juce::roundToInt (y), plot.getWidth(), 1);
            g.setColour (axisText);
            g.drawText (juce::String (juce::roundToInt (v)), 0, juce::roundToInt (y) - 8, plot.getX() - 8, 16, juce::Justification::centredRight);
        }

        // X labels: first, last (and the middle when there's room).
        g.setColour (axisText);
        const auto labelY = plot.getBottom() + 4;
        g.drawText (points.front().when.formatted ("%d %b"), plot.getX(), labelY, 80, 16, juce::Justification::centredLeft);

        if (points.size() > 1)
            g.drawText (points.back().when.formatted ("%d %b"), plot.getRight() - 80, labelY, 80, 16, juce::Justification::centredRight);

        // Line, then markers with a surface ring so overlapping dots stay distinct.
        if (points.size() > 1)
        {
            juce::Path line;

            for (size_t i = 0; i < points.size(); ++i)
            {
                const auto p = posFor (points[i]);

                if (i == 0) line.startNewSubPath (p);
                else        line.lineTo (p);
            }

            g.setColour (seriesColour);
            g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        for (size_t i = 0; i < points.size(); ++i)
        {
            const auto p = posFor (points[i]);
            const auto hovered = (int) i == hoverIndex;
            const auto r = hovered ? 6.0f : 4.0f;
            g.setColour (theme::background);
            g.fillEllipse (p.x - r - 2.0f, p.y - r - 2.0f, (r + 2.0f) * 2.0f, (r + 2.0f) * 2.0f);
            g.setColour (seriesColour);
            g.fillEllipse (p.x - r, p.y - r, r * 2.0f, r * 2.0f);
        }

        // Direct labels: the latest value, and the best one if it isn't the latest.
        auto best = std::max_element (points.begin(), points.end(), [] (const Point& a, const Point& b) { return a.value < b.value; });
        g.setFont (juce::FontOptions (12.5f).withStyle ("Bold"));
        g.setColour (primaryText);
        labelPoint (g, points.back(), "now " + format (points.back().value));

        if (best != points.end() - 1 && best->value > points.back().value)
            labelPoint (g, *best, "best " + format (best->value));

        if (juce::isPositiveAndBelow (hoverIndex, (int) points.size()))
        {
            const auto& p = points[(size_t) hoverIndex];
            drawTooltip (g, posFor (p), p.when.formatted ("%a %d %b") + ": " + format (p.value) + ", "
                                            + formatPracticeDuration (p.minutes * 60.0), getLocalBounds());
        }
    }

    void mouseMove (const juce::MouseEvent& e) override    { setHover (e.position); }
    void mouseDrag (const juce::MouseEvent& e) override    { setHover (e.position); }
    void mouseExit (const juce::MouseEvent&) override       { hoverIndex = -1; repaint(); }

private:
    void layout()
    {
        plot = getLocalBounds().withTrimmedTop (34).withTrimmedBottom (22).withTrimmedLeft (44).withTrimmedRight (40);

        auto lo = points.front().value, hi = lo;

        for (auto& p : points)
        {
            lo = juce::jmin (lo, p.value);
            hi = juce::jmax (hi, p.value);
        }

        if (hi - lo < 1.0e-9)
        {
            lo -= 5.0;
            hi += 5.0;
        }

        yStep = niceStep (hi - lo, 3);
        yMin = std::floor (lo / yStep) * yStep;
        yMax = std::ceil (hi / yStep) * yStep;

        t0 = (double) points.front().when.toMilliseconds();
        t1 = (double) points.back().when.toMilliseconds();
    }

    float yFor (double v) const
    {
        return (float) juce::jmap (v, yMin, yMax, (double) plot.getBottom(), (double) plot.getY());
    }

    juce::Point<float> posFor (const Point& p) const
    {
        const auto x = t1 > t0 ? juce::jmap ((double) p.when.toMilliseconds(), t0, t1, (double) plot.getX(), (double) plot.getRight())
                               : (double) plot.getCentreX();
        return { (float) x, yFor (p.value) };
    }

    juce::String format (double v) const     { return juce::String (juce::roundToInt (v)) + unit; }

    void labelPoint (juce::Graphics& g, const Point& p, const juce::String& text)
    {
        const auto pos = posFor (p);
        const auto w = textWidth (g.getCurrentFont(), text) + 4.0f;
        auto x = juce::jlimit (0.0f, (float) getWidth() - w, pos.x - w * 0.5f);
        g.drawText (text, juce::Rectangle<float> (x, pos.y - 26.0f, w, 16.0f), juce::Justification::centred, false);
    }

    void setHover (juce::Point<float> pos)
    {
        int best = -1;
        float bestDistance = 18.0f;

        if (! points.empty())
        {
            layout();

            for (size_t i = 0; i < points.size(); ++i)
            {
                const auto d = std::abs (posFor (points[i]).x - pos.x);

                if (d < bestDistance)
                {
                    bestDistance = d;
                    best = (int) i;
                }
            }
        }

        if (best != hoverIndex)
        {
            hoverIndex = best;
            repaint();
        }
    }

    juce::String title, unit;
    std::vector<Point> points;
    juce::Rectangle<int> plot;
    double yMin = 0.0, yMax = 1.0, yStep = 1.0, t0 = 0.0, t1 = 0.0;
    int hoverIndex = -1;
};

//==============================================================================
/** Minutes per week: bars from the baseline with rounded tops, a 2 px gap, hover shows the week. */
class TopicHistory::WeekBars final : public juce::Component
{
public:
    WeekBars (std::vector<double> secondsPerWeek, juce::Time thisWeek)
        : weeks (std::move (secondsPerWeek)), lastWeekStart (thisWeek)
    {
        setRepaintsOnMouseActivity (true);
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds();
        g.setColour (primaryText);
        g.setFont (juce::FontOptions (15.0f).withStyle ("Bold"));
        g.drawText ("Minutes per week", area.removeFromTop (24), juce::Justification::centredLeft);

        if (weeks.empty())
            return;

        const auto plot = plotArea();
        auto maxMinutes = 0.0;

        for (auto s : weeks)
            maxMinutes = juce::jmax (maxMinutes, s / 60.0);

        const auto step = niceStep (juce::jmax (10.0, maxMinutes), 2);
        const auto top = juce::jmax (step, std::ceil (maxMinutes / step) * step);
        const juce::Font small (juce::FontOptions (12.0f));
        g.setFont (small);

        for (auto v = 0.0; v <= top + step * 0.5; v += step)
        {
            const auto y = juce::roundToInt (juce::jmap (v, 0.0, top, (double) plot.getBottom(), (double) plot.getY()));
            g.setColour (gridColour);
            g.fillRect (plot.getX(), y, plot.getWidth(), 1);
            g.setColour (axisText);
            g.drawText (juce::String (juce::roundToInt (v)), 0, y - 8, plot.getX() - 8, 16, juce::Justification::centredRight);
        }

        const auto slot = (float) plot.getWidth() / (float) weeks.size();

        for (size_t i = 0; i < weeks.size(); ++i)
        {
            const auto minutes = weeks[i] / 60.0;
            const auto x = (float) plot.getX() + slot * (float) i + 1.0f;   // 2 px gap between bars
            const auto w = juce::jmax (1.0f, slot - 2.0f);

            if (minutes > 0.0)
            {
                const auto h = (float) juce::jmap (minutes, 0.0, top, 0.0, (double) plot.getHeight());
                auto bar = juce::Rectangle<float> (x, (float) plot.getBottom() - h, w, h);
                g.setColour ((int) i == hoverIndex ? seriesColour.brighter (0.25f) : seriesColour);

                // Rounded only at the data end; square on the baseline.
                juce::Path p;
                p.addRoundedRectangle (bar.getX(), bar.getY(), bar.getWidth(), bar.getHeight(),
                                       juce::jmin (4.0f, h, w * 0.5f), juce::jmin (4.0f, h, w * 0.5f), true, true, false, false);
                g.fillPath (p);
            }

            if (i == 0 || i == weeks.size() - 1 || (weeks.size() > 6 && i == weeks.size() / 2))
            {
                g.setColour (axisText);
                g.drawText (weekStart (i).formatted ("%d %b"), juce::roundToInt (x + w * 0.5f) - 40, plot.getBottom() + 4, 80, 16,
                            juce::Justification::centred);
            }
        }

        if (juce::isPositiveAndBelow (hoverIndex, (int) weeks.size()))
        {
            const auto i = (size_t) hoverIndex;
            const auto x = (float) plot.getX() + slot * ((float) i + 0.5f);
            const auto h = (float) juce::jmap (weeks[i] / 60.0, 0.0, top, 0.0, (double) plot.getHeight());
            drawTooltip (g, { x, (float) plot.getBottom() - h }, "Week of " + weekStart (i).formatted ("%d %b") + ": "
                                                                    + formatPracticeDuration (weeks[i]), getLocalBounds());
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto plot = plotArea();
        const auto i = (int) std::floor ((e.position.x - (float) plot.getX()) / ((float) plot.getWidth() / (float) juce::jmax ((size_t) 1, weeks.size())));
        const auto newIndex = juce::isPositiveAndBelow (i, (int) weeks.size()) && plot.toFloat().expanded (0, 20).contains (e.position) ? i : -1;

        if (newIndex != hoverIndex)
        {
            hoverIndex = newIndex;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override     { hoverIndex = -1; repaint(); }

private:
    juce::Rectangle<int> plotArea() const
    {
        return getLocalBounds().withTrimmedTop (34).withTrimmedBottom (22).withTrimmedLeft (44).withTrimmedRight (12);
    }

    juce::Time weekStart (size_t i) const
    {
        const auto back = (int) (weeks.size() - 1 - i) * 7;
        return juce::Time (lastWeekStart.getYear(), lastWeekStart.getMonth(), lastWeekStart.getDayOfMonth() - back, 0, 0, 0, 0, true);
    }

    std::vector<double> weeks;
    juce::Time lastWeekStart;
    int hoverIndex = -1;
};

//==============================================================================
/** Every session of the topic, newest first, with its note. */
class TopicHistory::SessionList final : public juce::Component
{
public:
    explicit SessionList (std::vector<PracticeEntry> e) : entries (std::move (e)) {}

    int contentHeight() const
    {
        int h = 0;

        for (auto& e : entries)
            h += e.note.isNotEmpty() ? 46 : 26;

        return juce::jmax (30, h);
    }

    void paint (juce::Graphics& g) override
    {
        int y = 0;

        for (auto it = entries.rbegin(); it != entries.rend(); ++it)
        {
            auto r = juce::Rectangle<int> (4, y, getWidth() - 8, 26);
            y += 26;

            juce::StringArray detail { formatPracticeDuration (it->playedSeconds) };

            if (it->maxBpm > 0.0)    detail.add (juce::String (juce::roundToInt (it->maxBpm)) + " bpm");
            if (it->maxSpeed > 0.0)  detail.add (juce::String (juce::roundToInt (it->maxSpeed * 100.0)) + " %");
            if (it->looper)          detail.add ("looper");

            g.setFont (juce::FontOptions (14.0f));
            g.setColour (axisText);
            g.drawText (detail.joinIntoString ("  -  "), r, juce::Justification::centredRight, true);
            g.setColour (primaryText);
            g.drawText (it->start.formatted ("%a %d %b %Y, %H:%M"), r, juce::Justification::centredLeft, true);

            if (it->note.isNotEmpty())
            {
                g.setColour (theme::highlight.withAlpha (0.85f));
                g.setFont (juce::FontOptions (13.0f).withStyle ("Italic"));
                g.drawText (it->note.replaceCharacters ("\r\n", "  "), r.translated (16, 20).withHeight (18), juce::Justification::centredLeft, true);
                y += 20;
            }
        }
    }

private:
    std::vector<PracticeEntry> entries;
};

//==============================================================================
TopicHistory::TopicHistory (const PracticeLog& log, const juce::String& topicId)
{
    getProperties().set ("fillOverlay", true);

    std::vector<PracticeEntry> entries;
    std::vector<LineChart::Point> tempo, speed;

    for (auto* e : log.historyFor (topicId))
    {
        entries.push_back (*e);

        if (e->maxBpm > 0.0)
            tempo.push_back ({ e->end, e->maxBpm, e->playedSeconds / 60.0 });

        if (e->maxSpeed > 0.0)
            speed.push_back ({ e->end, e->maxSpeed * 100.0, e->playedSeconds / 60.0 });
    }

    const auto now = juce::Time::getCurrentTime();
    const auto summary = log.summarise (topicId);

    if (! entries.empty())
    {
        juce::StringArray stats;
        stats.add (juce::String (summary.sessions) + (summary.sessions == 1 ? " session" : " sessions"));
        stats.add (formatPracticeDuration (summary.totalSeconds) + " in total");
        stats.add ("since " + entries.front().start.formatted ("%d %b %Y"));
        stats.add ("last practised " + formatPracticeAgo (summary.last, now));
        statsLabel.setText (stats.joinIntoString ("     "), juce::dontSendNotification);
    }

    statsLabel.setFont (juce::FontOptions (16.0f));
    statsLabel.setColour (juce::Label::textColourId, primaryText);
    addAndMakeVisible (statsLabel);

    emptyLabel.setText ("Not practised yet. Press Practise and play: your sessions, tempo and song speed show up here by themselves.",
                        juce::dontSendNotification);
    emptyLabel.setJustificationType (juce::Justification::centred);
    emptyLabel.setColour (juce::Label::textColourId, axisText);
    addChildComponent (emptyLabel);
    emptyLabel.setVisible (entries.empty());

    tempoChart = std::make_unique<LineChart> ("Metronome tempo you played along to (bpm)", " bpm", std::move (tempo));
    speedChart = std::make_unique<LineChart> ("Song speed you played along to (%)", " %", std::move (speed));
    weekBars = std::make_unique<WeekBars> (log.weeklySeconds (topicId, now, 12), PracticeLog::startOfWeek (now));
    addChildComponent (*tempoChart);
    addChildComponent (*speedChart);
    addChildComponent (*weekBars);
    tempoChart->setVisible (tempoChart->hasData());
    speedChart->setVisible (speedChart->hasData());
    weekBars->setVisible (! entries.empty());

    addAndMakeVisible (styleCaption (sessionsHeader, "Sessions"));
    sessionsHeader.setVisible (! entries.empty());
    sessionList = std::make_unique<SessionList> (std::move (entries));
    sessionViewport.setViewedComponent (sessionList.get(), false);
    sessionViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (sessionViewport);
}

TopicHistory::~TopicHistory() = default;

void TopicHistory::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
}

void TopicHistory::resized()
{
    auto area = getLocalBounds().reduced (8);
    statsLabel.setBounds (area.removeFromTop (32));
    area.removeFromTop (8);
    emptyLabel.setBounds (area.withHeight (juce::jmin (area.getHeight(), 120)));

    juce::Rectangle<int> charts, list;

    if (area.getWidth() >= 1000)
    {
        charts = area.removeFromLeft ((int) (area.getWidth() * 0.62f));
        area.removeFromLeft (24);
        list = area;
    }
    else
    {
        list = area.removeFromBottom (juce::jmax (160, area.getHeight() / 3));
        area.removeFromBottom (12);
        charts = area;
    }

    std::vector<juce::Component*> visibleCharts;

    for (juce::Component* c : { (juce::Component*) tempoChart.get(), (juce::Component*) speedChart.get(), (juce::Component*) weekBars.get() })
        if (c->isVisible())
            visibleCharts.push_back (c);

    if (! visibleCharts.empty())
    {
        const auto h = juce::jmin (300, (charts.getHeight() - 16 * ((int) visibleCharts.size() - 1)) / (int) visibleCharts.size());

        for (auto* c : visibleCharts)
        {
            c->setBounds (charts.removeFromTop (h));
            charts.removeFromTop (16);
        }
    }

    sessionsHeader.setBounds (list.removeFromTop (30));
    sessionViewport.setBounds (list);
    sessionList->setSize (juce::jmax (1, sessionViewport.getMaximumVisibleWidth()), sessionList->contentHeight());
}

} // namespace pv
