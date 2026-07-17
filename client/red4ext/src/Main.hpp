#pragma once

// Global RED4ext SDK handles for the cyber.rest client plugin.
//
// The SDK hands us the logger + plugin handle exactly once (in Main during
// EMainReason::Load) and there is no way to fetch them again later, so we cache them
// here for the rest of the plugin (NetCore, CyberRestSystem, PluginContext) to use.

#include <RED4ext/Api/v1/PluginHandle.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>

namespace cr
{
// The SDK interface (logger, hooking, scripting, ...). Set once in Main/Load.
extern const RED4ext::v1::Sdk* g_sdk;

// Identifies our plugin to the extender (used as the first arg to every logger call).
extern RED4ext::v1::PluginHandle g_plugin;
} // namespace cr
