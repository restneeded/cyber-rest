// cyber.rest — Phase 4 identity + paths implementation.
//
// See Identity.hpp. The GUID is resolved once and memoised; the plugin directory is resolved from
// the running module (this DLL) so paths are correct regardless of the game's working directory.

#include "Identity.hpp"
#include "PluginContext.hpp"
#include "Protocol.hpp" // kGuidLen

#include <array>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace cr::Identity
{
namespace
{
std::once_flag g_dirOnce;
std::string g_pluginDir;

std::once_flag g_guidOnce;
std::string g_localGuid;

// Anchor symbol whose address is inside THIS DLL, so GetModuleHandleExW resolves our own module
// (not the game exe or another plugin) to locate where the plugin file lives.
void ModuleAnchor() {}

// Convert a wide OS path to a narrow std::string (UTF-8). Best-effort; paths here are ASCII in
// practice (the mod folder), so a lossy conversion is acceptable if it ever isn't.
std::string NarrowPath(const std::wstring& aWide)
{
    if (aWide.empty())
    {
        return std::string();
    }
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), static_cast<int>(aWide.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (len <= 0)
    {
        return std::string();
    }
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), static_cast<int>(aWide.size()), out.data(), len, nullptr,
                          nullptr);
    return out;
}

void ResolvePluginDir()
{
    HMODULE mod = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&ModuleAnchor), &mod) &&
        mod != nullptr)
    {
        std::array<wchar_t, MAX_PATH> buf{};
        const DWORD n = ::GetModuleFileNameW(mod, buf.data(), static_cast<DWORD>(buf.size()));
        if (n > 0 && n < buf.size())
        {
            std::wstring full(buf.data(), n);
            const size_t slash = full.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
            {
                g_pluginDir = NarrowPath(full.substr(0, slash));
            }
        }
    }

    if (g_pluginDir.empty())
    {
        g_pluginDir = "."; // fall back to CWD; still functional
        Log::Warn("Identity: could not resolve plugin directory; using \".\"");
    }
    else
    {
        Log::Info("Identity: plugin directory = " + g_pluginDir);
    }
}

// Read a trimmed single-line GUID from a file (empty if missing/unreadable).
std::string ReadGuidFile(const std::string& aPath)
{
    std::ifstream f(aPath, std::ios::binary);
    if (!f.is_open())
    {
        return std::string();
    }
    std::string line;
    std::getline(f, line);
    // Trim whitespace/newlines.
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
    {
        line.pop_back();
    }
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
    {
        line.erase(line.begin());
    }
    return line;
}

void WriteGuidFile(const std::string& aPath, const std::string& aGuid)
{
    std::ofstream f(aPath, std::ios::binary | std::ios::trunc);
    if (!f.is_open())
    {
        Log::Warn("Identity: could not write GUID file " + aPath + " (id will not persist)");
        return;
    }
    f << aGuid << "\n";
}

// Generate "cr-<16 random hex>" using a non-deterministic seed. Well within kGuidLen.
std::string GenerateRandomGuid()
{
    std::random_device rd;
    std::mt19937_64 gen(((static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd())));
    const uint64_t v = gen();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "cr-%016llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

void ResolveGuid()
{
    // 1) Prefer a Steam-seeded id if the (best-effort) Steam API is wired up in this build.
    const uint64_t steamId = GetSteamId64();
    if (steamId != 0)
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "cr-steam-%llu", static_cast<unsigned long long>(steamId));
        g_localGuid = buf;
        Log::Info("Identity: using Steam-seeded GUID");
    }
    else
    {
        // 2) Persistent random file GUID. Load an existing one, else make + save a new one.
        const std::string path = GuidFilePath();
        std::string existing = ReadGuidFile(path);
        if (existing.empty())
        {
            existing = GenerateRandomGuid();
            WriteGuidFile(path, existing);
            Log::Info("Identity: generated new persistent GUID");
        }
        else
        {
            Log::Info("Identity: loaded persistent GUID from file");
        }
        g_localGuid = existing;
    }

    // Hard-guarantee it fits the fixed wire buffer with room for a NUL terminator.
    if (g_localGuid.size() >= kGuidLen)
    {
        g_localGuid.resize(kGuidLen - 1);
    }
    if (g_localGuid.empty())
    {
        g_localGuid = "cr-unknown"; // never empty
    }
}
} // namespace

const std::string& PluginDir()
{
    std::call_once(g_dirOnce, ResolvePluginDir);
    return g_pluginDir;
}

std::string GuidFilePath()
{
    return PluginDir() + "\\cyber_rest_guid.txt";
}

std::string HostStoreFilePath()
{
    return PluginDir() + "\\cyber_rest_world.json";
}

const std::string& LocalGuid()
{
    std::call_once(g_guidOnce, ResolveGuid);
    return g_localGuid;
}

uint64_t GetSteamId64()
{
    // Best-effort hook. This project links standalone GameNetworkingSockets, which does NOT require
    // the Steam client, and it does not link the Steam client API — so there is no live SteamUser to
    // query here and we return 0 (the persistent random-GUID path then guarantees a stable id).
    //
    // A later phase that links steam_api can replace this body with:
    //   if (SteamAPI_Init() && SteamUser()) return SteamUser()->GetSteamID().ConvertToUint64();
    // without touching any caller.
    return 0;
}
} // namespace cr::Identity
