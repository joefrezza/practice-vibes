#include "FootswitchPanel.h"
#include "Theme.h"

namespace pv
{

//==============================================================================
/** One switch: activity lamp, learned MIDI message, Learn / Forget, tap and hold actions. */
class FootswitchPanel::SwitchRow final : public juce::Component
{
public:
    SwitchRow (Footswitch& f, int i) : footswitch (f), index (i)
    {
        nameLabel.setText (Footswitch::switchName (index), juce::dontSendNotification);
        nameLabel.setFont (juce::FontOptions (26.0f).withStyle ("Bold"));
        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.8f));
        addAndMakeVisible (nameLabel);

        triggerLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.85f));
        triggerLabel.setMinimumHorizontalScale (0.6f);
        addAndMakeVisible (triggerLabel);

        learnButton.onClick = [this]
        {
            if (onLearn)
                onLearn();
        };
        forgetButton.onClick = [this] { footswitch.forget (index); };
        addAndMakeVisible (learnButton);
        addAndMakeVisible (forgetButton);

        for (auto* label : { &tapLabel, &holdLabel })
        {
            label->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
            addAndMakeVisible (*label);
        }

        tapLabel.setText ("Tap", juce::dontSendNotification);
        holdLabel.setText ("Hold 1 s", juce::dontSendNotification);

        for (size_t a = 0; a < actions.size(); ++a)
        {
            tapBox.addItem (actions[a].describe(), (int) a + 1);
            holdBox.addItem (actions[a].describe(), (int) a + 1);
        }

        tapBox.onChange  = [this] { footswitch.setTapAction (index, actionFor (tapBox)); };
        holdBox.onChange = [this] { footswitch.setHoldAction (index, actionFor (holdBox)); };
        addAndMakeVisible (tapBox);
        addAndMakeVisible (holdBox);
    }

    std::function<void()> onLearn;

    void update()
    {
        const auto& sw = footswitch.getSwitch (index);
        const auto learning = footswitch.getLearningIndex() == index;
        const auto pending = footswitch.getPendingIndex() == index;

        juce::String text;

        if (learning)
            text = "Press switch " + Footswitch::switchName (index) + " on the pedal now...";
        else if (! sw.trigger.isValid())
            text = "Not learned";
        else
            text = sw.trigger.describe() + (pending ? "  - checking switch type..."
                                           : sw.momentary ? "  - momentary (tap + hold)"
                                                          : "  - latching (tap only)");

        triggerLabel.setText (text, juce::dontSendNotification);
        learnButton.setButtonText (learning ? "Cancel" : "Learn");
        forgetButton.setEnabled (sw.trigger.isValid());

        selectAction (tapBox, sw.tap);
        selectAction (holdBox, sw.hold);

        const auto holdPossible = sw.momentary || ! sw.trigger.isValid();
        holdBox.setEnabled (holdPossible);
        holdLabel.setEnabled (holdPossible);
        holdBox.setTooltip (holdPossible ? juce::String() : "This switch sends one message per stomp, so the app cannot tell a hold. "
                                                            "Set it to momentary on the pedal and learn it again.");

        const auto lamp = juce::jlimit (0.0f, 1.0f, 1.0f - (float) footswitch.secondsSinceActivity (index) / 0.4f);
        const auto down = footswitch.isDown (index);

        if (std::abs (lamp - lampLevel) > 0.02f || down != lampDown || learning != lastLearning)
        {
            lampLevel = lamp;
            lampDown = down;
            lastLearning = learning;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (lastLearning ? theme::accent.withAlpha (0.45f) : theme::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f);

        const auto level = juce::jmax (lampLevel, lampDown ? 0.7f : 0.0f);
        g.setColour (juce::Colours::white.withAlpha (0.12f).interpolatedWith (theme::highlight, level));
        g.fillEllipse (lampArea.toFloat());
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8, 6);
        auto top = area.removeFromTop (area.getHeight() / 2);

        lampArea = top.removeFromLeft (22).withSizeKeepingCentre (16, 16);
        nameLabel.setBounds (top.removeFromLeft (36));
        forgetButton.setBounds (top.removeFromRight (80).reduced (3));
        learnButton.setBounds (top.removeFromRight (90).reduced (3));
        triggerLabel.setBounds (top);

        area.removeFromLeft (58);
        auto left = area.removeFromLeft (area.getWidth() / 2);
        tapLabel.setBounds (left.removeFromLeft (40));
        tapBox.setBounds (left.reduced (3, 4));
        holdLabel.setBounds (area.removeFromLeft (70).withTrimmedLeft (8));
        holdBox.setBounds (area.reduced (3, 4));
    }

private:
    FootswitchAction actionFor (const juce::ComboBox& box) const
    {
        const auto i = box.getSelectedId() - 1;
        return juce::isPositiveAndBelow (i, (int) actions.size()) ? actions[(size_t) i] : FootswitchAction {};
    }

    void selectAction (juce::ComboBox& box, const FootswitchAction& action)
    {
        if (box.isPopupActive())
            return;

        for (size_t a = 0; a < actions.size(); ++a)
            if (actions[a] == action)
            {
                if (box.getSelectedId() != (int) a + 1)
                    box.setSelectedId ((int) a + 1, juce::dontSendNotification);

                return;
            }
    }

    Footswitch& footswitch;
    const int index;
    const std::vector<FootswitchAction> actions = FootswitchAction::all (Looper::numTracks);

    juce::Label nameLabel, triggerLabel, tapLabel, holdLabel;
    juce::TextButton learnButton { "Learn" }, forgetButton { "Forget" };
    juce::ComboBox tapBox, holdBox;
    juce::Rectangle<int> lampArea;
    float lampLevel = 0.0f;
    bool lampDown = false, lastLearning = false;
};

//==============================================================================
FootswitchPanel::FootswitchPanel (AppEngine& e)
    : engine (e), footswitch (*e.footswitch)
{
    intro.setText ("Each switch has a tap action (on the press, so recording starts exactly on the stomp) and, "
                   "for momentary switches, a hold action after 1 second.\n"
                   "M-VAVE Chocolate: in CubeSuite choose Custom Control, give each switch its own CC number "
                   "and set Visual behavior Off (momentary). Then tap Learn and press the switch.",
                   juce::dontSendNotification);
    intro.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.8f));
    intro.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (intro);

    devicesLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
    devicesLabel.setMinimumHorizontalScale (0.6f);
    addAndMakeVisible (devicesLabel);

    learnAllButton.onClick = [this]
    {
        if (learningAll)
        {
            learningAll = false;
            footswitch.cancelLearning();
        }
        else
        {
            learningAll = true;
            learnAllIndex = 0;
            footswitch.startLearning (0);
        }
    };

    defaultsButton.setTooltip ("Switch A-D = track 1-4: tap records / overdubs / plays, hold clears the track. Learned switches are kept.");
    defaultsButton.onClick = [this] { footswitch.setDefaultActions(); };
    addAndMakeVisible (learnAllButton);
    addAndMakeVisible (defaultsButton);

    for (int i = 0; i < Footswitch::numSwitches; ++i)
    {
        auto row = std::make_unique<SwitchRow> (footswitch, i);
        row->onLearn = [this, i]
        {
            learningAll = false;

            if (footswitch.getLearningIndex() == i)
                footswitch.cancelLearning();
            else
                footswitch.startLearning (i);
        };
        addAndMakeVisible (*row);
        rows[(size_t) i] = std::move (row);
    }

    timerCallback();
    startTimerHz (30);
}

FootswitchPanel::~FootswitchPanel()
{
    stopTimer();
    footswitch.cancelLearning();
}

void FootswitchPanel::timerCallback()
{
    // "Learn A-D": move on once a switch is learned and its type is known.
    if (learningAll && footswitch.getLearningIndex() < 0 && footswitch.getPendingIndex() < 0)
    {
        if (++learnAllIndex < Footswitch::numSwitches)
            footswitch.startLearning (learnAllIndex);
        else
            learningAll = false;
    }

    learnAllButton.setButtonText (learningAll ? "Stop learning" : "Learn A-D in a row");

    const auto names = engine.midi->getEnabledInputNames();
    devicesLabel.setText (names.isEmpty() ? juce::String ("No MIDI inputs: connect or pair the pedal (Bluetooth MIDI button on the top bar)")
                                          : "MIDI inputs: " + names.joinIntoString (", "),
                          juce::dontSendNotification);

    for (auto& row : rows)
        row->update();
}

void FootswitchPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
}

void FootswitchPanel::resized()
{
    auto area = getLocalBounds().reduced (12);

    intro.setBounds (area.removeFromTop (92));
    devicesLabel.setBounds (area.removeFromTop (26));
    area.removeFromTop (4);

    auto buttons = area.removeFromTop (40);
    learnAllButton.setBounds (buttons.removeFromLeft (200).reduced (0, 3));
    buttons.removeFromLeft (8);
    defaultsButton.setBounds (buttons.removeFromLeft (160).reduced (0, 3));
    area.removeFromTop (8);

    const auto rowHeight = juce::jlimit (70, 104, area.getHeight() / Footswitch::numSwitches);

    for (auto& row : rows)
        row->setBounds (area.removeFromTop (rowHeight).reduced (0, 3));
}

} // namespace pv
