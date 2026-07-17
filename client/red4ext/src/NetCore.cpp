// cyber.rest — net core implementation (GNS host/client + presence relay).
//
// Threading model (unchanged from Phase 0): one worker thread owns the
// ISteamNetworkingSockets interface. It creates the socket(s), then loops calling
// RunCallbacks() + draining/sending messages until asked to stop. The public entry points
// (Start*/Shutdown/IsConnected/PushLocalTransform/DrainEvents) are called from the game
// thread and only touch atomics + mutex-guarded hand-off buffers.
//
// Phase 1 adds the presence protocol (Protocol.hpp):
//   * Host assigns a netId per accepted connection, relays each client's PlayerTransform
//     to the OTHER clients, and announces Join/Leave to everyone (and the existing roster
//     to a newcomer).
//   * Client sends its own PlayerTransform up and receives the others'.
//   * Both parse inbound packets into s_events for the game thread to drain.

#include "NetCore.hpp"
#include "HostStore.hpp"
#include "Identity.hpp"
#include "PluginContext.hpp"

#include <chrono>
#include <cstring>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h> // needed for the utils vtable, see GNS issue #171
#include <steam/steamnetworkingsockets.h>

namespace cr
{
// ---- static storage ----
std::mutex NetCore::s_mutex;
std::thread NetCore::s_thread;
std::atomic<bool> NetCore::s_running{false};
std::atomic<bool> NetCore::s_connected{false};
std::atomic<uint32_t> NetCore::s_localNetId{0};

NetMode NetCore::s_mode = NetMode::Idle;
uint16_t NetCore::s_port = 0;
std::string NetCore::s_hostAddr;

ISteamNetworkingSockets* NetCore::s_interface = nullptr;
HSteamListenSocket NetCore::s_listenSocket = k_HSteamListenSocket_Invalid;
HSteamNetPollGroup NetCore::s_pollGroup = k_HSteamNetPollGroup_Invalid;
HSteamNetConnection NetCore::s_connection = k_HSteamNetConnection_Invalid;

std::unordered_map<HSteamNetConnection, uint32_t> NetCore::s_connToNetId;
uint32_t NetCore::s_nextNetId = 0;
std::unordered_map<uint32_t, PlayerAppearance> NetCore::s_appearanceByNetId;
std::unordered_map<uint32_t, float> NetCore::s_healthByNetId;
std::unordered_map<uint32_t, double> NetCore::s_respawnDueAt;
std::unordered_map<uint32_t, std::string> NetCore::s_guidByNetId;
std::unordered_map<uint32_t, double> NetCore::s_lastPosSaveAt;
std::string NetCore::s_localGuid;
bool NetCore::s_helloSent = false;

std::mutex NetCore::s_localMutex;
PlayerTransform NetCore::s_localXform = {};
bool NetCore::s_localXformValid = false;
uint64_t NetCore::s_localTick = 0;

std::mutex NetCore::s_appearanceMutex;
PlayerAppearance NetCore::s_localAppearance = {};
bool NetCore::s_localAppearanceValid = false;
bool NetCore::s_localAppearanceDirty = false;

std::mutex NetCore::s_outCombatMutex;
std::vector<NetCore::OutCombat> NetCore::s_outCombat;

std::mutex NetCore::s_eventsMutex;
std::vector<NetEvent> NetCore::s_events;

namespace
{
// Monotonic seconds for respawn scheduling (steady clock, unaffected by wall-clock changes).
double NowSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

namespace
{
// GNS is single-instance per process, so a free-function trampoline can forward to our
// static callback without threading a `this` pointer through.
void SteamNetConnectionStatusChangedTrampoline(SteamNetConnectionStatusChangedCallback_t* aInfo)
{
    NetCore::OnConnectionStatusChanged(aInfo);
}
} // namespace

bool NetCore::StartHost(uint16_t aPort)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_running.load())
    {
        Log::Warn("StartHost ignored: a net session is already running");
        return false;
    }

    // A previous worker may have self-exited (e.g. setup failure) leaving s_running=false
    // but s_thread still joinable. Assigning to a joinable std::thread calls
    // std::terminate(), so reap the finished thread first.
    if (s_thread.joinable())
    {
        s_thread.join();
    }

    s_mode = NetMode::Host;
    s_port = aPort;
    s_hostAddr.clear();
    s_connected.store(false);

    // Host is participant #1. Client ids are handed out above the reserved host id.
    s_localNetId.store(kLocalHostNetId);
    s_nextNetId = kLocalHostNetId + 1;
    s_connToNetId.clear();
    s_appearanceByNetId.clear();
    s_healthByNetId.clear();
    s_respawnDueAt.clear();
    s_guidByNetId.clear();
    s_lastPosSaveAt.clear();
    // Host tracks its own HP too (starts full).
    s_healthByNetId[kLocalHostNetId] = kMaxHealth;

    // Phase 4: bring up the host persistence store (loads any prior save + starts its flush
    // thread). The host binds its OWN player to its local GUID so its position/appearance/stats
    // persist too. The store lives entirely on the host; clients never start it.
    HostStore::Start(Identity::HostStoreFilePath());
    {
        const std::string& myGuid = Identity::LocalGuid();
        s_guidByNetId[kLocalHostNetId] = myGuid;
        HostStore::EnsureRecord(myGuid);
        // Restore the host's own saved stats into the authoritative broadcast on first tick (its
        // position/appearance are driven by the live game; stats are the persisted bit that matters
        // for the host itself). Nothing to send yet — no clients — HostBroadcastStats no-ops.
    }

    // Reset the per-session local-transform hand-off + event queue.
    {
        std::lock_guard<std::mutex> l(s_localMutex);
        s_localXformValid = false;
        s_localTick = 0;
        // Host never sends a PlayerHello (it already knows its own GUID); mark it sent so
        // SendLocalHello is a no-op on the host.
        s_localGuid = Identity::LocalGuid();
        s_helloSent = true;
    }
    {
        std::lock_guard<std::mutex> l(s_appearanceMutex);
        s_localAppearanceValid = false;
        s_localAppearanceDirty = false;
    }
    {
        std::lock_guard<std::mutex> l(s_outCombatMutex);
        s_outCombat.clear();
    }
    {
        std::lock_guard<std::mutex> l(s_eventsMutex);
        s_events.clear();
    }

    s_running.store(true);
    s_thread = std::thread(&NetCore::ThreadMain);
    Log::Info("Host worker thread started on port " + std::to_string(aPort));
    return true;
}

bool NetCore::StartClient(const std::string& aHost, uint16_t aPort)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_running.load())
    {
        Log::Warn("StartClient ignored: a net session is already running");
        return false;
    }

    // See StartHost: reap any finished-but-unjoined worker before reassigning s_thread.
    if (s_thread.joinable())
    {
        s_thread.join();
    }

    s_mode = NetMode::Client;
    s_port = aPort;
    s_hostAddr = aHost;
    s_connected.store(false);

    // Our own netId is assigned by the host; unknown until then.
    s_localNetId.store(0);
    s_connToNetId.clear();
    s_appearanceByNetId.clear();
    s_healthByNetId.clear();  // clients don't own HP, but keep the maps clean per session
    s_respawnDueAt.clear();
    s_guidByNetId.clear();
    s_lastPosSaveAt.clear();

    // Phase 4: the client sends its identity GUID as the first message after Connected. Reset the
    // per-session flag + publish our local GUID so SendLocalHello has it.
    {
        std::lock_guard<std::mutex> l(s_localMutex);
        s_localGuid = Identity::LocalGuid();
        s_helloSent = false;
    }

    {
        std::lock_guard<std::mutex> l(s_localMutex);
        s_localXformValid = false;
        s_localTick = 0;
    }
    {
        std::lock_guard<std::mutex> l(s_appearanceMutex);
        s_localAppearanceValid = false;
        s_localAppearanceDirty = false;
    }
    {
        std::lock_guard<std::mutex> l(s_outCombatMutex);
        s_outCombat.clear();
    }
    {
        std::lock_guard<std::mutex> l(s_eventsMutex);
        s_events.clear();
    }

    s_running.store(true);
    s_thread = std::thread(&NetCore::ThreadMain);
    Log::Info("Client worker thread started for " + aHost + ":" + std::to_string(aPort));
    return true;
}

bool NetCore::IsConnected()
{
    return s_connected.load();
}

uint32_t NetCore::LocalNetId()
{
    return s_localNetId.load();
}

void NetCore::PushLocalTransform(float px, float py, float pz, float yaw, float vx, float vy, float vz,
                                 uint8_t locoState)
{
    std::lock_guard<std::mutex> lock(s_localMutex);
    s_localXform.px = px;
    s_localXform.py = py;
    s_localXform.pz = pz;
    s_localXform.yaw = yaw;
    s_localXform.vx = vx;
    s_localXform.vy = vy;
    s_localXform.vz = vz;
    s_localXform.locoState = locoState;
    // netId + tick are stamped by the worker at send time.
    s_localXformValid = true;
}

void NetCore::PushLocalAppearance(uint8_t bodyGender, const uint64_t* aClothing)
{
    std::lock_guard<std::mutex> lock(s_appearanceMutex);

    // Detect an actual change so we don't spam an unchanged look every capture. First push is
    // always a change (valid was false).
    PlayerAppearance next{};
    next.netId = 0; // stamped by the worker at send time (host fills the real id)
    next.bodyGender = bodyGender;
    for (uint32_t i = 0; i < kAppearanceSlots; ++i)
    {
        next.clothing[i] = (aClothing != nullptr) ? aClothing[i] : 0;
    }

    const bool changed =
        !s_localAppearanceValid || next.bodyGender != s_localAppearance.bodyGender ||
        std::memcmp(next.clothing, s_localAppearance.clothing, sizeof(next.clothing)) != 0;

    if (!changed)
    {
        return;
    }

    s_localAppearance = next;
    s_localAppearanceValid = true;
    s_localAppearanceDirty = true; // pending (re)send on the next worker pump
}

void NetCore::SetLocalGuid(const std::string& aGuid)
{
    std::lock_guard<std::mutex> lock(s_localMutex);
    s_localGuid = aGuid;
}

void NetCore::PushLocalFire(uint64_t aWeaponTweakID, float aOx, float aOy, float aOz, float aDx, float aDy, float aDz)
{
    OutCombat oc{};
    oc.kind = OutCombat::Kind::Fire;
    oc.fire.shooterNetId = 0; // stamped by the worker at send time (client 0 -> host fills)
    oc.fire.weaponTweakID = aWeaponTweakID;
    oc.fire.ox = aOx;
    oc.fire.oy = aOy;
    oc.fire.oz = aOz;
    oc.fire.dx = aDx;
    oc.fire.dy = aDy;
    oc.fire.dz = aDz;

    std::lock_guard<std::mutex> lock(s_outCombatMutex);
    s_outCombat.push_back(oc);
}

void NetCore::PushLocalHit(uint32_t aVictimNetId, float aDamage)
{
    OutCombat oc{};
    oc.kind = OutCombat::Kind::Hit;
    oc.hit.shooterNetId = 0; // stamped by the worker at send time (client 0 -> host fills)
    oc.hit.victimNetId = aVictimNetId;
    oc.hit.damage = aDamage;

    std::lock_guard<std::mutex> lock(s_outCombatMutex);
    s_outCombat.push_back(oc);
}

void NetCore::DrainEvents(std::vector<NetEvent>& aOut)
{
    aOut.clear();
    std::lock_guard<std::mutex> lock(s_eventsMutex);
    aOut.swap(s_events); // hand the batch off and leave s_events empty
}

void NetCore::QueueEvent(const NetEvent& aEvent)
{
    std::lock_guard<std::mutex> lock(s_eventsMutex);
    s_events.push_back(aEvent);
}

void NetCore::Shutdown()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_running.load() && !s_thread.joinable())
    {
        return;
    }

    // Signal the worker to exit its loop, then wait for it. The worker owns all socket
    // handles and closes them before returning, so nothing is touched after the join.
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }

    s_connected.store(false);
    s_localNetId.store(0);
    s_mode = NetMode::Idle;
    Log::Info("Net core shut down");
}

void NetCore::ThreadMain()
{
    s_interface = SteamNetworkingSockets();
    if (s_interface == nullptr)
    {
        Log::Error("SteamNetworkingSockets() returned null; aborting net worker");
        s_running.store(false);
        return;
    }

    bool ok = false;
    if (s_mode == NetMode::Host)
    {
        ok = SetupHost(s_port);
    }
    else if (s_mode == NetMode::Client)
    {
        ok = SetupClient(s_hostAddr, s_port);
    }

    if (!ok)
    {
        s_running.store(false);
    }

    // Pump the connection until asked to stop. RunCallbacks() drives the
    // ConnectionStatusChanged callback on THIS thread, so all GNS access stays single-
    // threaded. Each pump: drive callbacks, drain inbound packets, flush our local
    // transform out.
    while (s_running.load())
    {
        s_interface->RunCallbacks();
        PollMessages();
        SendLocalHello();      // Phase 4 (client): send identity GUID once the socket is up
        SendLocalAppearance(); // once-per-change; cheap no-op when nothing is dirty
        SendLocalTransform();
        SendLocalCombat();     // Phase 3: flush queued fire/hit events
        ServiceRespawns();     // Phase 3 (host): fire any due respawns
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // ---- teardown (still on the worker thread) ----
    if (s_connection != k_HSteamNetConnection_Invalid)
    {
        s_interface->CloseConnection(s_connection, 0, "cyber.rest shutdown", false);
        s_connection = k_HSteamNetConnection_Invalid;
    }
    // Close every accepted client connection (host).
    for (const auto& [conn, netId] : s_connToNetId)
    {
        s_interface->CloseConnection(conn, 0, "cyber.rest shutdown", false);
    }
    s_connToNetId.clear();
    s_appearanceByNetId.clear();
    s_healthByNetId.clear();
    s_respawnDueAt.clear();
    s_guidByNetId.clear();
    s_lastPosSaveAt.clear();

    // Phase 4: flush + stop the host persistence store (no-op on the client — it was never started).
    if (s_mode == NetMode::Host)
    {
        HostStore::Stop();
    }

    if (s_pollGroup != k_HSteamNetPollGroup_Invalid)
    {
        s_interface->DestroyPollGroup(s_pollGroup);
        s_pollGroup = k_HSteamNetPollGroup_Invalid;
    }
    if (s_listenSocket != k_HSteamListenSocket_Invalid)
    {
        s_interface->CloseListenSocket(s_listenSocket);
        s_listenSocket = k_HSteamListenSocket_Invalid;
    }
    s_interface = nullptr;
}

bool NetCore::SetupHost(uint16_t aPort)
{
    SteamNetworkingIPAddr localAddr = {};
    localAddr.Clear();
    localAddr.m_port = aPort;

    SteamNetworkingConfigValue_t opt = {};
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void*>(&SteamNetConnectionStatusChangedTrampoline));

    s_listenSocket = s_interface->CreateListenSocketIP(localAddr, 1, &opt);
    if (s_listenSocket == k_HSteamListenSocket_Invalid)
    {
        Log::Error("CreateListenSocketIP failed on port " + std::to_string(aPort));
        return false;
    }

    s_pollGroup = s_interface->CreatePollGroup();
    if (s_pollGroup == k_HSteamNetPollGroup_Invalid)
    {
        Log::Error("CreatePollGroup failed");
        s_interface->CloseListenSocket(s_listenSocket);
        s_listenSocket = k_HSteamListenSocket_Invalid;
        return false;
    }

    // The host's session is usable immediately: it can render its own player and accept
    // clients. (Presence for remotes flows as they connect.)
    s_connected.store(true);
    Log::Info("Listen server up on port " + std::to_string(aPort) + ", awaiting clients");
    return true;
}

bool NetCore::SetupClient(const std::string& aHost, uint16_t aPort)
{
    const std::string connectString = aHost + ":" + std::to_string(aPort);

    SteamNetworkingIPAddr addr = {};
    if (!addr.ParseString(connectString.c_str()))
    {
        Log::Error("Failed to parse connect address \"" + connectString + "\"");
        return false;
    }

    SteamNetworkingConfigValue_t opt = {};
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void*>(&SteamNetConnectionStatusChangedTrampoline));

    s_connection = s_interface->ConnectByIPAddress(addr, 1, &opt);
    if (s_connection == k_HSteamNetConnection_Invalid)
    {
        Log::Error("ConnectByIPAddress failed for \"" + connectString + "\"");
        return false;
    }

    Log::Info("Connecting to " + connectString + " ...");
    return true;
}

void NetCore::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* aInfo)
{
    // Runs on the worker thread from inside RunCallbacks().
    const auto newState = aInfo->m_info.m_eState;

    switch (newState)
    {
    case k_ESteamNetworkingConnectionState_Connecting:
    {
        // On the host, an inbound connection starts in Connecting and we must accept it.
        // On the client we initiated the connect, so there's nothing to accept.
        if (s_mode == NetMode::Host)
        {
            if (s_interface->AcceptConnection(aInfo->m_hConn) != k_EResultOK)
            {
                s_interface->CloseConnection(aInfo->m_hConn, 0, nullptr, false);
                Log::Warn("Failed to accept an incoming connection (already closed?)");
                break;
            }
            if (!s_interface->SetConnectionPollGroup(aInfo->m_hConn, s_pollGroup))
            {
                s_interface->CloseConnection(aInfo->m_hConn, 0, nullptr, false);
                Log::Warn("Failed to assign accepted connection to poll group");
                break;
            }
            Log::Info("Accepted incoming connection");
        }
        break;
    }

    case k_ESteamNetworkingConnectionState_Connected:
    {
        Log::Info("Connection established (state -> Connected)");
        if (s_mode == NetMode::Host)
        {
            // The accepted connection is now fully usable: assign its netId, tell it about
            // the existing roster, and announce it to everyone else.
            HostOnClientConnected(aInfo->m_hConn);
        }
        else if (s_mode == NetMode::Client)
        {
            // Client's socket to the host is up; presence begins flowing. The host will
            // send us a PlayerJoin for our own id (see HostOnClientConnected) which we use
            // to learn our LocalNetId and to ignore our own echoes.
            s_connected.store(true);
        }
        break;
    }

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
    {
        Log::Warn(std::string("Connection closed (state ") + std::to_string(static_cast<int>(newState)) +
                  "): " + aInfo->m_info.m_szEndDebug);

        if (s_mode == NetMode::Host)
        {
            // A client dropped: announce its departure and forget it.
            HostOnClientGone(aInfo->m_hConn);
        }

        s_interface->CloseConnection(aInfo->m_hConn, 0, nullptr, false);
        if (aInfo->m_hConn == s_connection)
        {
            // Client: we lost the host.
            s_connection = k_HSteamNetConnection_Invalid;
            s_connected.store(false);
        }
        break;
    }

    default:
        // Other states (None, FindingRoute, FinWait, Linger, Dead) need no action here.
        break;
    }
}

// ---- host presence bookkeeping --------------------------------------------------------

void NetCore::HostOnClientConnected(HSteamNetConnection aConn)
{
    if (s_connToNetId.count(aConn) != 0)
    {
        return; // already registered (defensive; Connected can fire once per conn)
    }

    const uint32_t newId = s_nextNetId++;
    Log::Info("Assigned netId " + std::to_string(newId) + " to a client");

    // Phase 3: the newcomer starts at full HP on the authoritative host.
    HostEnsureHealth(newId);

    // ORDERING MATTERS: GNS reliable messages are delivered in order per connection, and the
    // client treats the FIRST PlayerJoin it ever receives as "here is your own netId". So we
    // must send the newcomer its own id BEFORE any roster entry.

    // 1) Tell the newcomer its OWN netId first. The client keys off this to learn LocalNetId
    //    and to ignore echoes of its own player (it does NOT spawn a puppet for this one).
    {
        const PlayerJoin self{newId};
        const auto buf = Serialize(self);
        HostSendTo(aConn, buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);
    }

    // 2) Now the roster: the host itself, plus every already-connected client. (s_connToNetId
    //    does not yet contain the newcomer — we insert it after this loop — so no self-echo.)
    //    Each player's Join is immediately followed by its Appearance (if the host has one on
    //    file) so the newcomer spawns the puppet AND dresses it in one ordered burst. Both are
    //    Reliable so the look can't be lost. HostSendAppearanceOnFile is a no-op if unknown
    //    (the appearance will arrive later and be relayed then).
    {
        const PlayerJoin hostJoin{kLocalHostNetId};
        const auto buf = Serialize(hostJoin);
        HostSendTo(aConn, buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);
    }
    HostSendAppearanceOnFile(aConn, kLocalHostNetId);
    // Phase 3: also send the host's current HP so the newcomer renders the right health bar.
    {
        const PlayerHealth h{kLocalHostNetId, s_healthByNetId[kLocalHostNetId]};
        const auto buf = Serialize(h);
        HostSendTo(aConn, buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);
    }
    // Phase 4: the host's own persistent stats (keyed by its GUID) so the newcomer's scoreboard
    // shows them. GuidForNetId(kLocalHostNetId) was bound in StartHost.
    {
        uint32_t hk = 0, hd = 0;
        const std::string hostGuid = GuidForNetId(kLocalHostNetId);
        if (!hostGuid.empty())
        {
            HostStore::GetStats(hostGuid, hk, hd);
        }
        const PlayerStats st{kLocalHostNetId, hk, hd};
        const auto buf = Serialize(st);
        HostSendTo(aConn, buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);
    }
    for (const auto& [conn, netId] : s_connToNetId)
    {
        const PlayerJoin j{netId};
        const auto buf = Serialize(j);
        HostSendTo(aConn, buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);
        HostSendAppearanceOnFile(aConn, netId);
        // Phase 3: current HP of each already-present player.
        HostEnsureHealth(netId);
        const PlayerHealth h{netId, s_healthByNetId[netId]};
        const auto hbuf = Serialize(h);
        HostSendTo(aConn, hbuf.data(), static_cast<uint32_t>(hbuf.size()), k_nSteamNetworkingSend_Reliable);
        // Phase 4: each already-present player's persistent stats (0/0 if not yet identified).
        uint32_t ck = 0, cd = 0;
        const std::string cguid = GuidForNetId(netId);
        if (!cguid.empty())
        {
            HostStore::GetStats(cguid, ck, cd);
        }
        const PlayerStats cst{netId, ck, cd};
        const auto sbuf = Serialize(cst);
        HostSendTo(aConn, sbuf.data(), static_cast<uint32_t>(sbuf.size()), k_nSteamNetworkingSend_Reliable);
    }

    // 3) Announce the newcomer to everyone else already present (before we add it to the map,
    //    so HostRelayToOthers naturally excludes the newcomer itself).
    {
        const PlayerJoin j{newId};
        const auto buf = Serialize(j);
        HostRelayToOthers(aConn, buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);
    }
    // Phase 3: announce the newcomer's (full) HP to everyone else too.
    {
        const PlayerHealth h{newId, s_healthByNetId[newId]};
        const auto buf = Serialize(h);
        HostRelayToOthers(aConn, buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);
    }

    // Register the mapping now that the announcements are out.
    s_connToNetId[aConn] = newId;

    // 4) Locally (host is a participant too) surface the newcomer as a Join event so the
    //    host's own game thread spawns a puppet for it.
    NetEvent ev{};
    ev.kind = NetEvent::Kind::Join;
    ev.netId = newId;
    QueueEvent(ev);
}

void NetCore::HostOnClientGone(HSteamNetConnection aConn)
{
    const auto it = s_connToNetId.find(aConn);
    if (it == s_connToNetId.end())
    {
        return; // never fully connected / already removed
    }
    const uint32_t goneId = it->second;
    s_connToNetId.erase(it);
    s_appearanceByNetId.erase(goneId); // forget its look so a later newcomer isn't sent a ghost
    s_healthByNetId.erase(goneId);     // Phase 3: forget its HP + any pending respawn
    s_respawnDueAt.erase(goneId);

    // Phase 4: the leaving player's record is already up to date in the store (position throttled,
    // appearance + stats on change). Flush to disk now so a host crash after this keeps it, then
    // forget the netId<->GUID binding + its save throttle.
    s_guidByNetId.erase(goneId);
    s_lastPosSaveAt.erase(goneId);
    HostStore::FlushNow();

    // Tell the remaining clients it left (Reliable: a lost leave leaks a ghost puppet).
    const PlayerLeave leave{goneId};
    const auto buf = Serialize(leave);
    HostRelayToOthers(aConn, buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);

    // And surface it to the host's own game thread.
    NetEvent ev{};
    ev.kind = NetEvent::Kind::Leave;
    ev.netId = goneId;
    QueueEvent(ev);
    Log::Info("Client netId " + std::to_string(goneId) + " left");
}

void NetCore::HostHandleTransform(HSteamNetConnection aFrom, const PlayerTransform& aXform)
{
    const auto it = s_connToNetId.find(aFrom);
    if (it == s_connToNetId.end())
    {
        return; // transform from a connection we don't recognise; drop it
    }
    const uint32_t senderId = it->second;

    // Stamp the authoritative sender netId (a client cannot spoof another's id) and relay
    // to the OTHER clients only. UnreliableNoDelay: a stale movement sample is worthless, so
    // drop it rather than queue it (avoids head-of-line blocking under packet loss).
    PlayerTransform relayed = aXform;
    relayed.netId = senderId;
    const auto buf = Serialize(relayed);
    HostRelayToOthers(aFrom, buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_UnreliableNoDelay);

    // The host is a participant, so surface the client's movement to its own game thread.
    NetEvent ev{};
    ev.kind = NetEvent::Kind::Transform;
    ev.netId = senderId;
    ev.xform = relayed;
    QueueEvent(ev);

    // Phase 4: persist this player's position, throttled to kPositionSaveIntervalSeconds so we don't
    // touch the store on every movement packet. The store's own flush thread does the actual disk
    // write; this only updates the in-memory record.
    const std::string guid = GuidForNetId(senderId);
    if (!guid.empty())
    {
        const double now = NowSeconds();
        const auto it2 = s_lastPosSaveAt.find(senderId);
        if (it2 == s_lastPosSaveAt.end() || (now - it2->second) >= kPositionSaveIntervalSeconds)
        {
            s_lastPosSaveAt[senderId] = now;
            HostStore::UpsertPosition(guid, relayed.px, relayed.py, relayed.pz, relayed.yaw);
        }
    }
}

void NetCore::HostHandleAppearance(HSteamNetConnection aFrom, const PlayerAppearance& aAppear)
{
    const auto it = s_connToNetId.find(aFrom);
    if (it == s_connToNetId.end())
    {
        return; // appearance from a connection we don't recognise; drop it
    }
    const uint32_t senderId = it->second;

    // Stamp the authoritative sender netId (anti-spoof, same rule as transforms), store the
    // latest look so we can replay it to future newcomers, and relay it to the OTHER clients.
    // Reliable: an appearance is low-frequency and a lost one leaves a wrongly-dressed puppet.
    PlayerAppearance relayed = aAppear;
    relayed.netId = senderId;
    s_appearanceByNetId[senderId] = relayed;

    const auto buf = Serialize(relayed);
    HostRelayToOthers(aFrom, buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);

    // Surface to the host's own game thread so the host re-skins that puppet too.
    NetEvent ev{};
    ev.kind = NetEvent::Kind::Appearance;
    ev.netId = senderId;
    ev.appear = relayed;
    QueueEvent(ev);

    // Phase 4: persist this player's look under their GUID (in-memory; the store flushes later).
    const std::string guid = GuidForNetId(senderId);
    if (!guid.empty())
    {
        HostStore::UpsertAppearance(guid, relayed.bodyGender, relayed.clothing);
    }
}

// ---- Phase 3 host combat -------------------------------------------------------------

void NetCore::HostEnsureHealth(uint32_t aNetId)
{
    if (s_healthByNetId.find(aNetId) == s_healthByNetId.end())
    {
        s_healthByNetId[aNetId] = kMaxHealth;
    }
}

void NetCore::HostBroadcast(const void* aData, uint32_t aSize, int aSendFlags)
{
    for (const auto& [conn, netId] : s_connToNetId)
    {
        HostSendTo(conn, aData, aSize, aSendFlags);
    }
}

void NetCore::HostHandleFire(HSteamNetConnection aFrom, const PlayerFire& aFire)
{
    const auto it = s_connToNetId.find(aFrom);
    if (it == s_connToNetId.end())
    {
        return; // fire from a connection we don't recognise; drop it
    }
    const uint32_t senderId = it->second;

    // Cosmetic only: stamp the authoritative shooter id (anti-spoof) and relay to the OTHER
    // clients so they can play a tracer/muzzle from that puppet. UnreliableNoDelay — a lost
    // tracer is harmless and we never want it queued behind newer shots.
    PlayerFire relayed = aFire;
    relayed.shooterNetId = senderId;
    const auto buf = Serialize(relayed);
    HostRelayToOthers(aFrom, buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_UnreliableNoDelay);

    // Surface to the host's own game thread so the host also sees the tracer.
    NetEvent ev{};
    ev.kind = NetEvent::Kind::Fire;
    ev.netId = senderId;
    ev.fire = relayed;
    QueueEvent(ev);
}

void NetCore::HostHandleHit(HSteamNetConnection aFrom, const PlayerHit& aHit)
{
    const auto it = s_connToNetId.find(aFrom);
    if (it == s_connToNetId.end())
    {
        return; // hit claim from a connection we don't recognise; drop it
    }
    const uint32_t shooterId = it->second; // authoritative sender (ignore any spoofed field)

    HostApplyDamage(aHit.victimNetId, aHit.damage, shooterId);
}

void NetCore::HostApplyDamage(uint32_t aVictimNetId, float aDamage, uint32_t aSourceId)
{
    // ---- loose host-authoritative sanity checks ----
    // Unknown victim (not the host and not a connected client) -> ignore.
    const bool victimIsHost = (aVictimNetId == kLocalHostNetId);
    bool victimIsClient = false;
    for (const auto& [conn, netId] : s_connToNetId)
    {
        if (netId == aVictimNetId)
        {
            victimIsClient = true;
            break;
        }
    }
    if (!victimIsHost && !victimIsClient)
    {
        return;
    }
    // No self-damage from a reported hit (a shot can't legitimately claim to hit its own shooter).
    if (aVictimNetId == aSourceId)
    {
        return;
    }

    HostEnsureHealth(aVictimNetId);
    float& hp = s_healthByNetId[aVictimNetId];
    if (hp <= 0.0f)
    {
        return; // already dead / awaiting respawn; ignore further hits
    }

    // Clamp the claimed damage into [0, kMaxHitDamage] and apply.
    float dmg = aDamage;
    if (dmg < 0.0f)
    {
        dmg = 0.0f;
    }
    if (dmg > kMaxHitDamage)
    {
        dmg = kMaxHitDamage;
    }
    hp -= dmg;
    if (hp < 0.0f)
    {
        hp = 0.0f;
    }
    Log::Info("netId " + std::to_string(aSourceId) + " hit netId " + std::to_string(aVictimNetId) + " for " +
              std::to_string(dmg) + " -> hp " + std::to_string(hp));

    // Broadcast the authoritative HP to everyone + surface locally (this is the ONLY thing that
    // moves a health bar). Reliable: a lost health update desyncs the bar.
    {
        const PlayerHealth h{aVictimNetId, hp};
        const auto buf = Serialize(h);
        HostBroadcast(buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);
        NetEvent ev{};
        ev.kind = NetEvent::Kind::Health;
        ev.netId = aVictimNetId;
        ev.hp = hp;
        QueueEvent(ev);
    }

    // Death: broadcast + surface, then schedule a respawn.
    if (hp <= 0.0f)
    {
        const PlayerDeath d{aVictimNetId};
        const auto buf = Serialize(d);
        HostBroadcast(buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);
        NetEvent ev{};
        ev.kind = NetEvent::Kind::Death;
        ev.netId = aVictimNetId;
        QueueEvent(ev);

        s_respawnDueAt[aVictimNetId] = NowSeconds() + kRespawnDelaySeconds;
        Log::Info("netId " + std::to_string(aVictimNetId) + " died; respawn in " +
                  std::to_string(kRespawnDelaySeconds) + "s");

        // Phase 4: persist the K/D. The victim gets a death; the shooter gets a kill (unless the
        // death was self-inflicted / sourceless — aSourceId == victim can't happen here since
        // HostApplyDamage already rejects self-hits, but guard anyway). Then broadcast the updated
        // stats for both players so every scoreboard converges.
        const std::string victimGuid = GuidForNetId(aVictimNetId);
        if (!victimGuid.empty())
        {
            HostStore::RecordDeath(victimGuid);
            uint32_t k = 0, dcount = 0;
            HostStore::GetStats(victimGuid, k, dcount);
            HostBroadcastStats(aVictimNetId, k, dcount);
        }
        if (aSourceId != aVictimNetId)
        {
            const std::string killerGuid = GuidForNetId(aSourceId);
            if (!killerGuid.empty())
            {
                HostStore::RecordKill(killerGuid);
                uint32_t k = 0, dcount = 0;
                HostStore::GetStats(killerGuid, k, dcount);
                HostBroadcastStats(aSourceId, k, dcount);
            }
        }
    }
}

void NetCore::ServiceRespawns()
{
    if (s_mode != NetMode::Host || s_respawnDueAt.empty())
    {
        return;
    }
    const double now = NowSeconds();

    // Collect due respawns first (we mutate s_respawnDueAt while iterating otherwise).
    std::vector<uint32_t> due;
    for (const auto& [netId, at] : s_respawnDueAt)
    {
        if (now >= at)
        {
            due.push_back(netId);
        }
    }

    for (uint32_t netId : due)
    {
        s_respawnDueAt.erase(netId);

        // Only respawn a player still in the session.
        const bool isHost = (netId == kLocalHostNetId);
        bool isClient = false;
        for (const auto& [conn, id] : s_connToNetId)
        {
            if (id == netId)
            {
                isClient = true;
                break;
            }
        }
        if (!isHost && !isClient)
        {
            continue;
        }

        // Restore full HP (authoritative) and broadcast it.
        s_healthByNetId[netId] = kMaxHealth;
        {
            const PlayerHealth h{netId, kMaxHealth};
            const auto buf = Serialize(h);
            HostBroadcast(buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);
            NetEvent ev{};
            ev.kind = NetEvent::Kind::Health;
            ev.netId = netId;
            ev.hp = kMaxHealth;
            QueueEvent(ev);
        }

        // Respawn position. Phase 3 minimal: respawn in place at the last-known transform if we
        // have one, else at origin. The receiver decides the actual placement; for the host's own
        // player and remote puppets alike the reds side re-places them. We send 0,0,0 and let the
        // reds handler use the puppet's current/last position when the point is origin.
        // (IN-GAME ASSUMPTION: a proper spawn-point system is out of Phase-3 scope.)
        const PlayerRespawn r{netId, 0.0f, 0.0f, 0.0f};
        const auto buf = Serialize(r);
        HostBroadcast(buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);
        NetEvent ev{};
        ev.kind = NetEvent::Kind::Respawn;
        ev.netId = netId;
        ev.respawn = r;
        QueueEvent(ev);
        Log::Info("Respawned netId " + std::to_string(netId));
    }
}

// ---- Phase 4 host persistence -------------------------------------------------------------

std::string NetCore::GuidForNetId(uint32_t aNetId)
{
    const auto it = s_guidByNetId.find(aNetId);
    if (it == s_guidByNetId.end())
    {
        return std::string();
    }
    return it->second;
}

void NetCore::HostBroadcastStats(uint32_t aNetId, uint32_t aKills, uint32_t aDeaths)
{
    // Reliable: stats are low-frequency and a lost one desyncs a scoreboard. Broadcast to every
    // client + surface locally so the host's own game thread updates too.
    const PlayerStats st{aNetId, aKills, aDeaths};
    const auto buf = Serialize(st);
    HostBroadcast(buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);

    NetEvent ev{};
    ev.kind = NetEvent::Kind::Stats;
    ev.netId = aNetId;
    ev.stats = st;
    QueueEvent(ev);
}

void NetCore::HostHandleHello(HSteamNetConnection aFrom, const PlayerHello& aHello)
{
    const auto it = s_connToNetId.find(aFrom);
    if (it == s_connToNetId.end())
    {
        return; // hello from a connection we don't recognise yet; drop it
    }
    const uint32_t senderId = it->second;

    if (aHello.protocolVersion != kProtocolVersion)
    {
        Log::Warn("Client netId " + std::to_string(senderId) + " protocol version " +
                  std::to_string(aHello.protocolVersion) + " != host " + std::to_string(kProtocolVersion) +
                  " (continuing; Phase 4 does not hard-reject)");
    }

    // The GUID buffer is fixed-length + always NUL-terminated within the buffer by the sender, but
    // treat it defensively: bound the length to the buffer so a non-terminated buffer can't overrun.
    size_t glen = 0;
    while (glen < kGuidLen && aHello.guid[glen] != '\0')
    {
        ++glen;
    }
    std::string guid(aHello.guid, glen);
    if (guid.empty())
    {
        Log::Warn("Client netId " + std::to_string(senderId) + " sent an empty GUID; ignoring hello");
        return;
    }

    // Bind netId <-> GUID for this session.
    s_guidByNetId[senderId] = guid;
    const bool isNew = HostStore::EnsureRecord(guid);
    Log::Info("netId " + std::to_string(senderId) + " identified as GUID " + guid +
              (isNew ? " (new player)" : " (returning player)"));

    // Restore the returning player's saved state and send it back to THEM so their client applies
    // it (position -> teleport local V; stats -> scoreboard). Appearance is already replayed by the
    // normal appearance-on-file path from other players' perspective; here we also seed the host's
    // OWN cache so the returning player's puppet (on other clients) can be dressed even before they
    // re-send their look. All Reliable.
    PlayerRecord rec;
    if (HostStore::Get(guid, rec))
    {
        // 1) Stats: tell everyone this player's persistent K/D (and the returning player themselves).
        HostBroadcastStats(senderId, rec.kills, rec.deaths);

        // 2) Saved appearance: seed the host's relay cache + fan it out so puppets are dressed from
        //    the persisted look immediately (the player will also re-send it, which just refreshes).
        if (rec.hasAppearance)
        {
            PlayerAppearance ap{};
            ap.netId = senderId;
            ap.bodyGender = rec.bodyGender;
            for (uint32_t i = 0; i < kAppearanceSlots; ++i)
            {
                ap.clothing[i] = rec.clothing[i];
            }
            s_appearanceByNetId[senderId] = ap;
            const auto buf = Serialize(ap);
            // To everyone else (so their puppets get the saved look) AND surface locally (host puppet).
            HostRelayToOthers(aFrom, buf.data(), static_cast<uint32_t>(buf.size()),
                              k_nSteamNetworkingSend_Reliable);
            NetEvent aev{};
            aev.kind = NetEvent::Kind::Appearance;
            aev.netId = senderId;
            aev.appear = ap;
            QueueEvent(aev);
        }

        // 3) Saved position: send a RestorePos to the returning player so their client teleports the
        //    local V to where they last were. Only if we actually have a saved position (else the
        //    player just spawns wherever the game put them).
        if (rec.hasPosition)
        {
            const PlayerRespawn r{senderId, rec.px, rec.py, rec.pz};
            const auto buf = Serialize(r);
            // Sent ONLY to the returning player (it's their local V we want to move). We reuse the
            // PlayerRespawn wire struct but a fresh MsgType so the receiver teleports the LOCAL V
            // rather than treating it as a combat respawn.
            std::vector<uint8_t> restoreBuf(1 + sizeof(PlayerRespawn));
            restoreBuf[0] = static_cast<uint8_t>(MsgType::PlayerRestorePos);
            std::memcpy(restoreBuf.data() + 1, &r, sizeof(PlayerRespawn));
            HostSendTo(aFrom, restoreBuf.data(), static_cast<uint32_t>(restoreBuf.size()),
                       k_nSteamNetworkingSend_Reliable);
            Log::Info("Sent saved position to returning netId " + std::to_string(senderId));
        }
    }
}

void NetCore::HostSendAppearanceOnFile(HSteamNetConnection aConn, uint32_t aNetId)
{
    const auto it = s_appearanceByNetId.find(aNetId);
    if (it == s_appearanceByNetId.end())
    {
        return; // no appearance known yet; it will be relayed once the player sends it
    }
    const auto buf = Serialize(it->second);
    HostSendTo(aConn, buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable);
}

void NetCore::HostRelayToOthers(HSteamNetConnection aExcept, const void* aData, uint32_t aSize, int aSendFlags)
{
    for (const auto& [conn, netId] : s_connToNetId)
    {
        if (conn == aExcept)
        {
            continue;
        }
        HostSendTo(conn, aData, aSize, aSendFlags);
    }
}

void NetCore::HostSendTo(HSteamNetConnection aConn, const void* aData, uint32_t aSize, int aSendFlags)
{
    s_interface->SendMessageToConnection(aConn, aData, aSize, aSendFlags, nullptr);
}

// ---- receive + local send -------------------------------------------------------------

void NetCore::PollMessages()
{
    SteamNetworkingMessage_t* messages[32] = {};

    int received = 0;
    if (s_mode == NetMode::Host)
    {
        received = s_interface->ReceiveMessagesOnPollGroup(s_pollGroup, messages, 32);
    }
    else if (s_mode == NetMode::Client && s_connection != k_HSteamNetConnection_Invalid)
    {
        received = s_interface->ReceiveMessagesOnConnection(s_connection, messages, 32);
    }

    if (received <= 0)
    {
        return; // 0 = nothing waiting, <0 = connection gone (state callback handles it)
    }

    for (int i = 0; i < received; ++i)
    {
        SteamNetworkingMessage_t* msg = messages[i];
        const void* data = msg->m_pData;
        const size_t size = msg->m_cbSize;
        const MsgType type = PeekType(data, size);

        if (s_mode == NetMode::Host)
        {
            // A client sends the host its own transform (frequent) and its appearance (once /
            // on change). The host relays + surfaces each.
            if (type == MsgType::PlayerTransform)
            {
                PlayerTransform xform{};
                if (Deserialize(data, size, xform))
                {
                    HostHandleTransform(msg->m_conn, xform);
                }
            }
            else if (type == MsgType::PlayerAppearance)
            {
                PlayerAppearance appear{};
                if (Deserialize(data, size, appear))
                {
                    HostHandleAppearance(msg->m_conn, appear);
                }
            }
            else if (type == MsgType::PlayerFire)
            {
                PlayerFire fire{};
                if (Deserialize(data, size, fire))
                {
                    HostHandleFire(msg->m_conn, fire);
                }
            }
            else if (type == MsgType::PlayerHit)
            {
                PlayerHit hit{};
                if (Deserialize(data, size, hit))
                {
                    HostHandleHit(msg->m_conn, hit);
                }
            }
            else if (type == MsgType::PlayerHello)
            {
                // Phase 4: client identity. Bind netId<->GUID + restore saved state.
                PlayerHello hello{};
                if (Deserialize(data, size, hello))
                {
                    HostHandleHello(msg->m_conn, hello);
                }
            }
            else
            {
                Log::Warn("Host received unexpected message type " +
                          std::to_string(static_cast<int>(type)));
            }
        }
        else // client
        {
            switch (type)
            {
            case MsgType::PlayerJoin:
            {
                PlayerJoin j{};
                if (Deserialize(data, size, j))
                {
                    // The FIRST join whose id we don't yet own, and while we have no
                    // LocalNetId, is (by protocol) our own id from the host. Learn it and
                    // do NOT spawn a puppet for ourselves.
                    if (s_localNetId.load() == 0)
                    {
                        s_localNetId.store(j.netId);
                        Log::Info("Learned own netId " + std::to_string(j.netId) + " from host");
                        break;
                    }
                    if (j.netId == s_localNetId.load())
                    {
                        break; // never spawn a puppet for our own player
                    }
                    NetEvent ev{};
                    ev.kind = NetEvent::Kind::Join;
                    ev.netId = j.netId;
                    QueueEvent(ev);
                }
                break;
            }
            case MsgType::PlayerLeave:
            {
                PlayerLeave l{};
                if (Deserialize(data, size, l))
                {
                    NetEvent ev{};
                    ev.kind = NetEvent::Kind::Leave;
                    ev.netId = l.netId;
                    QueueEvent(ev);
                }
                break;
            }
            case MsgType::PlayerTransform:
            {
                PlayerTransform xform{};
                if (Deserialize(data, size, xform))
                {
                    if (xform.netId == s_localNetId.load())
                    {
                        break; // ignore any echo of our own movement
                    }
                    NetEvent ev{};
                    ev.kind = NetEvent::Kind::Transform;
                    ev.netId = xform.netId;
                    ev.xform = xform;
                    QueueEvent(ev);
                }
                break;
            }
            case MsgType::PlayerAppearance:
            {
                PlayerAppearance appear{};
                if (Deserialize(data, size, appear))
                {
                    if (appear.netId == s_localNetId.load())
                    {
                        break; // ignore any echo of our own look
                    }
                    NetEvent ev{};
                    ev.kind = NetEvent::Kind::Appearance;
                    ev.netId = appear.netId;
                    ev.appear = appear;
                    QueueEvent(ev);
                }
                break;
            }
            case MsgType::PlayerFire:
            {
                PlayerFire fire{};
                if (Deserialize(data, size, fire))
                {
                    if (fire.shooterNetId == s_localNetId.load())
                    {
                        break; // don't re-play our own muzzle (we already fired locally)
                    }
                    NetEvent ev{};
                    ev.kind = NetEvent::Kind::Fire;
                    ev.netId = fire.shooterNetId;
                    ev.fire = fire;
                    QueueEvent(ev);
                }
                break;
            }
            case MsgType::PlayerHealth:
            {
                // Authoritative HP from the host — applies to OUR player too (this is how the
                // local V takes networked damage), so do NOT filter our own netId here.
                PlayerHealth h{};
                if (Deserialize(data, size, h))
                {
                    NetEvent ev{};
                    ev.kind = NetEvent::Kind::Health;
                    ev.netId = h.netId;
                    ev.hp = h.hp;
                    QueueEvent(ev);
                }
                break;
            }
            case MsgType::PlayerDeath:
            {
                PlayerDeath d{};
                if (Deserialize(data, size, d))
                {
                    NetEvent ev{};
                    ev.kind = NetEvent::Kind::Death;
                    ev.netId = d.netId;
                    QueueEvent(ev);
                }
                break;
            }
            case MsgType::PlayerRespawn:
            {
                PlayerRespawn r{};
                if (Deserialize(data, size, r))
                {
                    NetEvent ev{};
                    ev.kind = NetEvent::Kind::Respawn;
                    ev.netId = r.netId;
                    ev.respawn = r;
                    QueueEvent(ev);
                }
                break;
            }
            case MsgType::PlayerStats:
            {
                // Phase 4: authoritative persistent K/D from the host — applies to any netId
                // (including our own, for our own scoreboard entry), so do NOT filter our id.
                PlayerStats st{};
                if (Deserialize(data, size, st))
                {
                    NetEvent ev{};
                    ev.kind = NetEvent::Kind::Stats;
                    ev.netId = st.netId;
                    ev.stats = st;
                    QueueEvent(ev);
                }
                break;
            }
            case MsgType::PlayerRestorePos:
            {
                // Phase 4: host tells US where our saved position is so reds teleports the LOCAL V.
                // Same wire layout as PlayerRespawn; surfaced as a distinct RestorePos event.
                PlayerRespawn r{};
                if (Deserialize(data, size, r))
                {
                    NetEvent ev{};
                    ev.kind = NetEvent::Kind::RestorePos;
                    ev.netId = r.netId;
                    ev.respawn = r;
                    QueueEvent(ev);
                }
                break;
            }
            default:
                Log::Warn("Client received unexpected message type " +
                          std::to_string(static_cast<int>(type)));
                break;
            }
        }

        msg->Release();
    }
}

void NetCore::SendLocalHello()
{
    // CLIENT ONLY, once per session: as soon as our socket is Connected, send our stable identity
    // GUID as a PlayerHello. This is the first thing the host hears from us, so it can restore our
    // saved state before any transform/appearance arrives. The host has s_helloSent == true (set in
    // StartHost) so this is a no-op on the host.
    if (s_mode != NetMode::Client)
    {
        return;
    }
    if (s_connection == k_HSteamNetConnection_Invalid || !s_connected.load())
    {
        return; // not connected yet; retry next pump
    }

    PlayerHello hello{};
    {
        std::lock_guard<std::mutex> lock(s_localMutex);
        if (s_helloSent)
        {
            return; // already sent this session
        }
        hello.netId = 0; // unused on the wire
        hello.protocolVersion = kProtocolVersion;
        // Copy the GUID into the fixed buffer, always NUL-terminated within it.
        std::memset(hello.guid, 0, sizeof(hello.guid));
        const size_t n = (s_localGuid.size() < (kGuidLen - 1)) ? s_localGuid.size() : (kGuidLen - 1);
        std::memcpy(hello.guid, s_localGuid.data(), n);
        s_helloSent = true;
    }

    const auto buf = Serialize(hello);
    s_interface->SendMessageToConnection(s_connection, buf.data(), static_cast<uint32_t>(buf.size()),
                                         k_nSteamNetworkingSend_Reliable, nullptr);
    Log::Info("Sent PlayerHello (identity GUID) to host");
}

void NetCore::SendLocalTransform()
{
    // Snapshot the latest local sample under the lock, then send outside it.
    PlayerTransform xform{};
    {
        std::lock_guard<std::mutex> lock(s_localMutex);
        if (!s_localXformValid)
        {
            return; // game thread hasn't pushed a sample yet this session
        }
        xform = s_localXform;
        xform.tick = ++s_localTick;
    }

    if (s_mode == NetMode::Client)
    {
        if (s_connection == k_HSteamNetConnection_Invalid)
        {
            return;
        }
        // netId 0 -> host stamps our real id on relay (clients can't spoof).
        xform.netId = 0;
        const auto buf = Serialize(xform);
        s_interface->SendMessageToConnection(s_connection, buf.data(), static_cast<uint32_t>(buf.size()),
                                             k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
    }
    else if (s_mode == NetMode::Host)
    {
        // Phase 4: persist the HOST's own position too (throttled), keyed by its GUID. Done before
        // the "no clients" early-out so the host's position saves even when playing solo.
        {
            const std::string guid = GuidForNetId(kLocalHostNetId);
            if (!guid.empty())
            {
                const double now = NowSeconds();
                const auto it = s_lastPosSaveAt.find(kLocalHostNetId);
                if (it == s_lastPosSaveAt.end() || (now - it->second) >= kPositionSaveIntervalSeconds)
                {
                    s_lastPosSaveAt[kLocalHostNetId] = now;
                    HostStore::UpsertPosition(guid, xform.px, xform.py, xform.pz, xform.yaw);
                }
            }
        }

        // The host originates its own transform: stamp its own id and relay to every
        // client. (There is no "sender" to exclude.)
        if (s_connToNetId.empty())
        {
            return; // nobody to send to
        }
        xform.netId = kLocalHostNetId;
        const auto buf = Serialize(xform);
        for (const auto& [conn, netId] : s_connToNetId)
        {
            s_interface->SendMessageToConnection(conn, buf.data(), static_cast<uint32_t>(buf.size()),
                                                 k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
        }
    }
}

void NetCore::SendLocalAppearance()
{
    // Snapshot only if the game thread has pushed a look AND it's pending a (re)send. Clearing
    // the dirty flag here means we send each distinct look exactly once (per change).
    PlayerAppearance appear{};
    {
        std::lock_guard<std::mutex> lock(s_appearanceMutex);
        if (!s_localAppearanceValid || !s_localAppearanceDirty)
        {
            return;
        }
        appear = s_localAppearance;
        s_localAppearanceDirty = false;
    }

    if (s_mode == NetMode::Client)
    {
        if (s_connection == k_HSteamNetConnection_Invalid)
        {
            // Not connected yet; re-arm so we resend once the socket is up (we cleared dirty).
            std::lock_guard<std::mutex> lock(s_appearanceMutex);
            s_localAppearanceDirty = true;
            return;
        }
        // netId 0 -> host stamps our real id on relay (clients can't spoof). Reliable: a lost
        // appearance leaves a wrongly-dressed puppet, and it's low-frequency, so pay for it.
        appear.netId = 0;
        const auto buf = Serialize(appear);
        s_interface->SendMessageToConnection(s_connection, buf.data(), static_cast<uint32_t>(buf.size()),
                                             k_nSteamNetworkingSend_Reliable, nullptr);
    }
    else if (s_mode == NetMode::Host)
    {
        // The host owns its own appearance: stamp its id and record it so any FUTURE newcomer
        // gets it via HostSendAppearanceOnFile (store even with zero clients right now).
        appear.netId = kLocalHostNetId;
        s_appearanceByNetId[kLocalHostNetId] = appear;

        // Phase 4: persist the host's own look under its GUID (in-memory; store flushes later).
        {
            const std::string guid = GuidForNetId(kLocalHostNetId);
            if (!guid.empty())
            {
                HostStore::UpsertAppearance(guid, appear.bodyGender, appear.clothing);
            }
        }

        const auto buf = Serialize(appear);
        for (const auto& [conn, netId] : s_connToNetId)
        {
            s_interface->SendMessageToConnection(conn, buf.data(), static_cast<uint32_t>(buf.size()),
                                                 k_nSteamNetworkingSend_Reliable, nullptr);
        }
    }
}

void NetCore::SendLocalCombat()
{
    // Drain the queued outgoing fire/hit events under the lock, then process outside it.
    std::vector<OutCombat> batch;
    {
        std::lock_guard<std::mutex> lock(s_outCombatMutex);
        if (s_outCombat.empty())
        {
            return;
        }
        batch.swap(s_outCombat);
    }

    for (const OutCombat& oc : batch)
    {
        if (oc.kind == OutCombat::Kind::Fire)
        {
            if (s_mode == NetMode::Client)
            {
                if (s_connection == k_HSteamNetConnection_Invalid)
                {
                    continue; // not connected; drop the cosmetic fire
                }
                // netId 0 -> host stamps our real id and relays to the others.
                PlayerFire fire = oc.fire;
                fire.shooterNetId = 0;
                const auto buf = Serialize(fire);
                s_interface->SendMessageToConnection(s_connection, buf.data(), static_cast<uint32_t>(buf.size()),
                                                     k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
            }
            else if (s_mode == NetMode::Host)
            {
                // Host originates: stamp its own id and relay to every client (cosmetic).
                PlayerFire fire = oc.fire;
                fire.shooterNetId = kLocalHostNetId;
                const auto buf = Serialize(fire);
                HostBroadcast(buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_UnreliableNoDelay);
                // The host doesn't surface its OWN muzzle back to itself (it fired locally).
            }
        }
        else // Hit
        {
            if (s_mode == NetMode::Client)
            {
                if (s_connection == k_HSteamNetConnection_Invalid)
                {
                    continue;
                }
                // netId 0 -> host stamps our real id; the host is the damage authority. Reliable:
                // a lost hit silently drops damage, and hits are low-frequency, so pay for it.
                PlayerHit hit = oc.hit;
                hit.shooterNetId = 0;
                const auto buf = Serialize(hit);
                s_interface->SendMessageToConnection(s_connection, buf.data(), static_cast<uint32_t>(buf.size()),
                                                     k_nSteamNetworkingSend_Reliable, nullptr);
            }
            else if (s_mode == NetMode::Host)
            {
                // The host IS the authority: apply its own claimed hit directly (no round-trip).
                HostApplyDamage(oc.hit.victimNetId, oc.hit.damage, kLocalHostNetId);
            }
        }
    }
}
} // namespace cr
