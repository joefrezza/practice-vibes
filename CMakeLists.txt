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
    NEEDS_WEB_BROWSER              FALSE
    NEEDS_CURL                     FALSE)

target_sources(PracticeVibes PRIVATE
    Source/Main.cpp
    Source/AppEngine.cpp
    Source/AppLog.cpp
    Source/MainComponent.cpp
    Source/MetronomeProcessor.cpp
    Source/MidiMonitor.cpp
    Source/PlayerProcessor.cpp
    Source/PluginCatalogue.cpp
    Source/PluginChain.cpp
    Source/PracticePanel.cpp
    Source/RigPanel.cpp
    Source/WaveformView.cpp)

target_compile_definitions(PracticeVibes PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PracticeVibes,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PracticeVibes,JUCE_VERSION>"
    JUCE_WEB_BROWSER=0
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
        Source/MetronomeProcessor.cpp
        Source/PlayerProcessor.cpp)

    target_compile_definitions(PracticeVibesTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1)

    target_link_libraries(PracticeVibesTests
        PRIVATE
            juce::juce_audio_utils
            signalsmith-stretch
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
