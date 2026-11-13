#include "MainComponent.h"

namespace pv
{

namespace
{
    const auto backgroundColour = juce::Colour (0xff1e1f24);
    const auto panelColour      = juce::Colour (0xff2a2c33);
    const auto accentColour     = juce::Colour (0xff3a6ea5);
}

//==============================================================================
class MainComponent::ListModel final : public juce::ListBoxModel
{
public:
    std::function<int()> rowCount;
    std::function<juce::String (int)> rowText;
    std::function<void (int)> onDoubleClick;
    std::function<void (int)> onSelect;

    int getNumRows() override { return rowCount ? rowCount() : 0; }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (selected)
            g.fillAll (accentColour);
        else if (row % 2 == 1)
            g.fillAll (juce::Colours::white.withAlpha (0.03f));

        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (16.0f));
        g.drawText (rowText ? rowText (row) : juce::String(), 10, 0, width - 20, height,
                    juce::Justification::centredLeft, true);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (onDoubleClick)
            onDoubleClick (row);
    }

    void selectedRowsChanged (int lastRowSelected) override
    {
        if (onSelect)
            onSelect (lastRowSelected);
    }
};

//==============================================================================
class MainComponent::LevelMeter final : public juce::Component
{
public:
    void push (float peak)
    {
        level = juce::jmax (peak, level * 0.85f);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRoundedRectangle (bounds, 3.0f);

        const auto db = juce::Decibels::gainToDecibels (level, -60.0f);
        const auto proportion = juce::jlimit (0.0f, 1.0f, juce::jmap (db, -60.0f, 0.0f, 0.0f, 1.0f));

        g.setColour (db > -3.0f ? juce::Colours::red : db > -12.0f ? juce::Colours::orange : juce::Colours::limegreen);
        g.fillRoundedRectangle (bounds.withWidth (bounds.getWidth() * proportion), 3.0f);

        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (level > 0.001f ? juce::String (db, 1) + " dB" : juce::String ("-inf"),
                    getLocalBounds().reduced (6, 0), juce::Justification::centredRight);
    }

private:
    float level = 0.0f;
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
MainComponent::MidiTrigger MainComponent::MidiTrigger::fromMessage (const juce::MidiMessage& m)
{
    MidiTrigger t;

    if (m.isController() && m.getControllerValue() >= 64)
        t = { Type::controller, m.getChannel(), m.getControllerNumber() };
    else if (m.isNoteOn())
        t = { Type::note, m.getChannel(), m.getNoteNumber() };
    else if (m.isProgramChange())
        t = { Type::program, m.getChannel(), m.getProgramChangeNumber() };

    return t;
}

bool MainComponent::MidiTrigger::matches (const juce::MidiMessage& m) const
{
    const auto other = fromMessage (m);
    return type != Type::none && other.type == type && other.channel == channel && other.number == number;
}

juce::String MainComponent::MidiTrigger::describe() const
{
    const auto ch = " (ch " + juce::String (channel) + ")";

    switch (type)
    {
        case Type::controller: return "CC " + juce::String (number) + ch;
        case Type::note:       return "Note " + juce::MidiMessage::getMidiNoteName (number, true, true, 3) + ch;
        case Type::program:    return "Program " + juce::String (number) + ch;
        case Type::none:       break;
    }

    return "none";
}

//==============================================================================
MainComponent::MainComponent (AppEngine& e, AppLog& l)
    : engine (e), appLog (l)
{
    auto setupHeader = [this] (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::FontOptions (17.0f).withStyle ("Bold"));
        label.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.9f));
        addAndMakeVisible (label);
    };

    setupHeader (titleLabel, juce::String (JUCE_APPLICATION_NAME_STRING) + "  -  feasibility test v" + JUCE_APPLICATION_VERSION_STRING);
    titleLabel.setFont (juce::FontOptions (22.0f).withStyle ("Bold"));
    setupHeader (catalogueHeader, "Available plugins");
    setupHeader (chainHeader, "Guitar chain");
    setupHeader (levelsHeader, "Input / output");
    setupHeader (midiHeader, "MIDI footswitch");
    setupHeader (logHeader, "Log");

    for (auto* b : { &audioSettingsButton, &bluetoothButton, &saveButton, &addButton, &scanButton,
                     &editButton, &bypassButton, &upButton, &downButton, &removeButton, &learnButton })
        addAndMakeVisible (*b);

    audioSettingsButton.onClick = [this] { openAudioSettings(); };

    bluetoothButton.onClick = [this]
    {
        if (juce::BluetoothMidiDevicePairingDialogue::isAvailable())
            juce::BluetoothMidiDevicePairingDialogue::open();
        else
            juce::Logger::writeToLog ("Bluetooth MIDI pairing is done by the operating system here: "
                                      "pair the footswitch in Windows Settings > Bluetooth & devices.");
    };

    saveButton.onClick = [this] { engine.saveState(); };

    // Catalogue ------------------------------------------------------------
    catalogueModel = std::make_unique<ListModel>();
    catalogueModel->rowCount = [this] { return (int) catalogueEntries.size(); };
    catalogueModel->rowText = [this] (int row) -> juce::String
    {
        if (! juce::isPositiveAndBelow (row, (int) catalogueEntries.size()))
            return {};

        const auto& entry = catalogueEntries[(size_t) row];
        return "[" + entry.kind + "]  " + entry.description.name;
    };
    catalogueModel->onDoubleClick = [this] (int) { addSelectedPlugin(); };
    catalogueList.setModel (catalogueModel.get());
    catalogueList.setRowHeight (40);
    catalogueList.setColour (juce::ListBox::backgroundColourId, panelColour);
    addAndMakeVisible (catalogueList);

    catalogueStatus.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
    addAndMakeVisible (catalogueStatus);

    addButton.onClick = [this] { addSelectedPlugin(); };
    scanButton.onClick = [this] { engine.catalogue->startScan(); };
    scanButton.setVisible (engine.catalogue->canScan());

    // Chain ----------------------------------------------------------------
    chainModel = std::make_unique<ListModel>();
    chainModel->rowCount = [this] { return engine.chain->getNumPlugins(); };
    chainModel->rowText = [this] (int row)
    {
        return juce::String (row + 1) + ".  " + engine.chain->getPluginName (row)
             + (engine.chain->isBypassed (row) ? "   (bypassed)" : "");
    };
    chainModel->onDoubleClick = [this] (int row) { openEditor (row); };
    chainList.setModel (chainModel.get());
    chainList.setRowHeight (44);
    chainList.setColour (juce::ListBox::backgroundColourId, panelColour);
    addAndMakeVisible (chainList);

    editButton.onClick = [this] { openEditor (selectedChainIndex()); };

    bypassButton.onClick = [this]
    {
        const auto i = selectedChainIndex();
        engine.chain->setBypassed (i, ! engine.chain->isBypassed (i));
    };

    upButton.onClick = [this]
    {
        const auto i = selectedChainIndex();
        engine.chain->movePlugin (i, -1);
        chainList.selectRow (juce::jmax (0, i - 1));
    };

    downButton.onClick = [this]
    {
        const auto i = selectedChainIndex();
        engine.chain->movePlugin (i, 1);
        chainList.selectRow (juce::jmin (engine.chain->getNumPlugins() - 1, i + 1));
    };

    removeButton.onClick = [this]
    {
        closeOverlay();
        engine.chain->removePlugin (selectedChainIndex());
    };

    engine.chain->onChange = [this] { refreshChain(); };

    // Levels ---------------------------------------------------------------
    inputMeter = std::make_unique<LevelMeter>();
    outputMeter = std::make_unique<LevelMeter>();
    addAndMakeVisible (*inputMeter);
    addAndMakeVisible (*outputMeter);

    addAndMakeVisible (inputChannelBox);
    inputChannelBox.onChange = [this]
    {
        engine.chain->setInputChannel (juce::jmax (0, inputChannelBox.getSelectedItemIndex()));
    };

    auto setupGain = [this] (juce::Slider& slider, juce::Label& label, const juce::String& name,
                             double minDb, double maxDb, GainMeterProcessor& stage)
    {
        label.setText (name, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.8f));
        addAndMakeVisible (label);

        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 24);
        slider.setRange (minDb, maxDb, 0.1);
        slider.setTextValueSuffix (" dB");
        slider.setValue (juce::Decibels::gainToDecibels (stage.gain.load()), juce::dontSendNotification);
        slider.setDoubleClickReturnValue (true, 0.0);
        slider.onValueChange = [&slider, &stage] { stage.gain = juce::Decibels::decibelsToGain ((float) slider.getValue()); };
        addAndMakeVisible (slider);
    };

    setupGain (inputGainSlider, inputGainLabel, "Input gain", -24.0, 24.0, engine.chain->getInputStage());
    setupGain (outputGainSlider, outputGainLabel, "Output gain", -60.0, 12.0, engine.chain->getOutputStage());

    monitorButton.setToggleState (! engine.chain->getOutputStage().muted.load(), juce::dontSendNotification);
    monitorButton.onClick = [this]
    {
        engine.chain->getOutputStage().muted = ! monitorButton.getToggleState();
        juce::Logger::writeToLog (monitorButton.getToggleState() ? "Output monitoring ON" : "Output monitoring OFF");
    };
    addAndMakeVisible (monitorButton);

    // MIDI -----------------------------------------------------------------
    midiDevicesLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.8f));
    midiDevicesLabel.setMinimumHorizontalScale (0.6f);
    addAndMakeVisible (midiDevicesLabel);

    lastMidiLabel.setText ("No MIDI received yet", juce::dontSendNotification);
    lastMidiLabel.setFont (juce::FontOptions (19.0f).withStyle ("Bold"));
    lastMidiLabel.setColour (juce::Label::textColourId, juce::Colour (0xffffd166));
    lastMidiLabel.setMinimumHorizontalScale (0.6f);
    addAndMakeVisible (lastMidiLabel);

    learnButton.onClick = [this]
    {
        learning = true;
        learnButton.setButtonText ("Press a footswitch now...");
    };

    engine.midi->onMessage = [this] (const juce::MidiMessage& m, const juce::String& source) { handleMidi (m, source); };
    engine.midi->onDevicesChanged = [this] { refreshMidiDevices(); };

    // Log ------------------------------------------------------------------
    logView.setMultiLine (true);
    logView.setReadOnly (true);
    logView.setScrollbarsShown (true);
    logView.setCaretVisible (false);
    logView.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    logView.setColour (juce::TextEditor::backgroundColourId, panelColour);
    addAndMakeVisible (logView);

    // Overlay (on top of everything) ----------------------------------------
    overlay = std::make_unique<Overlay>();
    overlay->onClose = [this] { closeOverlay(); };
    addChildComponent (*overlay);

    appLog.addChangeListener (this);
    engine.catalogue->addChangeListener (this);
    engine.deviceManager.addChangeListener (this);

    refreshCatalogue();
    refreshChain();
    refreshInputChannels();
    refreshMidiDevices();

    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    closeOverlay();

    appLog.removeChangeListener (this);
    engine.catalogue->removeChangeListener (this);
    engine.deviceManager.removeChangeListener (this);

    engine.chain->onChange = nullptr;
    engine.midi->onMessage = nullptr;
    engine.midi->onDevicesChanged = nullptr;

    catalogueList.setModel (nullptr);
    chainList.setModel (nullptr);
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

   #if JUCE_IOS
    if (auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds()))
        area = display->safeAreaInsets.subtractedFrom (area);
   #endif

    overlay->setBounds (area);

    area.reduce (12, 8);

    auto top = area.removeFromTop (48);
    saveButton.setBounds (top.removeFromRight (130).reduced (4));
    bluetoothButton.setBounds (top.removeFromRight (150).reduced (4));
    audioSettingsButton.setBounds (top.removeFromRight (150).reduced (4));
    titleLabel.setBounds (top);
    area.removeFromTop (6);

    constexpr int gap = 12;
    const auto columnWidth = (area.getWidth() - 2 * gap) / 3;
    auto left = area.removeFromLeft (columnWidth);
    area.removeFromLeft (gap);
    auto middle = area.removeFromLeft (columnWidth);
    area.removeFromLeft (gap);
    auto right = area;

    // Left: catalogue
    catalogueHeader.setBounds (left.removeFromTop (30));
    auto leftButtons = left.removeFromBottom (48);
    catalogueStatus.setBounds (left.removeFromBottom (26));

    if (scanButton.isVisible())
        scanButton.setBounds (leftButtons.removeFromRight (leftButtons.getWidth() / 2).reduced (3));

    addButton.setBounds (leftButtons.reduced (3));
    catalogueList.setBounds (left.reduced (0, 4));

    // Middle: chain
    chainHeader.setBounds (middle.removeFromTop (30));
    auto chainButtons = middle.removeFromBottom (48);
    const auto buttonWidth = chainButtons.getWidth() / 5;

    for (auto* b : { &editButton, &bypassButton, &upButton, &downButton, &removeButton })
        b->setBounds (chainButtons.removeFromLeft (buttonWidth).reduced (3));

    chainList.setBounds (middle.reduced (0, 4));

    // Right: levels, MIDI, log
    auto take = [&right] (int height) { return right.removeFromTop (height); };

    levelsHeader.setBounds (take (30));
    inputChannelBox.setBounds (take (38).reduced (0, 3));
    inputMeter->setBounds (take (24).reduced (0, 3));

    {
        auto r = take (36);
        inputGainLabel.setBounds (r.removeFromLeft (95));
        inputGainSlider.setBounds (r);
    }

    outputMeter->setBounds (take (24).reduced (0, 3));

    {
        auto r = take (36);
        outputGainLabel.setBounds (r.removeFromLeft (95));
        outputGainSlider.setBounds (r);
    }

    monitorButton.setBounds (take (36));
    take (8);

    midiHeader.setBounds (take (30));
    midiDevicesLabel.setBounds (take (30));
    lastMidiLabel.setBounds (take (36));
    learnButton.setBounds (take (48).reduced (0, 4));
    take (8);

    logHeader.setBounds (take (30));
    logView.setBounds (right);
}

//==============================================================================
void MainComponent::timerCallback()
{
    inputMeter->push (engine.chain->getInputStage().takePeak());
    outputMeter->push (engine.chain->getOutputStage().takePeak());

    if (logDirty)
    {
        logDirty = false;
        logView.setText (appLog.getText(), false);
        logView.moveCaretToEnd();
    }
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &appLog)
    {
        logDirty = true;
    }
    else if (source == engine.catalogue.get())
    {
        refreshCatalogue();
    }
    else if (source == &engine.deviceManager)
    {
        refreshInputChannels();
        engine.logAudioDeviceInfo();
    }
}

void MainComponent::refreshCatalogue()
{
    catalogueEntries = engine.catalogue->getEntries();
    catalogueList.updateContent();
    catalogueList.repaint();
    catalogueStatus.setText (engine.catalogue->getStatus(), juce::dontSendNotification);
    scanButton.setEnabled (! engine.catalogue->isScanning());
}

void MainComponent::refreshChain()
{
    chainList.updateContent();
    chainList.repaint();
}

void MainComponent::refreshInputChannels()
{
    inputChannelBox.clear (juce::dontSendNotification);

    if (auto* device = engine.deviceManager.getCurrentAudioDevice())
    {
        const auto names = device->getInputChannelNames();
        const auto active = device->getActiveInputChannels();
        int itemId = 1;

        for (int i = 0; i < names.size(); ++i)
            if (active[i])
                inputChannelBox.addItem (names[i].isNotEmpty() ? names[i] : "Input " + juce::String (i + 1), itemId++);
    }

    const auto numItems = inputChannelBox.getNumItems();
    inputChannelBox.setEnabled (numItems > 0);

    if (numItems == 0)
        inputChannelBox.setTextWhenNothingSelected ("No audio input");
    else
        inputChannelBox.setSelectedItemIndex (juce::jmin (engine.chain->getInputChannel(), numItems - 1),
                                              juce::dontSendNotification);
}

void MainComponent::refreshMidiDevices()
{
    const auto names = engine.midi->getEnabledInputNames();
    midiDevicesLabel.setText (names.isEmpty() ? juce::String ("No MIDI inputs - tap 'Bluetooth MIDI' to pair the footswitch")
                                              : "Inputs: " + names.joinIntoString (", "),
                              juce::dontSendNotification);
}

void MainComponent::handleMidi (const juce::MidiMessage& message, const juce::String& source)
{
    lastMidiLabel.setText (message.getDescription(), juce::dontSendNotification);
    juce::Logger::writeToLog ("MIDI [" + source + "] " + message.getDescription());

    if (learning)
    {
        const auto trigger = MidiTrigger::fromMessage (message);

        if (trigger.type != MidiTrigger::Type::none)
        {
            bypassTrigger = trigger;
            learning = false;
            learnButton.setButtonText ("Footswitch = " + trigger.describe() + "  (tap to re-learn)");
            juce::Logger::writeToLog ("Learned bypass footswitch: " + trigger.describe());
        }

        return;
    }

    if (bypassTrigger.matches (message))
    {
        auto index = selectedChainIndex();

        if (index < 0 && engine.chain->getNumPlugins() > 0)
            index = 0;

        if (index >= 0)
            engine.chain->setBypassed (index, ! engine.chain->isBypassed (index));
    }
}

//==============================================================================
void MainComponent::addSelectedPlugin()
{
    const auto row = catalogueList.getSelectedRow();

    if (! juce::isPositiveAndBelow (row, (int) catalogueEntries.size()))
    {
        juce::Logger::writeToLog ("Select a plugin in the list first");
        return;
    }

    addButton.setEnabled (false);

    engine.chain->addPlugin (catalogueEntries[(size_t) row].description, [safeThis = SafePointer<MainComponent> (this)] (const juce::String&)
    {
        if (safeThis != nullptr)
        {
            safeThis->addButton.setEnabled (true);
            safeThis->chainList.selectRow (safeThis->engine.chain->getNumPlugins() - 1);
        }
    });
}

void MainComponent::openEditor (int chainIndex)
{
    auto* plugin = engine.chain->getPlugin (chainIndex);

    if (plugin == nullptr)
        return;

    closeOverlay();

    std::unique_ptr<juce::Component> editor;

    if (plugin->hasEditor())
        editor.reset (plugin->createEditorAndMakeActive());

    if (editor == nullptr)
        editor = std::make_unique<juce::GenericAudioProcessorEditor> (*plugin);

    overlay->show (engine.chain->getPluginName (chainIndex), std::move (editor));
}

void MainComponent::openAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (engine.deviceManager,
                                                                          0, 8,    // inputs
                                                                          0, 8,    // outputs
                                                                          true,    // MIDI inputs
                                                                          false, false, false);
    overlay->show ("Audio settings", std::move (selector));
}

void MainComponent::closeOverlay()
{
    if (overlay != nullptr)
        overlay->hide();
}

int MainComponent::selectedChainIndex() const
{
    return chainList.getSelectedRow();
}

} // namespace pv
