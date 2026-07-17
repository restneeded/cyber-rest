// cyber.rest — CyberRestSystem native method bodies + net-event pump.
//
// The natives (HostGame/JoinGame/IsConnected/GetLocalNetId/PushLocalTransform) back the
// body-less `native func`s in scripts/CyberRest/CyberRestSystem.reds and just forward to
// NetCore. OnNetUpdate runs every frame on the GAME thread: it drains the inbound event
// queue NetCore filled on the worker thread and calls the redscript OnNet* handlers, which
// own all spawning/movement. This is the reference's native->reds trigger pattern
// (Red::CallVirtual(this, "<redscriptMethod>", ...)).

#include "CyberRestSystem.hpp"

#include "Identity.hpp"
#include "NetCore.hpp"
#include "PluginContext.hpp"

#include <cmath>

#include <RED4ext/SystemUpdate.hpp>
#include <RED4ext/Scripting/Natives/Generated/Vector4.hpp>

namespace cr
{
bool CyberRestSystem::HostGame(uint16_t aPort)
{
    Log::Info("HostGame(" + std::to_string(aPort) + ") requested from redscript");
    return NetCore::StartHost(aPort);
}

bool CyberRestSystem::JoinGame(const Red::CString& aHost, uint16_t aPort)
{
    const std::string host = aHost.c_str();
    Log::Info("JoinGame(" + host + ", " + std::to_string(aPort) + ") requested from redscript");
    return NetCore::StartClient(host, aPort);
}

bool CyberRestSystem::IsConnected()
{
    return NetCore::IsConnected();
}

uint32_t CyberRestSystem::GetLocalNetId()
{
    return NetCore::LocalNetId();
}

void CyberRestSystem::PushLocalTransform(float aPx, float aPy, float aPz, float aYaw, float aVx, float aVy, float aVz,
                                         int32_t aLocoState)
{
    NetCore::PushLocalTransform(aPx, aPy, aPz, aYaw, aVx, aVy, aVz, static_cast<uint8_t>(aLocoState));
}

void CyberRestSystem::PushLocalAppearance(int32_t aGender, uint64_t aItem0, uint64_t aItem1, uint64_t aItem2,
                                          uint64_t aItem3, uint64_t aItem4, uint64_t aItem5, uint64_t aItem6)
{
    // Fold the fixed 7 slot args into the wire array in the FIXED order the reds slot list +
    // Protocol.hpp kAppearanceSlots layout agree on. static_assert guards the count so this
    // won't silently truncate if kAppearanceSlots ever changes.
    static_assert(kAppearanceSlots == 7, "PushLocalAppearance arg list must match kAppearanceSlots");
    const uint64_t clothing[kAppearanceSlots] = {aItem0, aItem1, aItem2, aItem3, aItem4, aItem5, aItem6};
    NetCore::PushLocalAppearance(static_cast<uint8_t>(aGender), clothing);
}

bool CyberRestSystem::HasPeerAppearance(uint32_t aNetId)
{
    return m_peerAppearance.find(aNetId) != m_peerAppearance.end();
}

int32_t CyberRestSystem::GetPeerBodyGender(uint32_t aNetId)
{
    const auto it = m_peerAppearance.find(aNetId);
    if (it == m_peerAppearance.end())
    {
        return 0; // unknown
    }
    return static_cast<int32_t>(it->second.bodyGender);
}

RED4ext::TweakDBID CyberRestSystem::GetPeerClothingItem(uint32_t aNetId, int32_t aSlot)
{
    if (aSlot < 0 || static_cast<uint32_t>(aSlot) >= kAppearanceSlots)
    {
        return RED4ext::TweakDBID(); // out of range -> empty
    }
    const auto it = m_peerAppearance.find(aNetId);
    if (it == m_peerAppearance.end())
    {
        return RED4ext::TweakDBID(); // unknown -> empty
    }
    return TdbidFromHash(it->second.clothing[aSlot]);
}

RED4ext::TweakDBID CyberRestSystem::TdbidFromHash(uint64_t aHash)
{
    // RED4ext::TweakDBID is a union over a uint64 value with a lossless uint64 ctor, and its
    // RTTI type maps to redscript's TweakDBID — so returning it hands reds a usable TweakDBID.
    return RED4ext::TweakDBID(aHash);
}

void CyberRestSystem::PushLocalFire(uint64_t aWeaponTweakID, float aOx, float aOy, float aOz, float aDx, float aDy,
                                    float aDz)
{
    NetCore::PushLocalFire(aWeaponTweakID, aOx, aOy, aOz, aDx, aDy, aDz);
}

void CyberRestSystem::PushLocalHit(uint32_t aVictimNetId, float aDamage)
{
    NetCore::PushLocalHit(aVictimNetId, aDamage);
}

void CyberRestSystem::OnWorldAttached(Red::world::RuntimeScene*)
{
    // Phase 4: resolve + publish this player's stable identity GUID to the netcode once, on the game
    // thread. NetCore keeps it and uses it as the client's PlayerHello (identity/auth) + the host's
    // own persistence key. Identity::LocalGuid() memoises after the first call (it also creates the
    // persistent GUID file on first ever run), so calling it here is cheap on re-attach.
    NetCore::SetLocalGuid(Identity::LocalGuid());

    // Presence itself is still driven from OnNetUpdate.
}

void CyberRestSystem::OnRegisterUpdates(RED4ext::UpdateRegistrar* aRegistrar)
{
    IGameSystem::OnRegisterUpdates(aRegistrar);
    // FrameBegin: drain net events + drive puppets once per rendered frame. The reference
    // uses the same tick group for its network update.
    aRegistrar->RegisterUpdate(RED4ext::UpdateTickGroup::FrameBegin, this, "CyberRestNetUpdate",
                               [this](RED4ext::FrameInfo& aFrame, RED4ext::JobQueue& aJobQueue)
                               { this->OnNetUpdate(aFrame, aJobQueue); });
}

void CyberRestSystem::OnNetUpdate(RED4ext::FrameInfo&, RED4ext::JobQueue&)
{
    // Nothing to do until a session is live.
    if (!NetCore::IsConnected())
    {
        return;
    }

    // 1) LOCAL CAPTURE: ask redscript to read the local player's world pos/yaw/velocity +
    //    classify locomotion and call PushLocalTransform back into us. Done in reds because
    //    it has cheap access to the player + PlayerStateMachine blackboard. Runs on the
    //    game thread (safe). The worker thread sends whatever the latest pushed value is.
    Red::CallVirtual(this, "CaptureLocalTransform");

    // 1b) LOCAL APPEARANCE: ask reds to read the local player's body gender + equipped clothing
    //     and push it. NetCore only actually sends when the look CHANGES (dirty flag), so
    //     capturing every frame is cheap and auto-covers the player changing outfits mid-session.
    Red::CallVirtual(this, "CaptureLocalAppearance");

    // 2) Pull everything the worker thread parsed since last frame + drive puppets.
    NetCore::DrainEvents(m_eventScratch);
    if (m_eventScratch.empty())
    {
        return;
    }

    for (const NetEvent& ev : m_eventScratch)
    {
        switch (ev.kind)
        {
        case NetEvent::Kind::Join:
        {
            // reds: OnNetPlayerJoin(netId: Uint32) -> spawn a puppet + map it.
            Red::CallVirtual(this, "OnNetPlayerJoin", ev.netId);
            break;
        }
        case NetEvent::Kind::Leave:
        {
            // Forget any cached look so a recycled netId can't inherit a stale appearance.
            m_peerAppearance.erase(ev.netId);
            // reds: OnNetPlayerLeave(netId: Uint32) -> despawn + unmap.
            Red::CallVirtual(this, "OnNetPlayerLeave", ev.netId);
            break;
        }
        case NetEvent::Kind::Transform:
        {
            // Pack position into a Vector4 (w=1) and derive horizontal speed so the reds
            // driver can bucket walk/run/sprint (it also has the explicit locoState). This
            // matches the reference MoveOrAnimatePuppet(entity, pos, yaw, speed, loco) shape.
            const PlayerTransform& x = ev.xform;
            RED4ext::Vector4 pos{x.px, x.py, x.pz, 1.0f};
            const float speed = std::sqrt(x.vx * x.vx + x.vy * x.vy);

            // reds: OnNetPlayerTransform(netId, position, yaw, speed, locoState, tick).
            Red::CallVirtual(this, "OnNetPlayerTransform", ev.netId, pos, x.yaw, speed,
                             static_cast<int32_t>(x.locoState), x.tick);
            break;
        }
        case NetEvent::Kind::Appearance:
        {
            // Cache the look on the game side FIRST so the reds handler's getter natives see it,
            // then notify reds. reds pulls gender + clothing back via the getters and (re)skins
            // the puppet (or defers if the puppet hasn't spawned yet — it'll re-apply on spawn).
            m_peerAppearance[ev.netId] = ev.appear;
            Red::CallVirtual(this, "OnNetPlayerAppearance", ev.netId);
            break;
        }
        case NetEvent::Kind::Fire:
        {
            // Cosmetic: a player fired. Hand reds the shot ray so it can spawn a tracer/muzzle from
            // the shooter's puppet. Pack origin + direction into Vector4s (w=1/0).
            const PlayerFire& f = ev.fire;
            RED4ext::Vector4 origin{f.ox, f.oy, f.oz, 1.0f};
            RED4ext::Vector4 dir{f.dx, f.dy, f.dz, 0.0f};
            Red::CallVirtual(this, "OnNetPlayerFire", ev.netId, origin, dir, f.weaponTweakID);
            break;
        }
        case NetEvent::Kind::Health:
        {
            // Authoritative HP from the host. reds applies it to the puppet's bar (or the local V
            // if netId == our own — that's how the local player takes networked damage).
            Red::CallVirtual(this, "OnNetPlayerHealth", ev.netId, ev.hp);
            break;
        }
        case NetEvent::Kind::Death:
        {
            // reds ragdolls/kills the matching puppet (or the local V if it's us).
            Red::CallVirtual(this, "OnNetPlayerDeath", ev.netId);
            break;
        }
        case NetEvent::Kind::Respawn:
        {
            // reds re-places the puppet (or teleports the local V). A full-HP PlayerHealth was
            // broadcast alongside, so the bar is already restored by the Health case.
            const PlayerRespawn& r = ev.respawn;
            RED4ext::Vector4 pos{r.px, r.py, r.pz, 1.0f};
            Red::CallVirtual(this, "OnNetPlayerRespawn", ev.netId, pos);
            break;
        }
        case NetEvent::Kind::Stats:
        {
            // Phase 4: authoritative persistent K/D from the host. reds updates its scoreboard cache
            // (for any netId, including our own local player's entry).
            Red::CallVirtual(this, "OnNetPlayerStats", ev.netId, ev.stats.kills, ev.stats.deaths);
            break;
        }
        case NetEvent::Kind::RestorePos:
        {
            // Phase 4: host handed US our saved position on rejoin. reds teleports the LOCAL V there.
            const PlayerRespawn& r = ev.respawn;
            RED4ext::Vector4 pos{r.px, r.py, r.pz, 1.0f};
            Red::CallVirtual(this, "OnNetRestorePosition", ev.netId, pos);
            break;
        }
        }
    }
}
} // namespace cr

// ---- redscript accessor: GameInstance.GetCyberRestSystem() -------------------------------------
//
// The reds declare `@addMethod(GameInstance) public static native func GetCyberRestSystem() ->
// ref<CyberRestSystem>` and every call site does `GameInstance.GetCyberRestSystem()`. RED4ext's
// ValidateScripts requires a matching native function on the GameInstance (ScriptGameInstance)
// RTTI type — WITHOUT this expansion the whole CyberRest script blob is rejected at load
// ("Missing native function 'GetCyberRestSystem' in native class 'GameInstance'").
//
// A no-argument free function registered onto ScriptGameInstance via RTTI_METHOD_FQN is exposed
// as a STATIC method (no `this`), which is exactly what the `static native func` decl expects.
// Red::GetGameSystem<T>() resolves the live instance off the global game instance (null before a
// world/session exists — e.g. at the cold main menu — which is fine: ToHandle(nullptr) yields an
// undefined ref and the reds guard on IsDefined()).
static Red::Handle<cr::CyberRestSystem> GetCyberRestSystem()
{
    return Red::ToHandle(Red::GetGameSystem<cr::CyberRestSystem>());
}

RTTI_EXPAND_CLASS(Red::ScriptGameInstance, {
    RTTI_METHOD_FQN(GetCyberRestSystem, "GetCyberRestSystem");
});
