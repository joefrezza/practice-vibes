#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "AppPaths.h"

namespace pv
{

/** Web view settings shared by the document viewer and the YouTube player:
    WebView2 on Windows (data folder inside the app's folder), WKWebView on Apple platforms.
*/
inline juce::WebBrowserComponent::Options webViewOptions()
{
    auto options = juce::WebBrowserComponent::Options{};

   #if JUCE_WINDOWS
    options = options.withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
                     .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                                                  .withUserDataFolder (getAppDataFolder().getChildFile ("WebView2")));
   #endif

    return options;
}

/** False on a Windows PC without the WebView2 runtime: callers then open things in the browser instead. */
inline bool webViewAvailable()
{
    static const bool available = juce::WebBrowserComponent::areOptionsSupported (webViewOptions());
    return available;
}

} // namespace pv
