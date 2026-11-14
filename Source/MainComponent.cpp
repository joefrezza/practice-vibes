#include "MainComponent.h"
#include "LooperPanel.h"
#include "PracticePanel.h"
#include "RigPanel.h"
#include "TunerPanel.h"
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
        g.fillAll (juce::Colours::black.withAlpha (0.88f));
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
    looperPanel = std::make_unique<LooperPanel> (engine);
    rigPanel = std::make_unique<RigPanel> (engine);
    logPanel = std::make_unique<LogPanel> (appLog);

    rigPanel->showOverlay = [this] (const juce::String& title, std::unique_ptr<juce::Component> c) { showOverlay (title, std::move (c)); };
    rigPanel->hideOverlay = [this] { closeOverlay(); };
    looperPanel->showOverlay = [this] (const juce::String& title, std::unique_ptr<juce::Component> c) { showOverlay (title, std::move (c)); };
    looperPanel->hideOverlay = [this] { closeOverlay(); };

    tabs.setTabBarDepth (42);
    tabs.setOutline (0);
    tabs.addTab ("Practice", theme::background, practicePanel.get(), false);
    tabs.addTab ("Looper", theme::background, looperPanel.get(), false);
    tabs.addTab ("Guitar rig", theme::background, rigPanel.get(), false);
    tabs.addTab ("Log", theme::background, logPanel.get(), false);
    addAndMakeVisible (tabs);

    overlay = std::make_unique<Overlay>();
    overlay->onClose = [this] { closeOverlay(); };
    addChildComponent (*overlay);
}

MainComponent::~MainComponent()
{
    engine.onToggleTuner = nullptr;
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
