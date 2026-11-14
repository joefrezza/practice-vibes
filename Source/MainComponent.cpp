#include "MainComponent.h"
#include "LibraryPanel.h"
#include "LooperPanel.h"
#include "PracticePanel.h"
#include "RigPanel.h"
#include "TodayPanel.h"
#include "TunerPanel.h"
#include "YouTubePlayer.h"
#include "Theme.h"
#include "WebView.h"

namespace pv
{

//==============================================================================
/** The "Log" tab: everything the app writes to its log, plus a copy button
    (handy on the iPad, where there's no debugger).
*/
class MainComponent::LogPanel final : public juce::Component,
                                      private juce::ChangeListener,
                                      private juce::Timer
{
public:
    explicit LogPanel (AppLog& l) : appLog (l)
    {
        logView.setMultiLine (true);
        logView.setReadOnly (true);
        logView.setScrollbarsShown (true);
        logView.setCaretVisible (false);
        logView.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        logView.setColour (juce::TextEditor::backgroundColourId, theme::panel);
        addAndMakeVisible (logView);

        pathLabel.setText ("Log file: " + appLog.getFile().getFullPathName(), juce::dontSendNotification);
        pathLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
        pathLabel.setMinimumHorizontalScale (0.5f);
        addAndMakeVisible (pathLabel);

        copyButton.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (appLog.getText()); };
        addAndMakeVisible (copyButton);

        appLog.addChangeListener (this);
        startTimerHz (10);
    }

    ~LogPanel() override
    {
        appLog.removeChangeListener (this);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::background); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 8);
        auto bottom = area.removeFromBottom (44);
        copyButton.setBounds (bottom.removeFromRight (160).reduced (3));
        pathLabel.setBounds (bottom);
        logView.setBounds (area);
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { dirty = true; }

    void timerCallback() override
    {
        if (dirty && isShowing())
        {
            dirty = false;
            logView.setText (appLog.getText(), false);
            logView.moveCaretToEnd();
        }
    }

    AppLog& appLog;
    juce::TextEditor logView;
    juce::Label pathLabel;
    juce::TextButton copyButton { "Copy log" };
    bool dirty = true;
};

//==============================================================================
/** Full-window panel used for plugin UIs and the audio settings. */
class MainComponent::Overlay final : public juce::Component,
                                     private juce::ComponentListener
{
public:
    Overlay()
    {
        title.setFont (juce::FontOptions (20.0f).withStyle ("Bold"));
        title.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (title);
        addAndMakeVisible (closeButton);
        closeButton.onClick = [this] { if (onClose) onClose(); };
    }

    ~Overlay() override { hide(); }

    void show (const juce::String& titleText, std::unique_ptr<juce::Component> newContent)
    {
        hide();
        content = std::move (newContent);
        title.setText (titleText, juce::dontSendNotification);
        addAndMakeVisible (*content);
        content->addComponentListener (this);
        setVisible (true);
        toFront (true);
        resized();
    }

    void hide()
    {
        if (content != nullptr)
            content->removeComponentListener (this);

        content.reset();
        setVisible (false);
    }

    std::function<void()> onClose;

    juce::Component* getContent() const noexcept    { return content.get(); }

    void paint (juce::Graphics& g) override
    {
        const auto opaque = content != nullptr && (bool) content->getProperties()["fillOverlay"];
        g.fillAll (juce::Colours::black.withAlpha (opaque ? 1.0f : 0.88f));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        auto header = area.removeFromTop (44);
        closeButton.setBounds (header.removeFromRight (110).reduced (2));
        title.setBounds (header);
        area.removeFromTop (8);
        contentArea = area;
        layoutContent();
    }

private:
    void layoutContent()
    {
        if (content == nullptr || laying)
            return;

        const juce::ScopedValueSetter<bool> svs (laying, true);

        if (auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (content.get()))
        {
            if (editor->isResizable())
                editor->setBounds (contentArea);
            else
                editor->setCentrePosition (contentArea.getCentre());
        }
        else if (content->getProperties()["fillOverlay"])
        {
            content->setBounds (contentArea);   // document / web viewer
        }
        else
        {
            content->setBounds (contentArea.withSizeKeepingCentre (juce::jmin (720, contentArea.getWidth()),
                                                                   juce::jmin (640, contentArea.getHeight())));
        }
    }

    void componentMovedOrResized (juce::Component&, bool, bool wasResized) override
    {
        if (wasResized)
            layoutContent();   // a plugin UI resized itself: re-centre it
    }

    juce::Label title;
    juce::TextButton closeButton { "Close" };
    std::unique_ptr<juce::Component> content;
    juce::Rectangle<int> contentArea;
    bool laying = false;
};

//==============================================================================
MainComponent::MainComponent (AppEngine& e, AppLog& appLog)
    : engine (e)
{
    titleLabel.setText (JUCE_APPLICATION_NAME_STRING, juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (22.0f).withStyle ("Bold"));
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (titleLabel);

    for (auto* b : { &tunerButton, &audioSettingsButton, &bluetoothButton, &saveButton, &practisingButton })
        addAndMakeVisible (*b);

    practisingButton.setTooltip ("What you're practising: played time is logged to this topic (see the Today tab)");
    practisingButton.onClick = [this] { showPractisingMenu(); };

    audioSettingsButton.onClick = [this] { openAudioSettings(); };
    tunerButton.onClick = [this] { toggleTuner(); };
    tunerButton.setTooltip ("Chromatic tuner (a footswitch can open and close it too)");
    engine.onToggleTuner = [this] { toggleTuner(); };
    engine.onSaveState = [this]
    {
        if (libraryPanel != nullptr)
            libraryPanel->flush();

        tracker.flush();
    };

    bluetoothButton.onClick = []
    {
        if (juce::BluetoothMidiDevicePairingDialogue::isAvailable())
            juce::BluetoothMidiDevicePairingDialogue::open();
        else
            juce::Logger::writeToLog ("Bluetooth MIDI pairing is done by the operating system here: "
                                      "pair the footswitch in Windows Settings > Bluetooth & devices.");
    };

    saveButton.onClick = [this] { engine.saveState(); };

    todayPanel = std::make_unique<TodayPanel> (tracker);
    practicePanel = std::make_unique<PracticePanel> (*engine.player, *engine.metronome);
    libraryPanel = std::make_unique<LibraryPanel>();
    looperPanel = std::make_unique<LooperPanel> (engine);
    rigPanel = std::make_unique<RigPanel> (engine);
    logPanel = std::make_unique<LogPanel> (appLog);

    rigPanel->showOverlay = [this] (const juce::String& title, std::unique_ptr<juce::Component> c) { showOverlay (title, std::move (c)); };
    rigPanel->hideOverlay = [this] { closeOverlay(); };
    looperPanel->showOverlay = [this] (const juce::String& title, std::unique_ptr<juce::Component> c) { showOverlay (title, std::move (c)); };
    looperPanel->hideOverlay = [this] { closeOverlay(); };
    libraryPanel->showOverlay = [this] (const juce::String& title, std::unique_ptr<juce::Component> c) { showOverlay (title, std::move (c)); };
    libraryPanel->hideOverlay = [this] { closeOverlay(); };
    libraryPanel->onOpenAudio = [this] (const juce::File& file)
    {
        practicePanel->loadSong (juce::URL (file));
        showTab (practicePanel.get());
    };
    libraryPanel->onOpenYouTube = [this] (const YouTubeLink& link, const juce::String& title) { openYouTube (link, title); };

    // Practice log: opening something from a topic makes it the topic you're practising.
    libraryPanel->onLibraryOpened = [this] { libraryOpened(); };
    libraryPanel->onTopicsChanged = [this] { todayPanel->refresh(); updatePractisingButton(); };
    libraryPanel->onPractise = [this] (const juce::String& id) { practiseTopic (id); };
    libraryPanel->onTopicOpened = [this] (const juce::String& id)
    {
        if (auto* lib = libraryPanel->getLibrary())
            if (auto* t = lib->findTopic (id))
                tracker.setActiveTopic (t->id, t->title);

        updatePractisingButton();
    };

    todayPanel->getLibrary = [this] { return libraryPanel->getLibrary(); };
    todayPanel->onPractise = [this] (const juce::String& id) { practiseTopic (id); };
    todayPanel->onShowTopic = [this] (const juce::String& id)
    {
        libraryPanel->selectTopic (id);
        showTab (libraryPanel.get());
    };
    todayPanel->onBecameVisible = [this]
    {
        libraryPanel->checkForChanges();

        if (practiceLog != nullptr)
            practiceLog->reload();
    };

    tracker.captureSetup = [this]
    {
        auto s = practicePanel->captureSetup();

        if (s.songPath.isNotEmpty())
            s.songPath = PracticeSetup::portablePath (juce::File (s.songPath),
                                                      libraryPanel->getLibrary() != nullptr ? libraryPanel->getLibrary()->getRoot() : juce::File());
        return s;
    };
    tracker.onSaved = [this] { if (todayPanel->isShowing()) todayPanel->refresh(); };

    tabs.setTabBarDepth (42);
    tabs.setOutline (0);
    tabs.addTab ("Today", theme::background, todayPanel.get(), false);
    tabs.addTab ("Practice", theme::background, practicePanel.get(), false);
    tabs.addTab ("Topics", theme::background, libraryPanel.get(), false);
    tabs.addTab ("Looper", theme::background, looperPanel.get(), false);
    tabs.addTab ("Guitar rig", theme::background, rigPanel.get(), false);
    tabs.addTab ("Log", theme::background, logPanel.get(), false);
    addAndMakeVisible (tabs);

    youtube = std::make_unique<YouTubePlayer>();
    addChildComponent (*youtube);
    practicePanel->setYouTubePlayer (youtube.get());
    practicePanel->onCompactRequested = [this]
    {
        youTubeAutoCompact = false;
        youtube->setCompact (true);
        youtube->setBounds (lastCompactBounds.isEmpty() ? defaultCompactBounds() : lastCompactBounds);
        layoutYouTube();
    };
    youtube->onExpand = [this]
    {
        lastCompactBounds = youtube->getBounds();
        youtube->setCompact (false);
        youTubeAutoCompact = false;
        closeOverlay();
        showTab (practicePanel.get());
        layoutYouTube();
    };
    youtube->onClose = [this]
    {
        practicePanel->closeYouTube();
        layoutYouTube();
    };

    overlay = std::make_unique<Overlay>();
    overlay->onClose = [this] { closeOverlay(); };
    addChildComponent (*overlay);

    libraryOpened();   // the Topics tab opened the saved library folder before the callbacks were set
    tabs.setCurrentTabIndex (0);
    startTimerHz (10);
}

MainComponent::~MainComponent()
{
    stopTimer();
    tracker.finish();
    tracker.onSaved = nullptr;
    tracker.captureSetup = nullptr;
    libraryPanel->onLibraryOpened = nullptr;
    libraryPanel->onTopicsChanged = nullptr;
    libraryPanel->onTopicOpened = nullptr;
    engine.onToggleTuner = nullptr;
    engine.onSaveState = nullptr;
    closeOverlay();
    tabs.clearTabs();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

   #if JUCE_IOS
    if (auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds()))
        area = display->safeAreaInsets.subtractedFrom (area);
   #endif

    overlay->setBounds (area);

    auto top = area.removeFromTop (52).reduced (12, 4);
    saveButton.setBounds (top.removeFromRight (100).reduced (4));
    bluetoothButton.setBounds (top.removeFromRight (150).reduced (4));
    audioSettingsButton.setBounds (top.removeFromRight (150).reduced (4));
    tunerButton.setBounds (top.removeFromRight (90).reduced (4));
    practisingButton.setBounds (top.removeFromRight (juce::jlimit (150, 320, top.getWidth() - 180)).reduced (4));
    titleLabel.setBounds (top);

    tabs.setBounds (area);
    layoutYouTube();
}

//==============================================================================
void MainComponent::timerCallback()
{
    layoutYouTube();
    tickTracker();
}

void MainComponent::showTab (juce::Component* content)
{
    for (int i = 0; i < tabs.getNumTabs(); ++i)
        if (tabs.getTabContentComponent (i) == content)
            tabs.setCurrentTabIndex (i);
}

bool MainComponent::isTabShowing (juce::Component* content) const
{
    return tabs.getCurrentContentComponent() == content;
}

void MainComponent::openYouTube (const YouTubeLink& link, const juce::String& title)
{
    youtube->load (link, title);
    youtube->setCompact (false);
    youTubeAutoCompact = false;
    practicePanel->showYouTube();
    showTab (practicePanel.get());
    layoutYouTube();
}

//==============================================================================
void MainComponent::libraryOpened()
{
    tracker.setLog (nullptr);
    practiceLog.reset();
    tracker.setActiveTopic ({}, {});   // topic ids belong to the old library

    if (auto* lib = libraryPanel->getLibrary())
    {
        practiceLog = std::make_unique<PracticeLog> (lib->getRoot().getChildFile (".practice"), PracticeLog::getThisDeviceId());
        tracker.setLog (practiceLog.get());
    }

    todayPanel->refresh();
    updatePractisingButton();
}

void MainComponent::practiseTopic (const juce::String& topicId)
{
    auto* lib = libraryPanel->getLibrary();
    auto* topic = lib != nullptr ? lib->findTopic (topicId) : nullptr;

    if (topic == nullptr)
        return;

    tracker.setActiveTopic (topic->id, topic->title);
    updatePractisingButton();
    closeOverlay();

    // Bring back what was on screen the last time (on any device).
    if (const auto* last = practiceLog != nullptr ? practiceLog->latestSetupFor (topicId) : nullptr)
    {
        const auto setup = last->setup;

        if (setup.youTubeId.isNotEmpty() && webViewAvailable())
        {
            YouTubeLink link;
            link.videoId = setup.youTubeId;
            link.startSeconds = setup.loopOn && setup.loopEnd > setup.loopStart ? setup.loopStart : 0.0;
            openYouTube (link, setup.youTubeTitle);
            practicePanel->applySetup (setup);
            return;
        }

        if (setup.songPath.isNotEmpty())
        {
            const auto file = PracticeSetup::resolvePath (setup.songPath, lib->getRoot());

            if (file.existsAsFile())
            {
                lastSongFile = file;   // restoring: don't switch topic because of where the song lives

                if (practicePanel->getSongFile() == file && ! practicePanel->isYouTubeMode())
                    practicePanel->applySetup (setup);
                else
                    practicePanel->loadSong (juce::URL (file), setup);

                showTab (practicePanel.get());
                return;
            }
        }

        if (setup.metronome)
        {
            practicePanel->applySetup (setup);
            showTab (practicePanel.get());
            return;
        }
    }

    // First time: open its first song or video, or else show the topic itself.
    if (! libraryPanel->openFirstMedia (topicId))
    {
        libraryPanel->selectTopic (topicId);
        showTab (libraryPanel.get());
    }
}

void MainComponent::tickTracker()
{
    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    const auto seconds = lastTrackerTick > 0.0 ? (nowMs - lastTrackerTick) / 1000.0 : 0.0;
    lastTrackerTick = nowMs;

    // A song from a topic's folder (opened from anywhere) makes that topic the one you're practising.
    if (const auto songFile = practicePanel->getSongFile(); songFile != lastSongFile)
    {
        lastSongFile = songFile;

        if (auto* lib = libraryPanel->getLibrary(); lib != nullptr && songFile != juce::File())
            if (auto* topic = lib->findTopicForFile (songFile))
                tracker.setActiveTopic (topic->id, topic->title);
    }

    PracticeTracker::Activity activity;
    activity.songPlaying = practicePanel->isMediaPlaying();
    activity.speed = practicePanel->getMediaSpeed();
    activity.metronomeRunning = engine.metronome->isRunning();
    activity.bpm = engine.metronome->getCurrentBpm();
    activity.looperActive = ! engine.looper->isEmpty();
    activity.songName = practicePanel->getMediaName();

    tracker.tick (seconds, engine.chain->getInputStage().takeActivityPeak(), activity, juce::Time::getCurrentTime());

    if (--practisingButtonCountdown <= 0)
        updatePractisingButton();
}

void MainComponent::updatePractisingButton()
{
    practisingButtonCountdown = 10;   // once a second
    const auto active = tracker.getActiveTopicId().isNotEmpty();
    auto text = active ? tracker.getActiveTopicTitle() : juce::String ("Free practice");

    if (active && tracker.getActiveSeconds() >= 30.0)
        text << "  -  " << formatPracticeDuration (tracker.getActiveSeconds());

    practisingButton.setButtonText ((tracker.isPlaying() ? juce::String ("(playing) ") : juce::String()) + text);
    practisingButton.setColour (juce::TextButton::buttonColourId,
                                active ? theme::accent : getLookAndFeel().findColour (juce::TextButton::buttonColourId));
}

void MainComponent::showPractisingMenu()
{
    juce::PopupMenu menu;
    menu.addItem ("Open the Today tab", [this] { showTab (todayPanel.get()); });

    if (tracker.getActiveTopicId().isNotEmpty())
        menu.addItem ("Stop practising \"" + tracker.getActiveTopicTitle() + "\"", [this]
        {
            tracker.setActiveTopic ({}, {});
            updatePractisingButton();
            todayPanel->refresh();
        });

    if (auto* lib = libraryPanel->getLibrary())
    {
        juce::PopupMenu topics;

        for (auto& t : lib->getTopics())
            if (t.inRotation)
                topics.addItem (t.title, true, t.id == tracker.getActiveTopicId(), [this, id = t.id] { practiseTopic (id); });

        menu.addSeparator();

        if (topics.getNumItems() > 0)
            menu.addSubMenu ("Practise a rotation topic", topics);
        else
            menu.addItem ("No topics in the practice rotation yet", false, false, [] {});
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&practisingButton));
}

juce::Rectangle<int> MainComponent::defaultCompactBounds() const
{
    const auto w = YouTubePlayer::compactVideoWidth + 2;
    const auto h = YouTubePlayer::compactVideoHeight + YouTubePlayer::compactBarHeight + 1;
    return { getWidth() - w - 16, getHeight() - h - 16, w, h };
}

void MainComponent::layoutYouTube()
{
    if (youtube == nullptr || overlay == nullptr || practicePanel == nullptr)
        return;

    if (! youtube->hasVideo() || ! practicePanel->isYouTubeMode())
    {
        youtube->setVisible (false);
        return;
    }

    const auto practiceShowing = isTabShowing (practicePanel.get()) && ! overlay->isVisible();
    auto compact = youtube->isCompact();

    // Leaving the Practice tab while the video plays: keep it going in the floating player.
    if (! compact && ! practiceShowing && youtube->getState().playing)
    {
        compact = true;
        youTubeAutoCompact = true;
    }

    // ...and dock it again when coming back, unless you asked for the compact player yourself.
    if (compact && youTubeAutoCompact && practiceShowing)
    {
        compact = false;
        youTubeAutoCompact = false;
    }

    if (compact != youtube->isCompact())
    {
        if (! compact)
            lastCompactBounds = youtube->getBounds();

        youtube->setCompact (compact);

        if (compact)
            youtube->setBounds (lastCompactBounds.isEmpty() ? defaultCompactBounds() : lastCompactBounds);
    }

    if (compact)
    {
        // Keep it inside the window (after a resize, for instance).
        auto b = youtube->getBounds();
        b = b.constrainedWithin (getLocalBounds());

        if (b != youtube->getBounds())
            youtube->setBounds (b);

        if (! youtube->isVisible())
        {
            youtube->setVisible (true);
            youtube->toFront (false);
        }
    }
    else if (practiceShowing)
    {
        const auto area = getLocalArea (practicePanel.get(), practicePanel->getVideoArea());

        if (youtube->getBounds() != area)
            youtube->setBounds (area);

        youtube->setVisible (! area.isEmpty());
    }
    else
    {
        youtube->setVisible (false);   // paused on another tab or under an overlay
    }
}

//==============================================================================
void MainComponent::openAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (engine.deviceManager,
                                                                          0, 8,    // inputs
                                                                          0, 8,    // outputs
                                                                          true,    // MIDI inputs
                                                                          false, false, false);
    showOverlay ("Audio settings", std::move (selector));
}

void MainComponent::toggleTuner()
{
    if (overlay->isVisible() && dynamic_cast<TunerPanel*> (overlay->getContent()) != nullptr)
        closeOverlay();
    else
        showOverlay ("Tuner", std::make_unique<TunerPanel> (engine));
}

void MainComponent::showOverlay (const juce::String& title, std::unique_ptr<juce::Component> content)
{
    overlay->show (title, std::move (content));
}

void MainComponent::closeOverlay()
{
    if (overlay != nullptr)
        overlay->hide();
}

} // namespace pv
