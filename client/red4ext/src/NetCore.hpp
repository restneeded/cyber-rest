#pragma once

// cyber.rest — net core.
//
// A single, process-wide GameNetworkingSockets endpoint that runs in one of two modes on
// a dedicated worker thread:
//
//   * HOST  — CreateListenSocketIP, accept connections, assign each a netId, and RELAY
//             presence: every client learns of every other via PlayerJoin/PlayerLeave,
//             and each client's PlayerTransform is forwarded to the OTHER clients. It is also
//             the AUTHORITY for Phase-3 PvP: it owns each player's HP, applies sanity-checked
//             hit claims, and broadcasts PlayerHealth/PlayerDeath/PlayerRespawn.
//   * CLIENT— ConnectByIPAddress to host:port, send our own PlayerTransform up, and
//             receive the join/leave/transform stream for the other players. For PvP it reports
//             its own PlayerFire (cosmetic) + PlayerHit (damage claim) up and applies only the
//             host's authoritative PlayerHealth/Death/Respawn.
//
// Phase 0 was a text hello/hello-ack handshake. Phase 1 replaces the payloads with the
// binary presence protocol in Protocol.hpp while keeping the same threading model:
//
//   * ALL GNS calls (listen/connect/RunCallbacks/Receive/Send/Close) happen on the worker
//     thread, because ISteamNetworkingSockets is single-threaded per its docs.
//   * The worker NEVER touches game/RTTI state. It only parses packets into a mutex-
//     guarded inbound event queue (s_events). The GAME thread drains that queue each frame
//     (via CyberRestSystem) and does all spawning/movement in redscript.
//   * The game thread pushes the local player's transform down via PushLocalTransform();
//     the worker picks it up (mutex-guarded s_localXform) and sends it on the next pump.
//
// The public API is what CyberRestSystem (the redscript-facing native) delegates to.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <steam/steamnetworkingtypes.h>

#include "Protocol.hpp"

// Forward declaration so the header doesn't have to pull the full GNS interface in.
class ISteamNetworkingSockets;

namespace cr
{
enum class NetMode
{
    Idle,
    Host,
    Client,
};

// One parsed inbound presence/combat event, produced on the worker thread and consumed on the
// game thread. A flat POD (no handles/pointers) so it crosses the thread boundary safely
// by value. kind selects which fields are meaningful:
//   Join/Leave  -> netId only
//   Transform   -> netId + the xform (PlayerTransform) payload
//   Appearance  -> netId + the appear (PlayerAppearance) payload
//   Fire        -> netId (== shooter) + the fire (PlayerFire) payload   [Phase 3: cosmetic]
//   Health      -> netId + hp                                           [Phase 3: authoritative HP]
//   Death       -> netId only                                          [Phase 3]
//   Respawn     -> netId + respawn (PlayerRespawn) payload              [Phase 3]
//   Stats       -> netId + the stats (PlayerStats) payload              [Phase 4: persistent K/D]
//   RestorePos  -> netId + the respawn (PlayerRespawn) payload          [Phase 4: host tells a
//                  returning player where their saved position is so reds teleports the local V]
struct NetEvent
{
    enum class Kind : uint8_t
    {
        Join,
        Leave,
        Transform,
        Appearance,
        Fire,
        Health,
        Death,
        Respawn,
        Stats,      // Phase 4: persistent kills/deaths update
        RestorePos, // Phase 4: restore a returning player's saved position (uses `respawn`)
    };

    Kind kind;
    uint32_t netId;
    PlayerTransform xform;   // only valid when kind == Transform
    PlayerAppearance appear; // only valid when kind == Appearance
    PlayerFire fire;         // only valid when kind == Fire
    PlayerRespawn respawn;   // only valid when kind == Respawn or RestorePos
    PlayerStats stats;       // only valid when kind == Stats
    float hp;                // only valid when kind == Health
};

class NetCore
{
public:
    // Start an in-process listen server on aPort and spin up the worker thread. As HOST we
    // are also a participant: our own netId is kLocalHostNetId. Returns false if a session
    // is already running or the listen socket failed to create.
    static bool StartHost(uint16_t aPort);

    // Connect as a client to aHost:aPort and spin up the worker thread. Returns false if a
    // session is already running or the connect could not be initiated. Connection is
    // asynchronous; poll IsConnected().
    static bool StartClient(const std::string& aHost, uint16_t aPort);

    // True once we have an established, usable session (client: connected to host; host:
    // listen socket is up). Presence flows only while this is true.
    static bool IsConnected();

    // Our own network id for this session (host = kLocalHostNetId; client = assigned by
    // host, 0 until the host tells us — see PlayerTransform relay). Used so the game side
    // can ignore echoes of its own player. 0 means "not yet assigned".
    static uint32_t LocalNetId();

    // Game thread -> worker: publish the local player's latest transform. Cheap + lock-
    // guarded; the worker sends whatever the most recent value is on its next pump. Only
    // the movement fields matter here — netId/tick are filled by the worker.
    static void PushLocalTransform(float px, float py, float pz, float yaw, float vx, float vy, float vz,
                                   uint8_t locoState);

    // Game thread -> worker: publish the local player's appearance (body gender + the raw
    // TweakDBID hashes of the clothing in each synced slot). Latest-wins, lock-guarded. The
    // worker sends it ONCE the connection is established (and re-sends if the game pushes an
    // updated look). aClothing points at kAppearanceSlots u64s (caller owns the storage).
    static void PushLocalAppearance(uint8_t bodyGender, const uint64_t* aClothing);

    // Game thread -> worker: publish the local player's stable identity GUID (Phase 4). Set once
    // before/at connect. The client sends it as its PlayerHello (identity/auth); the host binds it
    // to its own kLocalHostNetId. Lock-guarded (reuses s_localMutex). Copies the string.
    static void SetLocalGuid(const std::string& aGuid);

    // ---- Phase 3 (PvP): game thread -> worker ----
    //
    // Publish that the local V just fired a shot (cosmetic — drives remote tracer/muzzle VFX).
    // Each call is a DISCRETE event (queued, not latest-wins) so no shot is coalesced away.
    // netId is filled by the worker (client 0 -> host stamps). aOrigin/aDir are 3 floats each.
    static void PushLocalFire(uint64_t aWeaponTweakID, float aOx, float aOy, float aOz, float aDx, float aDy,
                              float aDz);

    // Publish that the local V's shot hit a victim (shooter-reports damage claim). Queued as a
    // discrete event. The host applies + broadcasts authoritative HP; a client sends it up to
    // the host. aVictimNetId is the hit player's netId; aDamage is the claimed amount.
    static void PushLocalHit(uint32_t aVictimNetId, float aDamage);

    // Game thread <- worker: move all queued inbound events into aOut (which is cleared
    // first) and empties the internal queue. Called once per frame by CyberRestSystem.
    static void DrainEvents(std::vector<NetEvent>& aOut);

    // Stop the worker thread and close all sockets. Safe to call when idle.
    static void Shutdown();

private:
    // ---- worker-thread body + helpers (all run on s_thread) ----
    static void ThreadMain();
    static bool SetupHost(uint16_t aPort);
    static bool SetupClient(const std::string& aHost, uint16_t aPort);
    static void PollMessages();
    static void SendLocalHello();      // client: send our identity GUID once, right after connect (Phase 4)
    static void SendLocalTransform();  // flush the pending local transform to the wire
    static void SendLocalAppearance(); // flush the local appearance once (and on change)
    static void SendLocalCombat();     // flush queued outgoing fire/hit events (Phase 3)
    static void ServiceRespawns();     // host: fire due respawns (Phase 3)

    // Host-side helpers (worker thread). aSendFlags is a k_nSteamNetworkingSend_* mask:
    // presence (join/leave) is sent Reliable so it can never be lost; high-frequency
    // transforms are sent UnreliableNoDelay so a stale one is dropped rather than queued.
    static void HostHandleTransform(HSteamNetConnection aFrom, const PlayerTransform& aXform);
    static void HostHandleAppearance(HSteamNetConnection aFrom, const PlayerAppearance& aAppear);

    // ---- Phase 3 host combat (worker thread) ----
    // Relay a client's cosmetic fire to the OTHER clients + surface it locally (stamps sender id).
    static void HostHandleFire(HSteamNetConnection aFrom, const PlayerFire& aFire);
    // Apply a claimed hit to authoritative HP (sanity-checked), then broadcast the result.
    static void HostHandleHit(HSteamNetConnection aFrom, const PlayerHit& aHit);
    // Apply damage to aVictimNetId's HP, clamp, broadcast PlayerHealth, and on 0 broadcast
    // PlayerDeath + schedule a respawn. aSourceId is the shooter (records a persistent kill/death
    // on the store when the victim dies). Host-only.
    static void HostApplyDamage(uint32_t aVictimNetId, float aDamage, uint32_t aSourceId);

    // ---- Phase 4 host persistence (worker thread) ----
    // A client sent its identity hello: map its netId<->GUID, restore its saved state (position,
    // appearance, stats) from the store, and send that state back to it. Host-only.
    static void HostHandleHello(HSteamNetConnection aFrom, const PlayerHello& aHello);
    // Broadcast a player's persistent stats to everyone + surface locally. Host-only.
    static void HostBroadcastStats(uint32_t aNetId, uint32_t aKills, uint32_t aDeaths);
    // The GUID the host associates with a netId (empty if none / the netId is unknown). Host-only.
    static std::string GuidForNetId(uint32_t aNetId);
    // Broadcast one POD to EVERY client (host is not on a connection, so it's surfaced locally
    // by the caller separately). Used for host-authoritative health/death/respawn fan-out.
    static void HostBroadcast(const void* aData, uint32_t aSize, int aSendFlags);
    // Ensure a netId has an HP entry (starts full). Host-only.
    static void HostEnsureHealth(uint32_t aNetId);
    // Send the stored appearance for aNetId to aConn if the host has one on file; else no-op.
    static void HostSendAppearanceOnFile(HSteamNetConnection aConn, uint32_t aNetId);
    static void HostRelayToOthers(HSteamNetConnection aExcept, const void* aData, uint32_t aSize, int aSendFlags);
    static void HostSendTo(HSteamNetConnection aConn, const void* aData, uint32_t aSize, int aSendFlags);
    static void HostOnClientConnected(HSteamNetConnection aConn);
    static void HostOnClientGone(HSteamNetConnection aConn);

    // Push a parsed event onto the game-thread-facing queue (worker thread side).
    static void QueueEvent(const NetEvent& aEvent);

public:
    // GNS delivers connection-state changes via this static trampoline. Runs on the worker
    // thread inside RunCallbacks(). Public so the .cpp's free-function trampoline can
    // forward to it.
    static void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* aInfo);

private:
    // ---- session control ----
    static std::mutex s_mutex; // guards start/stop transitions
    static std::thread s_thread;
    static std::atomic<bool> s_running;
    static std::atomic<bool> s_connected;
    static std::atomic<uint32_t> s_localNetId;

    static NetMode s_mode;
    static uint16_t s_port;
    static std::string s_hostAddr;

    // ---- GNS handles (worker-thread-owned after startup) ----
    static ISteamNetworkingSockets* s_interface;
    static HSteamListenSocket s_listenSocket;
    static HSteamNetPollGroup s_pollGroup;   // host: groups all accepted client conns
    static HSteamNetConnection s_connection; // client: our single connection to the host

    // ---- host bookkeeping (worker-thread-only) ----
    // Every accepted client connection gets a stable netId. Two maps keep the mapping both
    // ways so we can (a) stamp a sender's netId onto its relayed transform and (b) tell the
    // survivors which netId left. Touched only on the worker thread.
    static std::unordered_map<HSteamNetConnection, uint32_t> s_connToNetId;
    static uint32_t s_nextNetId; // next id to hand out (host only)

    // Latest appearance the host has seen for each netId (keyed by netId, includes the host's
    // OWN appearance under kLocalHostNetId once pushed). Used to replay every present player's
    // look to a newcomer. Worker-thread-only. A client caches nothing here — it just relays
    // appearance events up to the game thread.
    static std::unordered_map<uint32_t, PlayerAppearance> s_appearanceByNetId;

    // ---- Phase 3 host combat state (worker-thread-only) ----
    // Authoritative HP per netId (host is the sole writer). Includes the host's own player under
    // kLocalHostNetId. Populated lazily to kMaxHealth on first join/reference.
    static std::unordered_map<uint32_t, float> s_healthByNetId;
    // Pending respawns: netId -> steady-clock time (seconds) at which to respawn. The host fires
    // each due entry from ServiceRespawns() (broadcast PlayerRespawn + full-HP PlayerHealth).
    static std::unordered_map<uint32_t, double> s_respawnDueAt;

    // ---- Phase 4 host persistence state (worker-thread-only) ----
    // The stable GUID the host has bound to each netId (from PlayerHello). Used to key the
    // persistence store. Includes the host's own player under kLocalHostNetId once it sends its
    // hello locally. Cleared per session + on client-gone.
    static std::unordered_map<uint32_t, std::string> s_guidByNetId;
    // Wall-clock (steady) seconds of the last position save per netId, so we persist a moving
    // player's position only every kPositionSaveIntervalSeconds rather than every packet (file IO
    // off the hot path — the store also only actually writes on its own thread).
    static std::unordered_map<uint32_t, double> s_lastPosSaveAt;
    // How often (seconds) the host persists a player's live position while they move.
    static constexpr double kPositionSaveIntervalSeconds = 5.0;

    // ---- Phase 4 local identity hand-off (game thread -> worker) ----
    // The local player's stable GUID, published once by the game thread before/at connect. The
    // client sends it as its PlayerHello; the host records its own under kLocalHostNetId. Guarded
    // by s_localMutex (reuses that lock — it's set once and read on the worker thread).
    static std::string s_localGuid;
    static bool s_helloSent; // client: has the PlayerHello been sent this session?

    // ---- local-transform hand-off (game thread -> worker) ----
    static std::mutex s_localMutex;
    static PlayerTransform s_localXform; // latest local sample (movement fields)
    static bool s_localXformValid;       // false until the game thread has pushed once
    static uint64_t s_localTick;         // monotonic tick stamped into outgoing transforms

    // ---- local-appearance hand-off (game thread -> worker) ----
    // Latest pushed local appearance + a "dirty" flag. The worker sends it when connected and
    // dirty, then clears dirty; the game thread re-sets it whenever the local look changes.
    static std::mutex s_appearanceMutex;
    static PlayerAppearance s_localAppearance;
    static bool s_localAppearanceValid; // game thread has pushed at least once
    static bool s_localAppearanceDirty; // a (re)send is pending

    // ---- Phase 3: outgoing combat queue (game thread -> worker) ----
    // Discrete outgoing fire/hit events the game thread produced. Unlike transform/appearance
    // these are NOT latest-wins — every shot/hit matters — so they queue and the worker drains
    // + sends them each pump. Small tagged POD so one queue carries both kinds.
    struct OutCombat
    {
        enum class Kind : uint8_t
        {
            Fire,
            Hit,
        };
        Kind kind;
        PlayerFire fire; // valid when kind == Fire
        PlayerHit hit;   // valid when kind == Hit
    };
    static std::mutex s_outCombatMutex;
    static std::vector<OutCombat> s_outCombat;

    // ---- inbound event queue (worker -> game thread) ----
    static std::mutex s_eventsMutex;
    static std::vector<NetEvent> s_events;

public:
    // Reserved netId for the host's own local player. Clients get ids starting above this.
    static constexpr uint32_t kLocalHostNetId = 1;
};
} // namespace cr
