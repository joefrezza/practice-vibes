#include "MainComponent.h"
#include "LibraryPanel.h"
#include "LooperPanel.h"
#include "PracticePanel.h"
#include "RigPanel.h"
#include "TunerPanel.h"
#include "YouTubePlayer.h"
#include "Theme.h"

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

    for (auto* b : { &tunerButton, &audioSettingsButton, &bluetoothButton, &saveButton })
        addAndMakeVisible (*b);

    audioSettingsButton.onClick = [this] { openAudioSettings(); };
    tunerButton.onClick = [this] { toggleTuner(); };
    tunerButton.setTooltip ("Chromatic tuner (a footswitch can open and close it too)");
    engine.onToggleTuner = [this] { toggleTuner(); };
    engine.onSaveState = [this] { if (libraryPanel != nullptr) libraryPanel->flush(); };

    bluetoothButton.onClick = []
    {
        if (juce::BluetoothMidiDevicePairingDialogue::isAvailable())
            juce::BluetoothMidiDevicePairingDialogue::open();
        else
            juce::Logger::writeToLog ("Bluetooth MIDI pairing is done by the operating system here: "
                                      "pair the footswitch in Windows Settings > Bluetooth & devices.");
    };

    saveButton.onClick = [this] { engine.saveState(); };

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
        tabs.setCurrentTabIndex (0);
    };
    libraryPanel->onOpenYouTube = [this] (const YouTubeLink& link, const juce::String& title)
    {
        youtube->load (link, title);
        youtube->setCompact (false);
        youTubeAutoCompact = false;
        practicePanel->showYouTube();
        tabs.setCurrentTabIndex (0);
        layoutYouTube();
    };

    tabs.setTabBarDepth (42);
    tabs.setOutline (0);
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
        tabs.setCurrentTabIndex (0);
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

    startTimerHz (10);
}

MainComponent::~MainComponent()
{
    stopTimer();
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
    titleLabel.setBounds (top);

    tabs.setBounds (area);
    layoutYouTube();
}

//==============================================================================
void MainComponent::timerCallback()
{
    layoutYouTube();
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

    const auto practiceShowing = tabs.getCurrentTabIndex() == 0 && ! overlay->isVisible();
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
