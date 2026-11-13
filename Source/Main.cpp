#include <juce_gui_basics/juce_gui_basics.h>
#include "AppEngine.h"
#include "AppLog.h"
#include "AppPaths.h"
#include "MainComponent.h"

namespace pv
{

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (const juce::String& name, AppEngine& engine, AppLog& log)
        : DocumentWindow (name, juce::Colour (0xff1e1f24), DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainComponent (engine, log), true);

       #if JUCE_IOS || JUCE_ANDROID
        setFullScreen (true);
       #else
        setResizable (true, true);
        setResizeLimits (900, 600, 4000, 3000);
        centreWithSize (1280, 800);
       #endif

        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

//==============================================================================
class PracticeVibesApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        log = std::make_unique<AppLog>();

        juce::Logger::writeToLog (getApplicationName() + " " + getApplicationVersion() + " starting on "
                                  + juce::SystemStats::getOperatingSystemName()
                                  + " (" + juce::SystemStats::getDeviceDescription() + ")");
        juce::Logger::writeToLog ("Data folder: " + getAppDataFolder().getFullPathName());

        engine = std::make_unique<AppEngine>();
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine, *log);
        engine->start();

       #if ! (JUCE_IOS || JUCE_ANDROID)
        // Desktop: a song file passed on the command line is opened in the player.
        const auto argument = commandLine.trim().unquoted();

        if (argument.isNotEmpty() && juce::File::isAbsolutePath (argument) && juce::File (argument).existsAsFile())
            engine->player->loadAsync (juce::URL (juce::File (argument)), {});
       #else
        juce::ignoreUnused (commandLine);
       #endif
    }

    void shutdown() override
    {
        if (engine != nullptr)
            engine->saveState();

        mainWindow.reset();
        engine.reset();
        log.reset();
    }

    void suspended() override
    {
        // iOS: the app is going to the background and may be killed without warning.
        if (engine != nullptr)
            engine->saveState();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    std::unique_ptr<AppLog> log;
    std::unique_ptr<AppEngine> engine;
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace pv

START_JUCE_APPLICATION (pv::PracticeVibesApplication)
