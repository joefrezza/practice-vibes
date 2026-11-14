cmake_minimum_required(VERSION 3.22)

project(PracticeVibes VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.5
    GIT_SHALLOW    TRUE)
FetchContent_Declare(signalsmith-stretch    # time-stretch / pitch-shift (MIT)
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG        1.4.0
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(JUCE signalsmith-stretch)

# Windows: the in-app viewer (documents, YouTube) uses Microsoft's WebView2 (runtime ships with Windows 11).
# The SDK package (headers + static loader) is downloaded from NuGet into the build folder, where JUCE's
# FindWebView2 looks for a "*Microsoft.Web.WebView2*" directory.
if(WIN32)
    set(PV_WEBVIEW2_VERSION 1.0.3485.44)
    FetchContent_Declare(webview2
        URL           https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${PV_WEBVIEW2_VERSION}
        DOWNLOAD_NAME webview2.zip
        SOURCE_DIR    ${CMAKE_BINARY_DIR}/nuget/Microsoft.Web.WebView2.${PV_WEBVIEW2_VERSION})
    FetchContent_MakeAvailable(webview2)
    set(JUCE_WEBVIEW2_PACKAGE_LOCATION ${CMAKE_BINARY_DIR}/nuget)
    set(PV_WEBVIEW2 TRUE)
else()
    set(PV_WEBVIEW2 FALSE)
endif()

# Treat third-party headers as system headers so our strict warnings don't apply to them.
set_target_properties(signalsmith-stretch signalsmith-linear PROPERTIES SYSTEM TRUE)

# AUv3 hosting exists only on Apple platforms, VST3 hosting only on desktop.
if(APPLE)
    set(PV_HOST_AU TRUE)
else()
    set(PV_HOST_AU FALSE)
endif()

juce_add_gui_app(PracticeVibes
    PRODUCT_NAME                   "Practice Vibes"
    COMPANY_NAME                   "Practice Vibes"
    BUNDLE_ID                      io.github.joefrezza.practicevibes
    VERSION                        ${PROJECT_VERSION}
    MICROPHONE_PERMISSION_ENABLED  TRUE
    MICROPHONE_PERMISSION_TEXT     "Practice Vibes processes the signal from your guitar."
    BLUETOOTH_PERMISSION_ENABLED   TRUE
    BLUETOOTH_PERMISSION_TEXT      "Practice Vibes connects to Bluetooth MIDI footswitches."
    BACKGROUND_AUDIO_ENABLED       TRUE
    FILE_SHARING_ENABLED           TRUE
    DOCUMENT_BROWSER_ENABLED       TRUE
    STATUS_BAR_HIDDEN              TRUE
    REQUIRES_FULL_SCREEN           TRUE
    TARGETED_DEVICE_FAMILY         "1,2"
    PLUGINHOST_AU                  ${PV_HOST_AU}
    NEEDS_WEB_BROWSER              TRUE
    NEEDS_WEBVIEW2                 ${PV_WEBVIEW2}
    NEEDS_CURL                     FALSE)

target_sources(PracticeVibes PRIVATE
    Source/Main.cpp
    Source/AppEngine.cpp
    Source/AppLog.cpp
    Source/ChordAnalyzer.cpp
    Source/ChordView.cpp
    Source/Footswitch.cpp
    Source/FootswitchPanel.cpp
    Source/Library.cpp
    Source/LibraryPanel.cpp
    Source/LoopFiles.cpp
    Source/Looper.cpp
    Source/LooperPanel.cpp
    Source/MainComponent.cpp
    Source/MetronomeProcessor.cpp
    Source/MidiMonitor.cpp
    Source/PlayerProcessor.cpp
    Source/PluginCatalogue.cpp
    Source/PluginChain.cpp
    Source/PracticeLog.cpp
    Source/PracticePanel.cpp
    Source/RigPanel.cpp
    Source/TodayPanel.cpp
    Source/Tuner.cpp
    Source/TunerPanel.cpp
    Source/WaveformView.cpp
    Source/YouTubePlayer.cpp)

target_compile_definitions(PracticeVibes PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PracticeVibes,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PracticeVibes,JUCE_VERSION>"
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_USE_CURL=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_SILENCE_XCODE_15_LINKER_WARNING=1)

if(WIN32)
    target_compile_definitions(PracticeVibes PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_USE_WINRT_MIDI=1     # Bluetooth LE MIDI on Windows 10/11
        JUCE_WASAPI=1
        JUCE_DIRECTSOUND=0)
endif()

target_link_libraries(PracticeVibes
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
        signalsmith-stretch
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Offline DSP tests (desktop only)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    juce_add_console_app(PracticeVibesTests PRODUCT_NAME "PracticeVibesTests")

    target_sources(PracticeVibesTests PRIVATE
        Tests/AudioTests.cpp
        Source/ChordAnalyzer.cpp
        Source/Footswitch.cpp
        Source/Library.cpp
        Source/LoopFiles.cpp
        Source/Looper.cpp
        Source/MetronomeProcessor.cpp
        Source/PlayerProcessor.cpp
        Source/PracticeLog.cpp
        Source/Tuner.cpp)

    target_compile_definitions(PracticeVibesTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1)

    target_link_libraries(PracticeVibesTests
        PRIVATE
            juce::juce_audio_utils
            juce::juce_dsp
            signalsmith-stretch
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    # Developer tool: prints the chord analysis of audio files.
    juce_add_console_app(ChordTool PRODUCT_NAME "ChordTool")
    target_sources(ChordTool PRIVATE Tools/ChordTool.cpp Source/ChordAnalyzer.cpp)
    target_compile_definitions(ChordTool PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(ChordTool
        PRIVATE
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
