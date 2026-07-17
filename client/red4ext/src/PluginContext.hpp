#pragma once

// Tiny logging facade over the RED4ext SDK logger.
//
// Everything in cyber.rest logs through cr::Log so we (a) don't repeat the
// g_sdk/g_plugin null-checks everywhere and (b) get a consistent "[cyber.rest]"
// prefix in the RED4ext log. The SDK logger is set up in Main/Load; calls made
// before that (there shouldn't be any) are silently dropped rather than crashing.

#include "Main.hpp"

#include <string>

namespace cr::Log
{
inline void Info(const std::string& aMessage)
{
    if (g_sdk && g_sdk->logger)
    {
        g_sdk->logger->Info(g_plugin, ("[cyber.rest] " + aMessage).c_str());
    }
}

inline void Warn(const std::string& aMessage)
{
    if (g_sdk && g_sdk->logger)
    {
        g_sdk->logger->Warn(g_plugin, ("[cyber.rest] " + aMessage).c_str());
    }
}

inline void Error(const std::string& aMessage)
{
    if (g_sdk && g_sdk->logger)
    {
        g_sdk->logger->Error(g_plugin, ("[cyber.rest] " + aMessage).c_str());
    }
}
} // namespace cr::Log
