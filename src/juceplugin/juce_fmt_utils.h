#pragma once

#include "juce_core/juce_core.h"
#include <fmt/format.h>

// Specialization to allow fmt to format juce::String
template <> struct fmt::formatter<juce::String> : formatter<std::string>
{
    auto format(const juce::String &str, format_context &ctx) const
    {
        return formatter<std::string>::format(str.toStdString(), ctx);
    }
};

namespace Log
{
template <typename... Args> void info(fmt::format_string<Args...> fmt, Args &&...args)
{
    std::string formatted = fmt::format(fmt, std::forward<Args>(args)...);
    juce::Logger::writeToLog(juce::String(formatted));
}
} // namespace Log
