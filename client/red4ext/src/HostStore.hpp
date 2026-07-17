#pragma once

// cyber.rest — Phase 4 host persistence store.
//
// A tiny, dependency-free persistence layer that lives ONLY on the host. It remembers, keyed by a
// player's stable GUID (see Protocol.hpp PlayerHello), the world state we want to survive a
// disconnect / a host restart:
//
//   * last position + yaw  (so a returning player spawns where they left off)
//   * appearance           (body gender + the kAppearanceSlots clothing TweakDBID hashes)
//   * PvP stats            (lifetime kills + deaths)
//
// Design constraints (from the phase brief):
//   * HOST ONLY. Clients never touch this.
//   * No heavy dependency: we hand-roll a minimal JSON reader/writer (flat object of records). The
//     format is human-readable and easy to hand-edit, but we never trust it blindly — parsing is
//     defensive and a malformed field just falls back to a default.
//   * File IO stays OFF the netcode hot path. The GNS worker thread only ever touches the in-memory
//     map (all methods here take s_mutex and return fast). A SEPARATE flush thread owned by this
//     class does the actual disk writes: it wakes on a dirty flag + a periodic interval and on
//     shutdown, so the worker never blocks on the filesystem.
//
// Threading contract:
//   * Load()  — called once from the game/plugin thread at host start (before the flush thread runs
//               heavily); it reads the file synchronously into the map. Cheap + one-shot.
//   * Get/Upsert*/RecordKill/RecordDeath — called from the GNS worker thread. Mutex-guarded,
//               in-memory only, and they set the dirty flag so the flush thread persists later.
//   * Start()/Stop() — spin the flush thread up/down (host session start / teardown).
//
// The store is process-wide static (one host per process), mirroring NetCore's shape.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "Protocol.hpp"

namespace cr
{
// One persisted player. Plain fields (not a wire POD — it carries a std::string GUID + is only ever
// in host memory / on disk, never sent as-is). Appearance mirrors PlayerAppearance's payload.
struct PlayerRecord
{
    std::string guid;

    // Last known transform (world position + facing yaw). hasPosition is false until we've seen at
    // least one transform for this player, so a brand-new player isn't teleported to (0,0,0).
    bool hasPosition = false;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float yaw = 0.0f;

    // Appearance. hasAppearance is false until the player has sent one.
    bool hasAppearance = false;
    uint8_t bodyGender = 0;                    // one of BodyGender
    uint64_t clothing[kAppearanceSlots] = {};  // raw TweakDBID hashes; 0 = empty

    // Lifetime PvP stats.
    uint32_t kills = 0;
    uint32_t deaths = 0;
};

class HostStore
{
public:
    // Load the store file synchronously into memory + start the background flush thread. Called on
    // host start. aFilePath is the absolute path to the JSON file; the directory must exist (the
    // caller resolves it next to the plugin). Safe to call again after Stop(). Idempotent-ish: a
    // second Start() without a Stop() is ignored.
    static void Start(const std::string& aFilePath);

    // Flush any pending changes and stop the background thread. Called on host teardown. Safe when
    // not started.
    static void Stop();

    // Copy out the record for a GUID. Returns true + fills aOut if the GUID is known; false if this
    // is a first-time player. Worker-thread safe.
    static bool Get(const std::string& aGuid, PlayerRecord& aOut);

    // Ensure a record exists for a GUID (create an empty one if new). Returns true if it was newly
    // created (first-time player), false if it already existed. Worker-thread safe.
    static bool EnsureRecord(const std::string& aGuid);

    // Update the saved transform for a GUID (creates the record if missing). Worker-thread safe.
    static void UpsertPosition(const std::string& aGuid, float aPx, float aPy, float aPz, float aYaw);

    // Update the saved appearance for a GUID (creates the record if missing). aClothing points at
    // kAppearanceSlots u64s. Worker-thread safe.
    static void UpsertAppearance(const std::string& aGuid, uint8_t aBodyGender, const uint64_t* aClothing);

    // Increment a GUID's lifetime kills / deaths (creates the record if missing). Worker-thread safe.
    static void RecordKill(const std::string& aGuid);
    static void RecordDeath(const std::string& aGuid);

    // Read a GUID's current stats (0/0 if unknown). Worker-thread safe.
    static void GetStats(const std::string& aGuid, uint32_t& aKills, uint32_t& aDeaths);

    // Force a synchronous flush to disk right now (used on disconnect + shutdown so a crash after
    // this point still keeps the latest state). Worker-thread safe; briefly holds s_mutex.
    static void FlushNow();

private:
    static void FlushThreadMain();
    static void SaveToDiskLocked(); // writes s_records to s_filePath; caller need not hold s_mutex
    static void LoadFromDisk();     // reads s_filePath into s_records (called once from Start)

    // Get-or-create a record pointer under the lock (caller must hold s_mutex).
    static PlayerRecord& TouchLocked(const std::string& aGuid);

    static std::mutex s_mutex; // guards s_records + s_dirty
    static std::unordered_map<std::string, PlayerRecord> s_records;
    static bool s_dirty; // a change is pending a disk write

    static std::string s_filePath;

    static std::thread s_thread;
    static std::atomic<bool> s_running;
    static std::condition_variable s_cv; // flush thread waits on this (dirty flag / interval / stop)
    static std::mutex s_cvMutex;

    // How often the flush thread persists if dirty (seconds). Keeps disk writes infrequent.
    static constexpr int kFlushIntervalSeconds = 15;
};
} // namespace cr
