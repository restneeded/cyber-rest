// cyber.rest — RED4ext plugin entry point.
//
// Phase 0 responsibilities:
//   * cache the SDK handle + plugin handle,
//   * bring up GameNetworkingSockets once for the whole process,
//   * register our RTTI types (CyberRestSystem + GetCyberRestSystem) so redscript
//     can call HostGame / JoinGame / IsConnected,
//   * tear everything down on unload.
//
// The actual listen-server / client sockets live in NetCore; the redscript-facing
// native class lives in CyberRestSystem. This file only wires them together.

#include "Main.hpp"
#include "NetCore.hpp"
#include "PluginContext.hpp"

#include <RED4ext/RED4ext.hpp>
#include <RedLib.hpp>

#include <steam/steamnetworkingsockets.h>
#include <steam/steamnetworkingtypes.h>

namespace cr
{
const RED4ext::v1::Sdk* g_sdk = nullptr;
RED4ext::v1::PluginHandle g_plugin = nullptr;
} // namespace cr

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle aHandle, RED4ext::v1::EMainReason aReason,
                                        const RED4ext::v1::Sdk* aSdk)
{
    switch (aReason)
    {
    case RED4ext::v1::EMainReason::Load:
    {
        // Cache the handles first — cr::Log and everything downstream depends on them.
        cr::g_sdk = aSdk;
        cr::g_plugin = aHandle;

        // Bring up GameNetworkingSockets once for the process. If this fails we still
        // return true so the RTTI types register and the menu entry appears; the
        // Host/Join natives will just report failure until the library is available.
        SteamNetworkingErrMsg errMsg = {};
        if (!GameNetworkingSockets_Init(nullptr, errMsg))
        {
            cr::Log::Error(std::string("GameNetworkingSockets_Init failed: ") + errMsg);
        }
        else
        {
            cr::Log::Info("GameNetworkingSockets initialized");
        }

        // Register CyberRestSystem + the GetCyberRestSystem() accessor (and anything
        // else declared via RTTI_DEFINE_CLASS / RTTI_DEFINE_GLOBALS) with the game's
        // RTTI system so the body-less `native` redscript declarations bind to our C++.
        Red::TypeInfoRegistrar::RegisterDiscovered();

        cr::Log::Info("cyber.rest client loaded");
        break;
    }

    case RED4ext::v1::EMainReason::Unload:
    {
        // Stop the worker thread + close any sockets, then shut GNS down.
        cr::NetCore::Shutdown();
        GameNetworkingSockets_Kill();
        cr::Log::Info("cyber.rest client unloaded");
        break;
    }
    }

    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* aInfo)
{
    aInfo->name = L"cyber.rest";
    aInfo->author = L"restneeded";
    aInfo->version = RED4EXT_V1_SEMVER(0, 1, 0);
    // Pin to the latest game runtime the SDK knows about (Cyberpunk 2077 v2.x).
    aInfo->runtime = RED4EXT_V1_RUNTIME_VERSION_LATEST;
    aInfo->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports()
{
    return RED4EXT_API_VERSION_1;
}
