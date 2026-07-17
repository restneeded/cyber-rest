#pragma once

// cyber.rest — redscript-facing native game system.
//
// CyberRestSystem is registered as a native IGameSystem via red-lib. redscript reaches it
// through GameInstance.GetCyberRestSystem() (an @addMethod accessor). It is the single
// bridge between the netcode (NetCore, worker thread) and the game world (redscript, game
// thread):
//
//   redscript -> C++ (natives, called on the game thread):
//     HostGame(port: Uint16) -> Bool             bool HostGame(uint16_t)
//     JoinGame(host: String, port: Uint16) -> Bool  bool JoinGame(const Red::CString&, uint16_t)
//     IsConnected() -> Bool                      bool IsConnected()
//     LocalNetId() -> Uint32                     uint32_t GetLocalNetId()
//     PushLocalTransform(px,py,pz,yaw,vx,vy,vz: Float, locoState: Int32) -> Void
//                                                void PushLocalTransform(...)
//     PushLocalAppearance(gender: Int32, i0..i6: Uint64) -> Void   (Phase 2)
//                                                void PushLocalAppearance(...)
//     HasPeerAppearance(netId: Uint32) -> Bool                     (Phase 2)
//     GetPeerBodyGender(netId: Uint32) -> Int32                    (Phase 2)
//     GetPeerClothingItem(netId: Uint32, slot: Int32) -> TweakDBID (Phase 2)
//     TdbidFromHash(hash: Uint64) -> TweakDBID                     (Phase 2; reds has no
//                                                RED4ext::TweakDBID TdbidFromHash(uint64_t)
//                                                TDBID.FromNumber, so C++ rebuilds it)
//     PushLocalFire(weaponTweakID: Uint64, ox,oy,oz,dx,dy,dz: Float) -> Void   (Phase 3)
//     PushLocalHit(victimNetId: Uint32, damage: Float) -> Void                 (Phase 3)
//
//   C++ -> redscript (triggered from OnUpdate each frame via Red::CallVirtual, mirroring
//   the reference's native->reds pattern; these are ordinary redscript methods on the
//   same class, NOT natives):
//     CaptureLocalTransform()
//     CaptureLocalAppearance()                                     (Phase 2)
//     OnNetPlayerJoin(netId: Uint32)
//     OnNetPlayerLeave(netId: Uint32)
//     OnNetPlayerTransform(netId: Uint32, position: Vector4, yaw: Float, speed: Float,
//                          locoState: Int32, tick: Uint64)
//     OnNetPlayerAppearance(netId: Uint32)                         (Phase 2; reds pulls the
//                          gender + clothing via the getter natives above)
//     OnNetPlayerFire(shooterNetId: Uint32, origin: Vector4, dir: Vector4, weaponTweakID: Uint64) (Phase 3)
//     OnNetPlayerHealth(netId: Uint32, hp: Float)                  (Phase 3)
//     OnNetPlayerDeath(netId: Uint32)                              (Phase 3)
//     OnNetPlayerRespawn(netId: Uint32, position: Vector4)         (Phase 3)
//     OnNetPlayerStats(netId: Uint32, kills: Uint32, deaths: Uint32)  (Phase 4; persistent K/D)
//     OnNetRestorePosition(netId: Uint32, position: Vector4)       (Phase 4; teleport the local V
//                          to its host-saved position on rejoin)
//
// Phase 4 adds NO new redscript->C++ natives: the stable player GUID is resolved + published
// internally (CyberRestSystem::OnWorldAttached -> NetCore::SetLocalGuid(Identity::LocalGuid())),
// and host persistence is entirely C++-side (HostStore, keyed by GUID). reds only gains the two
// inbound handlers above.
//
// The RTTI contract here MUST stay in lockstep with scripts/CyberRest/CyberRestSystem.reds.
// Only the natives are RTTI_METHOD-registered from C++; the OnNet* handlers are declared
// (with bodies) in redscript and reached by name via CallVirtual.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <RedLib.hpp>

#include <RED4ext/NativeTypes.hpp> // RED4ext::TweakDBID (u64 <-> TweakDBID round-trip)
#include <RED4ext/SystemUpdate.hpp> // UpdateRegistrar / FrameInfo / JobQueue / UpdateTickGroup (this header's method signatures use them; the proven reference includes it here too)

#include "NetCore.hpp"
#include "Protocol.hpp"

namespace cr
{
class CyberRestSystem : public Red::IGameSystem
{
public:
    // ---- redscript -> C++ natives ----

    // Start hosting a listen server on aPort (async handshake; poll IsConnected).
    bool HostGame(uint16_t aPort);

    // Connect to aHost:aPort as a client. Returns true if the connect was initiated.
    bool JoinGame(const Red::CString& aHost, uint16_t aPort);

    // True once the session is usable (host: listening; client: connected to host).
    bool IsConnected();

    // Our own network id this session (host = 1; client = host-assigned, 0 until known).
    uint32_t GetLocalNetId();

    // Publish the local player's transform to the netcode (sent up on the worker thread).
    // Called each tick by redscript after it reads the player's world pos/yaw/velocity.
    void PushLocalTransform(float aPx, float aPy, float aPz, float aYaw, float aVx, float aVy, float aVz,
                            int32_t aLocoState);

    // ---- Phase 2 (APPEARANCE) natives ----

    // Publish the local player's look: body gender (0 unknown/1 female/2 male) + the raw
    // TweakDBID hashes worn in each of the kAppearanceSlots clothing slots (0 = empty). Called
    // by redscript on connect and whenever the local outfit changes. Fixed arg list (7 slots)
    // keeps the RTTI signature simple and matches PlayerAppearance.clothing.
    void PushLocalAppearance(int32_t aGender, uint64_t aItem0, uint64_t aItem1, uint64_t aItem2, uint64_t aItem3,
                             uint64_t aItem4, uint64_t aItem5, uint64_t aItem6);

    // Does the game thread have a stored appearance for this remote netId yet? redscript checks
    // this in OnNetPlayerAppearance / at spawn time before reading the fields below.
    bool HasPeerAppearance(uint32_t aNetId);

    // The stored body gender for a remote netId (0 unknown if none on file).
    int32_t GetPeerBodyGender(uint32_t aNetId);

    // The stored clothing item (as a reconstructed TweakDBID) for a remote netId + slot index
    // [0..kAppearanceSlots). Returns an empty TweakDBID (value 0) if unknown or slot empty.
    RED4ext::TweakDBID GetPeerClothingItem(uint32_t aNetId, int32_t aSlot);

    // Rebuild a TweakDBID from its raw u64 hash (redscript's TDBID has ToNumber but no inverse;
    // RED4ext::TweakDBID has a lossless uint64 ctor). General helper the reds side uses.
    RED4ext::TweakDBID TdbidFromHash(uint64_t aHash);

    // ---- Phase 3 (PvP) natives ----

    // Publish that the local V just fired a shot: the weapon's TweakDBID hash (0 if unknown) plus
    // the world-space shot origin (ox,oy,oz) and normalized direction (dx,dy,dz). Cosmetic — the
    // netcode relays it so remote clients play a tracer/muzzle from this player's puppet. Called
    // from a redscript weapon-fire hook, not per-frame.
    void PushLocalFire(uint64_t aWeaponTweakID, float aOx, float aOy, float aOz, float aDx, float aDy, float aDz);

    // Publish that the local V's shot hit a remote player: victim's netId + claimed damage. The
    // host applies + broadcasts authoritative HP (a client sends it up to the host). Called from a
    // redscript hit hook, not per-frame.
    void PushLocalHit(uint32_t aVictimNetId, float aDamage);

private:
    // IGameSystem hooks.
    void OnWorldAttached(Red::world::RuntimeScene* aScene) override;

    // Register our per-frame update so we can drain the inbound net-event queue on the
    // game thread and trigger the redscript OnNet* handlers. Mirrors the reference's
    // OnRegisterUpdates + RegisterUpdate(FrameBegin, ...) pattern.
    void OnRegisterUpdates(RED4ext::UpdateRegistrar* aRegistrar) override;

    // Drains NetCore's inbound event queue and dispatches each event into redscript. Runs
    // on the game thread (safe to touch RTTI / call reds from here).
    void OnNetUpdate(RED4ext::FrameInfo& aFrame, RED4ext::JobQueue& aJobQueue);

    // Scratch buffer reused each frame so we don't reallocate the drain vector. Only
    // touched on the game thread inside OnNetUpdate.
    std::vector<NetEvent> m_eventScratch;

    // Latest known appearance per remote netId, populated from Appearance NetEvents on the
    // game thread. redscript reads it back through the getter natives when it (re)skins a
    // puppet. Game-thread-only, so no lock needed. (The netcode has its own worker-side copy
    // for relay; this is the game side's cache for the reds getters.)
    std::unordered_map<uint32_t, PlayerAppearance> m_peerAppearance;

    RTTI_IMPL_TYPEINFO(CyberRestSystem);
    RTTI_IMPL_ALLOCATOR();
};
} // namespace cr

RTTI_DEFINE_CLASS(cr::CyberRestSystem, {
    RTTI_METHOD(HostGame);
    RTTI_METHOD(JoinGame);
    RTTI_METHOD(IsConnected);
    RTTI_METHOD(GetLocalNetId);
    RTTI_METHOD(PushLocalTransform);
    RTTI_METHOD(PushLocalAppearance);
    RTTI_METHOD(HasPeerAppearance);
    RTTI_METHOD(GetPeerBodyGender);
    RTTI_METHOD(GetPeerClothingItem);
    RTTI_METHOD(TdbidFromHash);
    RTTI_METHOD(PushLocalFire);
    RTTI_METHOD(PushLocalHit);
    RTTI_ALIAS("CyberRest.Core.CyberRestSystem");
});
