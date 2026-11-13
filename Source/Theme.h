#pragma once

#include <juce_graphics/juce_graphics.h>

namespace pv::theme
{
    inline const juce::Colour background { 0xff1e1f24 };
    inline const juce::Colour panel      { 0xff2a2c33 };
    inline const juce::Colour accent     { 0xff3a6ea5 };
    inline const juce::Colour highlight  { 0xffffd166 };

    inline juce::FontOptions headerFont()  { return juce::FontOptions (17.0f).withStyle ("Bold"); }
}
