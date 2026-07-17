#pragma once

// cyber.rest — Phase 4 stable player identity + on-disk paths.
//
// Two jobs, both host- AND client-relevant:
//
//   1) GUID: every player has a STABLE, PERSISTENT identity string the host keys its saved world
//      state under (see Protocol.hpp PlayerHello / HostStore). It is derived once and then never
//      changes for this machine/user:
//        * If a Steam ID is available we seed the GUID from it ("cr-steam-<id>"), so the same Steam
//          account is recognised across reinstalls.
//        * Otherwise we generate a random GUID once and persist it to a small file next to the
//          plugin ("cr-<16 hex>"). Subsequent launches load that file, so the id is stable.
//      IN-GAME ASSUMPTION (flagged): standalone GameNetworkingSockets does NOT require the Steam
//      client, and this project does not link the Steam client API, so in practice the persistent
//      random-GUID path is what runs. The Steam seed is a best-effort hook (GetSteamId64 returns 0
//      unless a build wires the Steam API up) — the random file GUID guarantees stability either way.
//
//   2) PATHS: resolve the directory the plugin DLL lives in, so both the GUID file (client) and the
//      persistence JSON (host) sit alongside the mod rather than in some arbitrary CWD.
//
// All functions are cheap and safe to call from the game/plugin thread. Results are cached after
// the first resolve (the GUID is computed once and memoised).

#include <cstdint>
#include <string>

namespace cr::Identity
{
// The directory the cyber.rest plugin DLL resides in (absolute, no trailing separator). Falls back
// to "." if the OS path lookup fails. Cached after first call.
const std::string& PluginDir();

// Absolute path to the client-side persistent GUID file (<PluginDir>/cyber_rest_guid.txt).
std::string GuidFilePath();

// Absolute path to the host-side persistence store (<PluginDir>/cyber_rest_world.json).
std::string HostStoreFilePath();

// This player's stable identity GUID (see the header comment). Computed + memoised on first call:
// prefers a Steam-seeded id, else loads/creates the persistent random file GUID. Always non-empty
// and always <= kGuidLen-1 chars so it fits the PlayerHello wire buffer with a NUL terminator.
const std::string& LocalGuid();

// Best-effort Steam ID (64-bit). Returns 0 when unavailable (the default in this build — see the
// header note). Isolated here so a later phase can wire the Steam client API in one place.
uint64_t GetSteamId64();
} // namespace cr::Identity
