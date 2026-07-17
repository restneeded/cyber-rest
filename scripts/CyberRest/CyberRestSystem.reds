module CyberRest.Core

import CyberRest.Config.*

// Native game system implemented by the cyber.rest RED4ext C++ plugin, plus the redscript
// half of the presence layer (Phase 1).
//
// NATIVE SURFACE (bodies in C++, RTTI-registered in client/red4ext/src/CyberRestSystem.hpp):
//   HostGame / JoinGame / IsConnected / GetLocalNetId / PushLocalTransform
// Type mapping the C++ side honors: Uint16<->uint16_t, Uint32<->uint32_t, Int32<->int32_t,
// Float<->float, String<->Red::CString, Bool<->bool, Void<->void.
//
// C++ -> REDS (triggered by CyberRestSystem::OnNetUpdate each frame via Red::CallVirtual):
//   CaptureLocalTransform()                         -- read local player, push our transform up
//   CaptureLocalAppearance()                        -- read local look, push gender+clothing up
//   OnNetPlayerJoin(netId)                          -- spawn a puppet for a remote player
//   OnNetPlayerLeave(netId)                         -- despawn it
//   OnNetPlayerTransform(netId, pos, yaw, speed, loco, tick) -- walk/run/sprint it to target
//   OnNetPlayerAppearance(netId)                    -- (re)skin the puppet from the cached look
//   OnNetPlayerStats(netId, kills, deaths)          -- Phase 4: cache host-authoritative K/D
//   OnNetRestorePosition(netId, pos)                -- Phase 4: teleport local V to its saved pos
// These are ordinary (non-native) methods with bodies below; C++ reaches them by name.
//
// PHASE 4 (PERSISTENCE): the host remembers each player (keyed by a stable GUID the C++ resolves +
// sends) — last position, appearance, and PvP K/D — in a JSON file. A returning player is restored:
// their local V is teleported to their saved position (OnNetRestorePosition) and their K/D is
// re-sent (OnNetPlayerStats). All of this is host-C++-side; reds only gains the two handlers above
// and a small stats cache. NO new redscript->C++ natives were added in Phase 4.
//
// PHASE 2 (APPEARANCE): remote puppets look distinct. The local player's body gender + worn
// clothing (a FIXED, order-stable slot list) are read and sent on connect (and on change);
// the receiver picks a gender-appropriate base body record and dresses the puppet with the
// networked clothing items via the TransactionSystem. Full face-morph is out of scope — this
// is gender + clothing. New natives (bodies in C++, RTTI-registered):
//   PushLocalAppearance(gender: Int32, i0..i6: Uint64) -> Void
//   HasPeerAppearance(netId: Uint32) -> Bool
//   GetPeerBodyGender(netId: Uint32) -> Int32
//   GetPeerClothingItem(netId: Uint32, slot: Int32) -> TweakDBID
//   TdbidFromHash(hash: Uint64) -> TweakDBID   (reds has TDBID.ToNumber but no inverse)
public native class CyberRestSystem extends IGameSystem {
    // ---- native funcs (implemented in C++) ----

    public native func HostGame(port: Uint16) -> Bool;
    public native func JoinGame(host: String, port: Uint16) -> Bool;
    public native func IsConnected() -> Bool;

    // Our own net id this session (host = 1; client = host-assigned, 0 until known). Used to
    // avoid ever spawning a puppet for our own player. C++: uint32_t GetLocalNetId().
    public native func GetLocalNetId() -> Uint32;

    // Publish the local player's latest transform to the netcode. C++ forwards to
    // NetCore::PushLocalTransform, which sends it on the worker thread. locoState is one of
    // the compact LocoState codes (see ClassifyLoco). C++: void PushLocalTransform(8 floats
    // -> px,py,pz,yaw,vx,vy,vz + int32 loco).
    public native func PushLocalTransform(px: Float, py: Float, pz: Float, yaw: Float,
                                          vx: Float, vy: Float, vz: Float, locoState: Int32) -> Void;

    // ---- Phase 2 appearance natives (implemented in C++) ----

    // Publish the local look: body gender (0 unknown/1 female/2 male) + the raw TweakDBID
    // hashes worn in each of the 7 synced clothing slots (0 = empty). C++ only actually sends
    // when the look changes. Fixed 7-slot arg list mirrors PlayerAppearance.clothing.
    public native func PushLocalAppearance(gender: Int32, i0: Uint64, i1: Uint64, i2: Uint64,
                                           i3: Uint64, i4: Uint64, i5: Uint64, i6: Uint64) -> Void;

    // Is a remote player's appearance cached on the game side yet? Checked before reading it.
    public native func HasPeerAppearance(netId: Uint32) -> Bool;

    // Cached body gender for a remote netId (0 unknown if none). C++: Int32 GetPeerBodyGender.
    public native func GetPeerBodyGender(netId: Uint32) -> Int32;

    // Cached clothing item (rebuilt TweakDBID) for a remote netId + slot [0..7); empty if none.
    public native func GetPeerClothingItem(netId: Uint32, slot: Int32) -> TweakDBID;

    // Rebuild a TweakDBID from a raw u64 hash (TDBID has ToNumber but no FromNumber in reds).
    public native func TdbidFromHash(hash: Uint64) -> TweakDBID;

    // ---- Phase 3 (PvP) natives (implemented in C++) ----

    // Publish that the local V fired: weapon TweakDBID hash (0 if unknown) + world-space shot
    // origin and normalized direction. Cosmetic — drives the remote tracer/muzzle. Called from
    // the local weapon-fire hook (OnNetLocalShoot), not per-frame.
    public native func PushLocalFire(weaponTweakID: Uint64, ox: Float, oy: Float, oz: Float,
                                     dx: Float, dy: Float, dz: Float) -> Void;

    // Publish that the local V's shot hit a remote player: victim netId + claimed damage. The host
    // is the damage authority (a client relays this up). Called from the local hit-detection path.
    public native func PushLocalHit(victimNetId: Uint32, damage: Float) -> Void;

    // ---- Phase 1 presence state (redscript-owned, game thread only) ----

    // Remote netId -> spawned puppet EntityID. The authority for "which puppet is which".
    private let m_puppets: array<CyberRestPeer>;

    // Phase 4: our own local player's persistent K/D, mirrored from the host's PlayerStats
    // broadcast (the host owns these, keyed by our stable GUID). A scoreboard UI in a later phase
    // reads these + each peer's CyberRestPeer.kills/deaths.
    private let m_localKills: Uint32;
    private let m_localDeaths: Uint32;

    // Distance (m) beyond which we hard-correct with a teleport instead of walking there.
    // Frequent small updates walk; a big jump means desync or the source teleported.
    private let TELEPORT_THRESHOLD: Float = 8.0;

    // ---- cached GameInstance -------------------------------------------------------------
    //
    // We extend gameIGameSystem (Red::IGameSystem). Unlike gameScriptableSystem, IGameSystem does
    // NOT expose GetGameInstance(), so `this.GetGameInstance()` is unresolved on this class. Rather
    // than change 13 call sites, we DEFINE our own GetGameInstance() below that returns a cached
    // GameInstance. It is populated the first time the local player spawns, from the player's
    // GetGame() (GameObject.GetGame() -> GameInstance) in the PlayerPuppet spawn hook (and the
    // weapon-fire hook, as a hot-join backup) at the bottom of this file. Every per-frame path
    // no-ops until m_hasGI is set (the player — and therefore a valid GameInstance — doesn't exist
    // before spawn anyway, so GetPlayer/GetXxxSystem have nothing to resolve). Storing a
    // GameInstance in a field is the same pattern shipped game code uses (e.g. hud_johnny's
    // m_gameInstance). The value is stable for the whole session; re-caching is idempotent.
    private let m_gameInstance: GameInstance;
    private let m_hasGI: Bool = false;

    // ---- MVP hotkey host/join + co-location state -----------------------------------------
    // Codeware CallbackSystem raw-key hooks, armed once (F2 = Host, F3 = Join). m_localRole records
    // which we did, so the joiner (2) can converge onto the host on the first host transform.
    private let m_inputHooked: Bool = false;
    private let m_localRole: Int32 = 0;          // 0 = none, 1 = host, 2 = joiner
    private let m_coLocateDone: Bool = false;    // joiner has teleported onto the host once

    // ---- Phase 5 (WORLD STREAMING / SHARED SPAWN) tuning ----------------------------------
    //
    // The problem: Cyberpunk only streams collision/navmesh/entities around each player's OWN V.
    // A peer far away (or in an unstreamed sector) has no ground to stand on, so spawning a puppet
    // at the raw network position drops it through the map / into the void. Rather than force every
    // player to load the same save at the same spot, we converge everyone onto a SHARED SPAWN once
    // their world is ready, then only spawn a peer's puppet when it is (a) within a relevancy radius
    // of the local V (so the area is plausibly streamed) and (b) ground-snapped onto real, loaded
    // collision. A raycast MISS is the de-facto "this sector isn't streamed" probe and gates the
    // spawn (queue + retry next tick) instead of spawning into the void.

    // TUNABLE shared spawn. Set in EnsureSharedSpawn() (arrays/structs can't hold Vector4 literals
    // in a field initializer, and we want it built lazily on the game thread).
    private let m_sharedSpawnPos: Vector4;
    private let m_sharedSpawnRot: EulerAngles;
    private let m_sharedSpawnInit: Bool = false;

    // Relevancy radius (m): a peer whose networked target is farther than this from the local V is
    // NOT spawned (or is despawned if already up) and marked deferred; it (re)spawns when back in
    // range. TUNABLE — 600m is a generous "same neighborhood, plausibly co-streamed" bubble.
    private let RELEVANCY_RADIUS: Float = 600.0;

    // World-ready gate: we must NOT teleport the local V on the connect frame (GetPlayer is often
    // null / the world is mid-stream). We poll GetPlayer in the per-frame tick and only fire once it
    // has been defined for WORLD_READY_STABLE_TICKS consecutive frames (a streaming-settle debounce),
    // OR the OnMakePlayerVisibleAfterSpawn fast-path fires. TUNABLE (~0.5s at 60fps).
    private let WORLD_READY_STABLE_TICKS: Int32 = 30;
    private let m_worldReadyTicks: Int32 = 0;
    private let m_worldReady: Bool = false;
    // Fast-path arm set by the OnMakePlayerVisibleAfterSpawn wrapper (end-of-spawn-grace edge). It
    // still funnels through the same stable-tick fire so a stale event can't fire us on a null player.
    private let m_spawnEventSeen: Bool = false;
    // Fire-once latch for the shared-spawn teleport (we converge each local session exactly once).
    private let m_sharedSpawnDone: Bool = false;

    // Ground-snap raycast tuning. We cast a short ray from a bit ABOVE the target straight down past
    // it; the hit Z is the loaded floor. up/down are generous so the raw net Z can be anywhere near
    // the surface. TUNABLE.
    private let GROUND_SNAP_UP: Float = 2.0;
    private let GROUND_SNAP_DOWN: Float = 100.0;
    // Tiny lift so the puppet doesn't spawn embedded in the floor.
    private let GROUND_SNAP_EPSILON: Float = 0.1;

    // Phase 4 restore buffered through the world-ready gate: the host may hand us our saved position
    // on a connect frame where the world isn't streamed yet, so we stash it and teleport once ready.
    private let m_pendingRestore: Bool = false;
    private let m_pendingRestorePos: Vector4;

    // ---- Phase 3 (PvP) tuning (client-side hit detection; host clamps authoritatively) ----
    // Max range (m) a reported hit ray is considered against a peer puppet.
    private let HIT_MAX_RANGE: Float = 120.0;
    // How close (m) the peer's center must be to the shot ray to count as a hit. Loose because
    // this is a coarse client-side pre-filter; the host is the authority on whether it counts.
    // NOTE (needs in-game validation): tune against real puppet capsule size.
    private let HIT_RAY_RADIUS: Float = 0.9;
    // Damage per reported hit. Flat for the minimal PvP loop (no per-weapon damage yet). The host
    // clamps this to [0, kMaxHitDamage]. NOTE: real weapon damage wiring is deferred.
    private let HIT_DAMAGE: Float = 25.0;
    // Full health, mirrors C++ kMaxHealth. Used to seed peer HP + local respawn bookkeeping.
    private let MAX_HEALTH: Float = 100.0;

    // Fallback body used when we have no appearance / gender for a peer yet. Character.Panam is
    // a known-good NPC record (used by the reference project); still valid as the default.
    private let PEER_RECORD: TweakDBID = t"Character.Panam";

    // Phase 2: base body records chosen by the peer's networked body gender, so silhouettes
    // differ before clothing is even applied. These are stock generic-citizen character records
    // with the right body type. NOTE (needs in-game validation): record names + that they spawn
    // a plain dressable puppet must be confirmed live; if a record is invalid the spawn falls
    // back to PEER_RECORD (Panam) so we never end up with no puppet.
    private let PEER_RECORD_FEMALE: TweakDBID = t"Character.afterlife_mtx_prostitute_body_wa"; // female avg body
    private let PEER_RECORD_MALE: TweakDBID = t"Character.afterlife_mtx_prostitute_body_ma";   // male avg body

    // Phase 2 clothing slot layout. FIXED and order-stable — index i here MUST match the wire
    // slot index i in Protocol.hpp (kAppearanceSlots=7) and the C++ PushLocalAppearance arg
    // order. Each entry pairs a gamedataEquipmentArea (what we READ locally) with the
    // AttachmentSlots TweakDBID (where we EQUIP on the puppet). Verified against the decompiled
    // equipmentSystem.script InitializeClothingSlotsInfo mapping.
    //   0 Outfit -> AttachmentSlots.Outfit   (a full outfit; overrides pieces below when worn)
    //   1 OuterChest -> AttachmentSlots.Torso
    //   2 InnerChest -> AttachmentSlots.Chest
    //   3 Legs -> AttachmentSlots.Legs
    //   4 Feet -> AttachmentSlots.Feet
    //   5 Head -> AttachmentSlots.Head
    //   6 Face -> AttachmentSlots.Eyes
    private let APPEARANCE_AREAS: array<gamedataEquipmentArea>;
    private let APPEARANCE_SLOTS: array<TweakDBID>;

    // ---- cached GameInstance accessor + setter --------------------------------------------

    // Returns the cached session GameInstance. This SHADOWS the accessor that gameScriptableSystem
    // would provide but gameIGameSystem does not — so every existing `this.GetGameInstance()` call
    // in this file resolves here. Callers must have gated on m_hasGI (or be on a path that only
    // runs once the local player exists); an un-cached GameInstance is not valid to pass to
    // GetPlayer/GameInstance.GetXxxSystem.
    private func GetGameInstance() -> GameInstance {
        return this.m_gameInstance;
    }

    // Cache the session GameInstance from any local GameObject's GetGame() (called from the player
    // spawn + weapon-fire hooks at the bottom of this file). Idempotent — the GameInstance is
    // stable for the whole game session.
    public func CacheGameInstance(gi: GameInstance) -> Void {
        this.m_gameInstance = gi;
        this.m_hasGI = true;
    }

    // ---- MVP: in-game hotkeys (Codeware CallbackSystem raw-key hook) -----------------------
    //
    // Armed once from the local player-spawn hook. F2 hosts a listen-server on port 7777; F3 joins
    // the address in CyberRest.Config (edit Config.reds — joiner only). The Input/Key callback fires
    // at the raw-input layer (below game key-bindings), so F2/F3 need no binding and never trigger a
    // game action. F2/F3 are unbound in CP2077 (F5/F9 = quicksave/quickload, deliberately avoided).
    public func EnsureInputHooks() -> Void {
        if this.m_inputHooked {
            return;
        }
        let cbs = GameInstance.GetCallbackSystem();
        if !IsDefined(cbs) {
            return; // Codeware not ready yet; the spawn hook calls us again on the next spawn
        }
        cbs.RegisterCallback(n"Input/Key", this, n"OnCyberRestKey")
            .AddTarget(InputTarget.Key(EInputKey.IK_F2))
            .AddTarget(InputTarget.Key(EInputKey.IK_F3));
        this.m_inputHooked = true;
        FTLog("[cyber.rest] hotkeys armed: F2 = Host (:7777), F3 = Join (address in Config.reds)");
    }

    // Raw-key handler (Codeware CallbackSystem). Only F2/F3 reach here (AddTarget filter). Act on the
    // key RELEASE edge so a held key fires once.
    private cb func OnCyberRestKey(evt: ref<KeyInputEvent>) {
        if !Equals(EnumInt(evt.GetAction()), EnumInt(EInputAction.IACT_Release)) {
            return;
        }
        let key = EnumInt(evt.GetKey());
        if Equals(key, EnumInt(EInputKey.IK_F2)) {
            if this.IsConnected() {
                FTLog("[cyber.rest] F2 ignored - already in a session");
                return;
            }
            this.m_localRole = 1;
            let ok = this.HostGame(CyberRestPort());
            FTLog(s"[cyber.rest] F2 -> HostGame = \(ok). Give friends your address; they Join on port 7777.");
        } else {
            if Equals(key, EnumInt(EInputKey.IK_F3)) {
                if this.IsConnected() {
                    FTLog("[cyber.rest] F3 ignored - already in a session");
                    return;
                }
                let addr = CyberRestHostAddress();
                this.m_localRole = 2;
                let ok = this.JoinGame(addr, CyberRestPort());
                FTLog(s"[cyber.rest] F3 -> JoinGame(\(addr)) = \(ok)");
            }
        }
    }

    // ---- session-start helpers for the in-game Multiplayer menu (CyberRest.UI) --------------
    // The popup is a different class and can't touch our private role state, so it goes through
    // these. They set m_localRole (so the joiner co-locates onto the host) then call the native.
    public func StartHostSession() -> Bool {
        this.m_localRole = 1;
        let ok = this.HostGame(CyberRestPort());
        FTLog(s"[cyber.rest] menu -> HostGame = \(ok)");
        return ok;
    }

    public func StartJoinSession(addr: String) -> Bool {
        this.m_localRole = 2;
        let ok = this.JoinGame(addr, CyberRestPort());
        FTLog(s"[cyber.rest] menu -> JoinGame(\(addr)) = \(ok)");
        return ok;
    }

    // ---- C++ -> reds: local capture -------------------------------------------------------

    // Read the local player's world position, yaw and velocity, classify locomotion, and
    // push it up. Called every frame from C++ while connected.
    public func CaptureLocalTransform() -> Void {
        // No GameInstance cached yet (the local player hasn't spawned this session) -> nothing to
        // do. It's cached from the player's GetGame() in the PlayerPuppet spawn hook below; the
        // world-ready gate + relevancy sweep both need it, so we no-op until then. (Before the
        // player spawns there is no local V to sync and no valid GameInstance to resolve anyway;
        // the spawn hook also arms the world-ready fast path, so nothing is missed.)
        if !this.m_hasGI {
            return;
        }
        // Phase 5: this is the one method C++ calls every connected frame BEFORE draining events,
        // so it's the natural home for the per-frame world-ready gate + the peer relevancy/retry
        // sweep. Both run even if the local player isn't resolved yet (the gate needs to see the
        // null->defined transition; the sweep no-ops without a local V).
        this.TickWorldReadyGate();
        this.TickPeerStreaming();

        let player = GetPlayer(this.GetGameInstance());
        if !IsDefined(player) {
            return;
        }

        let pos = player.GetWorldPosition();

        // Yaw from world orientation (facing only; exact 0deg-axis convention needs in-game
        // validation but only affects facing, not position sync). ToEulerAngles is a static
        // on Quaternion, so call it in static form (not as a method on the value).
        let euler = Quaternion.ToEulerAngles(player.GetWorldOrientation());
        let yaw = euler.Yaw;

        // World-space linear velocity (gamePuppet.GetVelocity(), native). .X/.Y horizontal,
        // .Z vertical. NOTE (needs in-game validation): if noisy at our sample rate, derive
        // velocity from successive position deltas instead.
        let vel = player.GetVelocity();
        let horizontalSpeed = SqrtF(vel.X * vel.X + vel.Y * vel.Y);

        let loco = this.ClassifyLoco(player, horizontalSpeed);

        this.PushLocalTransform(pos.X, pos.Y, pos.Z, yaw, vel.X, vel.Y, vel.Z, loco);
    }

    // ---- Phase 5: world-ready gate + shared spawn -----------------------------------------

    // Lazily build the TUNABLE shared-spawn transform (Vector4 + EulerAngles can't be field
    // initializers). Idempotent. TUNE these two values in-game: pick a wide-open, ground-solid,
    // reliably-streamed downtown spot. The value below is a Corpo-Plaza-ish downtown placeholder.
    //
    // NOTE (needs in-game validation): this coordinate is a PLACEHOLDER and MUST be verified live
    // as open sky + solid ground. Even with ground-snap, a coord inside geometry or over a hole
    // will misbehave. EulerAngles is (Roll, Pitch, Yaw); only Yaw matters for a standing spawn.
    private func EnsureSharedSpawn() -> Void {
        if this.m_sharedSpawnInit {
            return;
        }
        this.m_sharedSpawnPos = new Vector4(-1560.0, 400.0, 15.0, 1.0); // TUNABLE downtown-ish
        // EulerAngles is a native struct; the `new EulerAngles(...)` intrinsic ctor warns. A
        // default-declared local zero-inits Roll/Pitch/Yaw (0,0,0) cleanly. TUNE Yaw only (a
        // standing spawn ignores Roll/Pitch); set rot.Yaw here if a facing is wanted.
        let rot: EulerAngles;
        this.m_sharedSpawnRot = rot;                                    // TUNABLE (Roll,Pitch,Yaw)
        this.m_sharedSpawnInit = true;
    }

    // Fast-path arm from the OnMakePlayerVisibleAfterSpawn wrapper (end-of-spawn-grace edge). Kept
    // trivial on purpose (the wrapper is a hot override); the real work happens in the tick gate,
    // which still requires GetPlayer to be stably defined before firing.
    public func NotifyLocalSpawned() -> Void {
        this.m_spawnEventSeen = true;
    }

    // Poll-until-stable world-ready gate. Called every connected frame from CaptureLocalTransform.
    // We deliberately do NOT teleport on the connect frame: GetPlayer is null in the menu and often
    // during the first stream-in, and firing a local-V teleport then does nothing (or worse). We
    // wait until GetPlayer has been defined for WORLD_READY_STABLE_TICKS consecutive frames (a
    // streaming-settle debounce) — the spawn-grace event only shortens the wait, it can't bypass
    // the "player actually exists" requirement (covers hot-join, where the event never re-fires).
    private func TickWorldReadyGate() -> Void {
        if this.m_worldReady {
            return; // already fired; nothing more to do
        }
        let player = GetPlayer(this.GetGameInstance());
        if !IsDefined(player) {
            // Lost/not-yet player (menu, load screen, mid-stream). Reset the debounce so we require
            // a fresh run of stable frames once it comes back.
            this.m_worldReadyTicks = 0;
            return;
        }
        this.m_worldReadyTicks += 1;
        // The spawn-grace event, when seen, lets us fire as soon as the player is defined at all
        // (it already means the engine ended the post-spawn grace period). Otherwise wait for the
        // full stable-tick debounce.
        let ready = this.m_spawnEventSeen || this.m_worldReadyTicks >= this.WORLD_READY_STABLE_TICKS;
        if ready {
            this.m_worldReady = true;
            this.OnLocalWorldReady();
        }
    }

    // Fire-once when the local world is ready: converge onto the shared spawn (unless the host sent
    // us a saved position to restore, which wins). This is the auto-converge that removes the
    // "everyone load the same save" chore.
    private func OnLocalWorldReady() -> Void {
        let gi = this.GetGameInstance();
        let player = GetPlayer(gi);
        if !IsDefined(player) {
            this.m_worldReady = false; // defensive: shouldn't happen (gate checked it), re-arm
            return;
        }
        let tp = GameInstance.GetTeleportationFacility(gi);
        if !IsDefined(tp) {
            return;
        }

        // A buffered Phase-4 restore (returning player) wins over the shared spawn.
        if this.m_pendingRestore {
            let orient = Quaternion.ToEulerAngles(player.GetWorldOrientation());
            tp.Teleport(player, this.m_pendingRestorePos, orient);
            this.m_pendingRestore = false;
            FTLog(s"[cyber.rest] world-ready -> restored local V to saved position");
            return;
        }

        // MVP: no fixed-coordinate shared spawn (the placeholder coord dropped players into the
        // void). The HOST stays where their save loaded them; the JOINER is teleported onto the host
        // the first time it hears the host's position (see OnNetPlayerTransform). So just latch the
        // converge as done here -- there is nothing to teleport on our own.
        this.m_sharedSpawnDone = true;
    }

    // ---- Phase 5: ground-snap raycast -----------------------------------------------------

    // Snap a spawn position's Z onto loaded, solid ground under (X, Y) near Z. Returns true and
    // writes `snapped` (with w=1.0) on a hit; returns false if NOTHING was hit — which, critically,
    // is the expected signal that this area's collision isn't streamed. Callers MUST treat false as
    // "not ready, queue + retry", NOT as "spawn at the raw Z" (that reintroduces the void-spawn bug).
    //
    // Cast a short ray from a bit above the target straight down. Try n"PlayerBlocker" first (the
    // game's own "can a player stand here" floor group), fall back to n"Static" (the generic floor
    // group GetFloorAngle uses). staticOnly=true (ground is static), dynamicOnly=false.
    private func GroundSnap(pos: Vector4, out snapped: Vector4) -> Bool {
        let sqs = GameInstance.GetSpatialQueriesSystem(this.GetGameInstance());
        if !IsDefined(sqs) {
            return false;
        }
        let start = new Vector4(pos.X, pos.Y, pos.Z + this.GROUND_SNAP_UP, 1.0);
        let end = new Vector4(pos.X, pos.Y, pos.Z - this.GROUND_SNAP_DOWN, 1.0);
        let res: TraceResult;
        // 6-arg out-param form verified against decompiled spatialQueriesSystem.swift + codeware wiki
        // (NOT the CET/Lua 5-arg tuple-return shape, which won't compile in redscript).
        if !sqs.SyncRaycastByCollisionGroup(start, end, n"PlayerBlocker", res, true, false)
        || !TraceResult.IsValid(res) {
            if !sqs.SyncRaycastByCollisionGroup(start, end, n"Static", res, true, false)
            || !TraceResult.IsValid(res) {
                return false; // no ground loaded here -> caller queues a retry next tick
            }
        }
        // TraceResult.position is a Vector3; read .Z directly and rebuild a spawn Vector4 with w=1.0
        // (Cast<Vector4> of a Vector3 yields w=0, which is wrong for a position).
        snapped = new Vector4(pos.X, pos.Y, res.position.Z + this.GROUND_SNAP_EPSILON, 1.0);
        return true;
    }

    // ---- Phase 5: relevancy --------------------------------------------------------------

    // Is a peer's networked target within the relevancy radius of the local V? (i.e. plausibly in a
    // co-streamed area). Returns false if we have no local V yet (nothing is relevant without one).
    private func IsPeerRelevant(peerPos: Vector4) -> Bool {
        if !this.m_hasGI {
            return false; // no local V without a GameInstance -> nothing is relevant yet
        }
        let player = GetPlayer(this.GetGameInstance());
        if !IsDefined(player) {
            return false;
        }
        return Vector4.Distance(player.GetWorldPosition(), peerPos) <= this.RELEVANCY_RADIUS;
    }

    // ---- Phase 5: per-frame peer streaming sweep ------------------------------------------

    // Runs every connected frame (from CaptureLocalTransform). Drives the deferred/pending-spawn
    // retry loop that the join/transform handlers set up: for each tracked peer that isn't spawned
    // yet but has a known target, retry the relevancy + ground-snap + create when its area streams
    // in; and despawn a spawned puppet that has drifted out of the relevancy radius (it re-spawns
    // when the local V comes back near). This is what makes a peer who was in the void "pop in"
    // once you get close enough for their ground to load.
    private func TickPeerStreaming() -> Void {
        // Don't churn until our own world is ready (and thus we're at the shared spawn); before that
        // GetPlayer may be null and every relevancy check is false anyway.
        if !this.m_worldReady {
            return;
        }
        let i = 0;
        while i < ArraySize(this.m_puppets) {
            let peer = this.m_puppets[i];
            // Only act on peers we have a target for (a transform arrived at least once).
            if peer.hasTarget {
                let relevant = this.IsPeerRelevant(peer.targetPos);
                if peer.spawned {
                    // Spawned but drifted out of range -> despawn + mark deferred so it re-spawns.
                    if !relevant {
                        this.DespawnPeerPuppet(peer.entityId);
                        let emptyId: EntityID;   // default-zero -> EntityID.IsDefined == false
                        peer.spawned = false;
                        peer.entityId = emptyId;
                        peer.appearanceApplied = false;
                        peer.deferred = true;
                        // Drop the AICommand ref that belonged to the now-deleted puppet. If we don't,
                        // a later re-spawn (back in range) would feed this stale ref into
                        // MoveOrAnimatePuppet -> GetCommandState/CancelCommand on the NEW puppet's AI
                        // component with a command that isn't its own (dangling / wrong-entity op).
                        peer.lastCmd = null;
                        this.m_puppets[i] = peer;
                        FTLog(s"[cyber.rest] netId \(peer.netId) left relevancy -> despawned (deferred)");
                    }
                } else {
                    // Not spawned yet. If in range, try to ground-snap + create now (retry loop).
                    // TrySpawnPeer owns the m_puppets[i] write-back on success/failure.
                    if relevant {
                        this.TrySpawnPeer(i);
                    }
                }
            }
            i += 1;
        }
    }

    // Attempt to create a peer's puppet at its (ground-snapped) target. On success, fills entityId +
    // spawned=true and dresses it if we have the look. On a ground-snap miss (area not streamed) it
    // leaves the peer un-spawned so the next TickPeerStreaming retries. Idempotent-ish: no-ops if the
    // peer is already spawned. Operates on m_puppets[idx] and writes the struct back.
    private func TrySpawnPeer(idx: Int32) -> Void {
        let peer = this.m_puppets[idx];
        if peer.spawned {
            return;
        }
        if !peer.hasTarget {
            return; // nothing to place it at yet
        }
        // Ground-snap onto loaded collision. A miss means the sector isn't streamed -> retry later.
        let snapped: Vector4;
        if !this.GroundSnap(peer.targetPos, snapped) {
            peer.deferred = true; // still waiting for the area to stream in
            this.m_puppets[idx] = peer;
            return;
        }

        // Pick the base body record from the peer's networked gender if we have their appearance.
        let gender = 0;
        if this.HasPeerAppearance(peer.netId) {
            gender = this.GetPeerBodyGender(peer.netId);
        }

        let spec = new DynamicEntitySpec();
        spec.recordID = this.RecordForGender(gender);
        spec.alwaysSpawned = true;   // don't let the engine cull our puppet
        spec.persistState = false;
        spec.persistSpawn = false;
        spec.tags = [n"CyberRest", n"CyberRestPeer"];
        spec.position = snapped;                              // ground-snapped, not raw net Z
        spec.orientation = new Quaternion(0.0, 0.0, 0.0, 1.0);

        let entId = GameInstance.GetDynamicEntitySystem().CreateEntity(spec);
        if !EntityID.IsDefined(entId) {
            // Create failed (area still settling) -> leave deferred, retry next tick.
            peer.deferred = true;
            this.m_puppets[idx] = peer;
            FTLog(s"[cyber.rest] netId \(peer.netId) CreateEntity failed -> retry next tick");
            return;
        }

        peer.entityId = entId;
        peer.spawned = true;
        peer.deferred = false;
        peer.appearanceApplied = false;
        this.m_puppets[idx] = peer;
        FTLog(s"[cyber.rest] Spawned puppet for netId \(peer.netId) at ground-snapped (\(snapped.X), \(snapped.Y), \(snapped.Z)) gender \(gender)");

        // Dress it now if we already know the look (else spawn/transform paths retry).
        if this.HasPeerAppearance(peer.netId) {
            this.TryApplyAppearance(peer.netId);
        }
    }

    // Delete a peer's spawned puppet if the id is live. Small wrapper so despawn is uniform.
    private func DespawnPeerPuppet(entityId: EntityID) -> Void {
        if EntityID.IsDefined(entityId) {
            GameInstance.GetDynamicEntitySystem().DeleteEntity(entityId);
        }
    }

    // ---- Phase 2: local appearance capture ------------------------------------------------

    // Lazily build the fixed area<->slot layout (arrays can't hold t"" literals in a field
    // initializer). Idempotent; called before any read/apply that needs the layout.
    private func EnsureAppearanceLayout() -> Void {
        if ArraySize(this.APPEARANCE_AREAS) == 7 {
            return;
        }
        ArrayClear(this.APPEARANCE_AREAS);
        ArrayClear(this.APPEARANCE_SLOTS);
        // Order MUST match Protocol.hpp kAppearanceSlots layout + C++ PushLocalAppearance args.
        ArrayPush(this.APPEARANCE_AREAS, gamedataEquipmentArea.Outfit);
        ArrayPush(this.APPEARANCE_SLOTS, t"AttachmentSlots.Outfit");
        ArrayPush(this.APPEARANCE_AREAS, gamedataEquipmentArea.OuterChest);
        ArrayPush(this.APPEARANCE_SLOTS, t"AttachmentSlots.Torso");
        ArrayPush(this.APPEARANCE_AREAS, gamedataEquipmentArea.InnerChest);
        ArrayPush(this.APPEARANCE_SLOTS, t"AttachmentSlots.Chest");
        ArrayPush(this.APPEARANCE_AREAS, gamedataEquipmentArea.Legs);
        ArrayPush(this.APPEARANCE_SLOTS, t"AttachmentSlots.Legs");
        ArrayPush(this.APPEARANCE_AREAS, gamedataEquipmentArea.Feet);
        ArrayPush(this.APPEARANCE_SLOTS, t"AttachmentSlots.Feet");
        ArrayPush(this.APPEARANCE_AREAS, gamedataEquipmentArea.Head);
        ArrayPush(this.APPEARANCE_SLOTS, t"AttachmentSlots.Head");
        ArrayPush(this.APPEARANCE_AREAS, gamedataEquipmentArea.Face);
        ArrayPush(this.APPEARANCE_SLOTS, t"AttachmentSlots.Eyes");
    }

    // Read the local player's body gender + the clothing worn in each synced slot, and push it
    // up. Called every frame from C++ while connected; C++ only sends when the look changes, so
    // this cheaply covers the player swapping outfits mid-session.
    public func CaptureLocalAppearance() -> Void {
        if !this.m_hasGI {
            return; // no GameInstance cached yet (player not spawned) -> nothing to read
        }
        let player = GetPlayer(this.GetGameInstance());
        if !IsDefined(player) {
            return;
        }
        this.EnsureAppearanceLayout();

        // Body gender. GetResolvedGenderName() (on gamePuppet) returns n"Female"/n"Male".
        // NOTE (needs in-game validation): the exact returned CNames; unrecognized -> unknown(0).
        let gender = this.GenderNameToCode(player.GetResolvedGenderName());

        // Equipped clothing item per area -> its TweakDBID hash on the wire (0 = empty).
        let hashes: array<Uint64>;
        let i = 0;
        while i < ArraySize(this.APPEARANCE_AREAS) {
            ArrayPush(hashes, this.ReadEquippedHash(player, this.APPEARANCE_AREAS[i]));
            i += 1;
        }

        // Fixed 7-slot arg list (mirrors PlayerAppearance.clothing / the C++ native).
        this.PushLocalAppearance(gender,
            hashes[0], hashes[1], hashes[2], hashes[3], hashes[4], hashes[5], hashes[6]);
    }

    // Read the ItemID equipped in an area for a game object and return its TweakDBID as a raw
    // u64 (0 if the slot is empty / invalid). Uses the EquipmentSystem player data (the same
    // path the base game uses to answer "what's equipped in area X").
    private func ReadEquippedHash(player: ref<GameObject>, area: gamedataEquipmentArea) -> Uint64 {
        let data = EquipmentSystem.GetData(player);
        if !IsDefined(data) {
            return 0ul;
        }
        let itemID = data.GetItemInEquipSlot(area, 0);
        if !ItemID.IsValid(itemID) {
            return 0ul;
        }
        return TDBID.ToNumber(ItemID.GetTDBID(itemID));
    }

    // Map a resolved gender CName to the compact wire code (0 unknown, 1 female, 2 male).
    private func GenderNameToCode(genderName: CName) -> Int32 {
        if Equals(genderName, n"Female") {
            return 1;
        }
        if Equals(genderName, n"Male") {
            return 2;
        }
        return 0; // unknown -> receiver falls back to the default body record
    }

    // Classify the local player's locomotion into the compact wire code, mirroring the
    // shared C++ LocoState enum: 0 idle, 1 walk, 2 run, 3 sprint, 4 crouch, 5 jump, 6 fall.
    // Reads the PlayerStateMachine blackboard for the special states, then buckets by speed.
    // Ported from the reference GetLocalLocoState. THRESHOLDS ARE UNVERIFIED and must be
    // tuned in-game (CP2077 walk ~1.5, run ~3-4, sprint ~6+ m/s).
    private func ClassifyLoco(player: ref<GameObject>, horizontalSpeed: Float) -> Int32 {
        let bbSystem = GameInstance.GetBlackboardSystem(this.GetGameInstance());
        if IsDefined(bbSystem) {
            let defs = GetAllBlackboardDefs();
            let psm = bbSystem.GetLocalInstanced(player.GetEntityID(), defs.PlayerStateMachine);
            if IsDefined(psm) {
                // In a vehicle: no clean puppet loco-state; treat as idle (Phase 2 handles vehicles).
                if psm.GetBool(defs.PlayerStateMachine.MountedToVehicle) {
                    return 0;
                }
                let locoBB = psm.GetInt(defs.PlayerStateMachine.Locomotion);
                if Equals(locoBB, EnumInt(gamePSMLocomotionStates.Sprint))
                || Equals(locoBB, EnumInt(gamePSMLocomotionStates.CrouchSprint)) {
                    return 3; // sprint
                }
                if Equals(locoBB, EnumInt(gamePSMLocomotionStates.Crouch))
                || Equals(locoBB, EnumInt(gamePSMLocomotionStates.CrouchDodge)) {
                    return 4; // crouch (driver maps to walk)
                }
                if Equals(locoBB, EnumInt(gamePSMLocomotionStates.Jump)) {
                    return 5; // jump (best-effort hint, not animated in Phase 1)
                }
                let fall = psm.GetInt(defs.PlayerStateMachine.Fall);
                if !Equals(fall, EnumInt(gamePSMFallStates.Default)) {
                    return 6; // fall (best-effort hint)
                }
            }
        }

        // Grounded default: bucket by horizontal speed.
        if horizontalSpeed < 0.3 { return 0; } // idle
        if horizontalSpeed < 1.8 { return 1; } // walk
        if horizontalSpeed < 4.0 { return 2; } // run
        return 3;                              // sprint
    }

    // ---- C++ -> reds: remote presence -----------------------------------------------------

    // A remote player joined: register a peer slot but DO NOT spawn a puppet yet. Phase 5: we can't
    // spawn until we know WHERE (we need a transform to run the relevancy + ground-snap gate), and
    // spawning at origin/raw-Z is exactly the void-spawn bug we're fixing. The per-frame streaming
    // sweep (TickPeerStreaming) creates the puppet once the peer's first transform arrives, it's in
    // range, and there's loaded ground to snap to.
    public func OnNetPlayerJoin(netId: Uint32) -> Void {
        if netId == this.GetLocalNetId() {
            return; // never spawn our own player (defensive; C++ already filters)
        }
        if this.FindPeerIndex(netId) != -1 {
            FTLog(s"[cyber.rest] OnNetPlayerJoin: netId \(netId) already tracked");
            return;
        }

        let peer: CyberRestPeer;
        peer.netId = netId;
        // entityId is left default (zero) -> EntityID.IsDefined == false until we actually spawn it.
        peer.spawned = false;
        peer.deferred = true;               // waiting for a target + streamed ground
        peer.hasTarget = false;             // no transform yet -> nowhere to place it
        peer.appearanceApplied = false;
        peer.hp = this.MAX_HEALTH; // assume full until the host's first PlayerHealth arrives
        peer.alive = true;
        peer.kills = 0u;  // Phase 4: host's PlayerStats (join replay) fills the real values
        peer.deaths = 0u;
        ArrayPush(this.m_puppets, peer);
        FTLog(s"[cyber.rest] Registered peer netId \(netId) (spawn deferred until in-range + ground-ready)");
    }

    // Base body record for a wire gender code (0 unknown -> default Panam, 1 female, 2 male).
    private func RecordForGender(gender: Int32) -> TweakDBID {
        if gender == 1 {
            return this.PEER_RECORD_FEMALE;
        }
        if gender == 2 {
            return this.PEER_RECORD_MALE;
        }
        return this.PEER_RECORD;
    }

    // A remote player left: despawn its puppet (if one is live) and forget it. Phase 5: a peer may
    // have been registered but never spawned (deferred / out of range), so guard the delete.
    public func OnNetPlayerLeave(netId: Uint32) -> Void {
        let idx = this.FindPeerIndex(netId);
        if idx == -1 {
            FTLog(s"[cyber.rest] OnNetPlayerLeave: no peer for netId \(netId)");
            return;
        }
        this.DespawnPeerPuppet(this.m_puppets[idx].entityId);
        ArrayErase(this.m_puppets, idx);
        FTLog(s"[cyber.rest] Removed peer netId \(netId)");
    }

    // ---- Phase 2: remote appearance -------------------------------------------------------

    // A remote player's look arrived (or changed). The C++ side has already cached it (its
    // getter natives read from that cache), so just (re)apply it to the puppet. If the puppet
    // hasn't spawned yet, this is a no-op and OnNetPlayerJoin / a later re-drive applies it.
    public func OnNetPlayerAppearance(netId: Uint32) -> Void {
        if netId == this.GetLocalNetId() {
            return; // never skin our own player
        }
        let idx = this.FindPeerIndex(netId);
        if idx != -1 {
            // Force a re-apply even if we dressed it before — the look may have changed. Use the
            // copy-out/write-back pattern (array-indexed struct writes aren't reliable in reds).
            let peer = this.m_puppets[idx];
            peer.appearanceApplied = false;
            this.m_puppets[idx] = peer;
        }
        this.TryApplyAppearance(netId);
    }

    // Apply the cached appearance to a peer's puppet if the puppet exists and we haven't already
    // dressed it for the current look. Idempotent + safe to call repeatedly (spawn, appearance,
    // and transform paths all funnel here so a late-resolving entity still gets dressed).
    private func TryApplyAppearance(netId: Uint32) -> Void {
        let idx = this.FindPeerIndex(netId);
        if idx == -1 {
            return; // no puppet yet; will be applied at/after spawn
        }
        let peer = this.m_puppets[idx];
        if !peer.spawned {
            return; // puppet not created yet (deferred / out of range); dressed at/after spawn
        }
        if peer.appearanceApplied {
            return; // already dressed for the current look
        }
        if !this.HasPeerAppearance(netId) {
            return; // no look cached yet
        }

        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(peer.entityId);
        let puppet = entity as ScriptedPuppet;
        if !IsDefined(puppet) {
            return; // still async-spawning; a later call retries
        }

        this.ApplyAppearanceToPuppet(puppet, netId);
        peer.appearanceApplied = true;
        this.m_puppets[idx] = peer; // write back the applied flag
    }

    // Dress the puppet in the peer's networked clothing via the TransactionSystem: give each
    // item then attach it to its clothing slot. gender already picked the base body record at
    // spawn (that can't be changed live), so this just layers the garments on top.
    //
    // NOTE (needs in-game validation): GiveItem+AddItemToSlot on a dynamically-spawned NPC is
    // the AppearanceMenuMod-proven way to equip garments on a puppet, but the exact garment
    // refresh behavior (whether AddItemToSlot alone re-skins, or a RefreshAttachment is needed)
    // must be confirmed live. Items that fail to give/attach are skipped, not fatal.
    private func ApplyAppearanceToPuppet(puppet: ref<ScriptedPuppet>, netId: Uint32) -> Void {
        this.EnsureAppearanceLayout();
        let ts = GameInstance.GetTransactionSystem(this.GetGameInstance());
        if !IsDefined(ts) {
            return;
        }

        let i = 0;
        while i < ArraySize(this.APPEARANCE_SLOTS) {
            let tdbid = this.GetPeerClothingItem(netId, i);
            if TDBID.IsValid(tdbid) {
                let itemID = ItemID.FromTDBID(tdbid);
                // Give the garment to the puppet (idempotent enough — HasItem guard avoids dup
                // stacks) then attach it to the mapped clothing slot so the mesh actually shows.
                if !ts.HasItem(puppet, itemID) {
                    ts.GiveItem(puppet, itemID, 1);
                }
                ts.AddItemToSlot(puppet, this.APPEARANCE_SLOTS[i], itemID);
            }
            i += 1;
        }
        FTLog(s"[cyber.rest] Applied appearance to puppet for netId \(netId)");
    }

    // A remote player moved. Phase 5: first record the latest target (this is what lets the
    // streaming sweep place the puppet), then — if the puppet is actually spawned — drive it. If
    // it isn't spawned yet (deferred, out of range, or its ground hasn't streamed), we try to spawn
    // it right here too so an in-range peer with loaded ground pops in on its very first transform
    // instead of waiting a frame for the sweep.
    public func OnNetPlayerTransform(netId: Uint32, position: Vector4, yaw: Float, speed: Float,
                                     locoState: Int32, tick: Uint64) -> Void {
        let idx = this.FindPeerIndex(netId);
        if idx == -1 {
            // Transform before the join arrived (out-of-order) — ignore; the join will register.
            return;
        }

        // Record the newest networked target so the relevancy + retry logic always has a place to
        // put the puppet, spawned or not. Copy-out / write-back (array-indexed struct writes aren't
        // reliable in reds).
        let peer = this.m_puppets[idx];
        peer.targetPos = position;
        peer.hasTarget = true;
        this.m_puppets[idx] = peer;

        // MVP co-location: the JOINER converges onto the HOST the first time it hears the host's
        // position, so both players end up in the same streamed area (the host's own loaded world --
        // guaranteed real geometry, unlike a hardcoded coord). 2-player MVP: the only peer IS the
        // host, so the first transform we get is the host's.
        if this.m_localRole == 2 && !this.m_coLocateDone {
            let coGi = this.GetGameInstance();
            let localV = GetPlayer(coGi);
            if IsDefined(localV) {
                let coTp = GameInstance.GetTeleportationFacility(coGi);
                if IsDefined(coTp) {
                    let coOrient = Quaternion.ToEulerAngles(localV.GetWorldOrientation());
                    coTp.Teleport(localV, position, coOrient);
                    this.m_coLocateDone = true;
                    this.m_sharedSpawnDone = true;
                    FTLog(s"[cyber.rest] joined -> teleported onto host at (\(position.X), \(position.Y), \(position.Z))");
                }
            }
        }

        // Not spawned yet: try now (in-range + ground-ready), else it stays deferred for the sweep.
        if !peer.spawned {
            if this.IsPeerRelevant(position) {
                this.TrySpawnPeer(idx);
            }
            peer = this.m_puppets[idx];
            if !peer.spawned {
                return; // still can't place it (out of range / no streamed ground) — retry later
            }
        }

        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(peer.entityId);
        let puppet = entity as ScriptedPuppet;
        if !IsDefined(puppet) {
            return; // still async-spawning; a later transform will catch it
        }

        // Phase 3: don't drive a dead puppet (it's ragdolled/defeated until it respawns). The
        // respawn handler flips alive back on and teleports it into place.
        if !peer.alive {
            return;
        }

        // Deferred-apply: if an appearance arrived before the puppet finished spawning (so
        // TryApplyAppearance no-op'd), the puppet is resolved now — dress it. Cheap: no-ops once
        // applied (appearanceApplied guard).
        if !peer.appearanceApplied && this.HasPeerAppearance(netId) {
            this.ApplyAppearanceToPuppet(puppet, netId);
            peer.appearanceApplied = true;
        }

        peer.lastCmd = this.MoveOrAnimatePuppet(puppet, position, yaw, speed, locoState, peer.lastCmd);
        this.m_puppets[idx] = peer;
    }

    // ---- puppet driver (ported from the reference NetworkGameSystem.reds) -----------------

    // Hard snap. Kept for the drift-reconciliation / fresh-spawn escape hatch only (no
    // locomotion animation).
    private func TeleportPuppet(puppet: ref<ScriptedPuppet>, position: Vector4, rotation: Float) -> ref<AICommand> {
        let cmd = new AITeleportCommand();
        cmd.position = position;
        cmd.rotation = rotation;
        cmd.doNavTest = false;

        let ai = puppet.GetAIControllerComponent();
        ai.SendCommand(cmd);
        // Temp (per reference): disable collider so nearby NPCs don't shove the puppet off
        // its networked path. Revisit in a later phase.
        ai.DisableCollider();
        ai.ForceTickNextFrame();
        return cmd;
    }

    // Make the remote puppet WALK/RUN/SPRINT toward the networked target with real
    // locomotion animation instead of a per-frame teleport slide. Called once per received
    // movement packet. locoState: 0 idle,1 walk,2 run,3 sprint,4 crouch,5 jump,6 fall.
    private func MoveOrAnimatePuppet(puppet: ref<ScriptedPuppet>, targetPos: Vector4, yaw: Float,
                                     velocity: Float, locoState: Int32, lastCmd: ref<AICommand>) -> ref<AICommand> {
        let ai = puppet.GetAIControllerComponent();
        if !IsDefined(ai) {
            return null;
        }

        let currentPos = puppet.GetWorldPosition();
        let drift = Vector4.Distance(currentPos, targetPos);
        let freshSpawn = currentPos.X == 0.0 && currentPos.Y == 0.0 && currentPos.Z == 0.0;

        // --- Hard-correct: fresh spawn (at origin), too far to plausibly walk, or teleport. ---
        if freshSpawn || drift > this.TELEPORT_THRESHOLD || locoState < 0 {
            if IsDefined(lastCmd) && Equals(EnumInt(ai.GetCommandState(lastCmd)), EnumInt(AICommandState.Executing)) {
                ai.CancelCommand(lastCmd);
            }
            return this.TeleportPuppet(puppet, targetPos, yaw);
        }

        // --- Idle: stop any active walk and stand. ---
        if locoState == 0 || velocity < 0.3 {
            if IsDefined(lastCmd)
            && !Equals(EnumInt(ai.GetCommandState(lastCmd)), EnumInt(AICommandState.Success))
            && !Equals(EnumInt(ai.GetCommandState(lastCmd)), EnumInt(AICommandState.Failure)) {
                ai.CancelCommand(lastCmd);
            }
            return null;
        }

        // --- Walk / Run / Sprint toward the target, animated by the locomotion graph. ---
        let mtype: moveMovementType;
        if locoState >= 3 || velocity > 5.5 {
            mtype = moveMovementType.Sprint;
        } else if locoState == 2 || velocity > 3.0 {
            mtype = moveMovementType.Run;
        } else {
            // Walk covers explicit walk (1) and crouch (4).
            mtype = moveMovementType.Walk;
        }

        let targetWorldPos: WorldPosition;
        WorldPosition.SetVector4(targetWorldPos, targetPos);
        let targetSpec: AIPositionSpec;
        AIPositionSpec.SetWorldPosition(targetSpec, targetWorldPos);

        let cmd = new AIMoveToCommand();
        cmd.movementTarget = targetSpec;
        cmd.movementType = mtype;
        cmd.ignoreNavigation = true;   // frequent net updates -> don't let full pathfinding fight us
        cmd.useStart = false;          // no start-transition anim on every re-issue (prevents jerk)
        cmd.useStop = false;           // let the next command chain the walk cycle
        cmd.desiredDistanceFromTarget = 0.0;
        cmd.finishWhenDestinationReached = true;

        // Face travel direction: aim a point a bit ahead along yaw. NOTE: yaw->forward-vector
        // convention needs in-game validation; only affects facing, not where it walks.
        let yawRad = Deg2Rad(yaw);
        let ahead = new Vector4(targetPos.X + CosF(yawRad) * 2.0, targetPos.Y + SinF(yawRad) * 2.0, targetPos.Z, 1.0);
        let aheadWorldPos: WorldPosition;
        WorldPosition.SetVector4(aheadWorldPos, ahead);
        let facingSpec: AIPositionSpec;
        AIPositionSpec.SetWorldPosition(facingSpec, aheadWorldPos);
        cmd.facingTarget = facingSpec;
        cmd.rotateEntityTowardsFacingTarget = true;

        // Smoothest re-issue: only cancel the previous move once it's terminal; otherwise let
        // the AI queue replace the executing same-class move (continuous walk cycle, no hitch).
        if IsDefined(lastCmd) {
            let st = ai.GetCommandState(lastCmd);
            if Equals(EnumInt(st), EnumInt(AICommandState.Success))
            || Equals(EnumInt(st), EnumInt(AICommandState.Failure)) {
                ai.CancelCommand(lastCmd);
            }
        }

        ai.DisableCollider();
        ai.SendCommand(cmd);
        return cmd;
    }

    // ---- Phase 3 (PvP): local fire + hit reporting ----------------------------------------

    // Called from the local weapon-fire hook (@wrapMethod BaseProjectile.OnShoot -> this) when the
    // local V shoots. Reads the shot origin + direction, sends a cosmetic PlayerFire so remotes
    // draw a tracer, then does a coarse client-side hit check against every peer puppet along the
    // ray and reports any hit up to the host (host is the damage authority + clamps it).
    //
    // NOTE (needs in-game validation): startPoint/startVelocity on gameprojectileShootEvent give a
    // good ray for projectile + most hitscan weapons; for pure-hitscan the exact convention should
    // be confirmed live. This is deliberately simple — the host arbitrates, so a loose client-side
    // check is acceptable for a minimal PvP loop.
    public func OnNetLocalShoot(origin: Vector4, dir: Vector4, weaponTweakID: Uint64) -> Void {
        if !this.IsConnected() {
            return;
        }
        // Normalize the direction (guard against a zero vector).
        let len = SqrtF(dir.X * dir.X + dir.Y * dir.Y + dir.Z * dir.Z);
        if len < 0.0001 {
            return;
        }
        let nd = new Vector4(dir.X / len, dir.Y / len, dir.Z / len, 0.0);

        // 1) Cosmetic fire (relayed to the other clients for the tracer/muzzle).
        this.PushLocalFire(weaponTweakID, origin.X, origin.Y, origin.Z, nd.X, nd.Y, nd.Z);

        // 2) Coarse hit detection: find the nearest peer puppet whose center is close to the ray,
        //    in front of the shooter, within range. Report a hit on it (flat damage; host clamps).
        let hitNetId = this.RaycastPeers(origin, nd);
        if hitNetId != 0u {
            this.PushLocalHit(hitNetId, this.HIT_DAMAGE);
        }
    }

    // Return the netId of the nearest alive peer puppet intersected by the ray origin+dir (within
    // HIT_RAY_RADIUS of the ray line, in front of the origin, within HIT_MAX_RANGE), or 0 if none.
    private func RaycastPeers(origin: Vector4, dir: Vector4) -> Uint32 {
        let best: Uint32 = 0u;
        let bestT: Float = this.HIT_MAX_RANGE;
        let des = GameInstance.GetDynamicEntitySystem();

        let i = 0;
        while i < ArraySize(this.m_puppets) {
            let peer = this.m_puppets[i];
            if peer.alive {
                let entity = des.GetEntity(peer.entityId);
                let puppet = entity as ScriptedPuppet;
                if IsDefined(puppet) {
                    let center = puppet.GetWorldPosition();
                    // Project the puppet center onto the ray. t is distance along the ray.
                    let ox = center.X - origin.X;
                    let oy = center.Y - origin.Y;
                    let oz = center.Z - origin.Z;
                    let t = ox * dir.X + oy * dir.Y + oz * dir.Z;
                    if t > 0.0 && t <= bestT {
                        // Closest point on the ray to the center, and its perpendicular distance.
                        let cx = origin.X + dir.X * t;
                        let cy = origin.Y + dir.Y * t;
                        let cz = origin.Z + dir.Z * t;
                        let dx = center.X - cx;
                        let dy = center.Y - cy;
                        let dz = center.Z - cz;
                        let perp = SqrtF(dx * dx + dy * dy + dz * dz);
                        if perp <= this.HIT_RAY_RADIUS {
                            best = peer.netId;
                            bestT = t; // prefer the nearest along the ray
                        }
                    }
                }
            }
            i += 1;
        }
        return best;
    }

    // ---- Phase 3 (PvP): inbound combat handlers (called from C++ OnNetUpdate) --------------

    // A remote player fired: draw a cosmetic tracer/muzzle from the shooter's puppet along the
    // shot ray. Minimal Phase-3 effect: spawn an impact/tracer VFX at the muzzle. (No damage — the
    // host's PlayerHealth is the only thing that moves a health bar.)
    public func OnNetPlayerFire(shooterNetId: Uint32, origin: Vector4, dir: Vector4, weaponTweakID: Uint64) -> Void {
        // Source the cosmetic effect from the shooter's puppet if we track it; otherwise no puppet
        // to play it on (the tracer is skipped — purely cosmetic, host still governs damage).
        let idx = this.FindPeerIndex(shooterNetId);
        if idx != -1 {
            let entity = GameInstance.GetDynamicEntitySystem().GetEntity(this.m_puppets[idx].entityId);
            let puppet = entity as ScriptedPuppet;
            if IsDefined(puppet) {
                this.PlayTracerVFX(puppet, origin, dir);
            }
        }
        FTLog(s"[cyber.rest] Remote fire from netId \(shooterNetId)");
    }

    // Play a short cosmetic muzzle/tracer effect for a remote shot. Minimal Phase-3 effect: fire a
    // gameplay effect event on the shooter's puppet (so it plays at the puppet, roughly at the
    // muzzle). GameObjectEffectHelper.StartEffectEvent(GameObject, CName) is the game's own path
    // (verified against decompiled muppet.swift, e.g. n"fx_damage_high").
    //
    // NOTE (needs in-game validation): the exact effect CName that reads as a muzzle flash/tracer
    // on an NPC puppet must be confirmed live; a wrong CName just no-ops the cosmetic (non-fatal).
    // A fully world-anchored tracer beam along `dir` is deferred to a later phase.
    private func PlayTracerVFX(shooterPuppet: ref<GameObject>, origin: Vector4, dir: Vector4) -> Void {
        if !IsDefined(shooterPuppet) {
            return;
        }
        GameObjectEffectHelper.StartEffectEvent(shooterPuppet, n"shootProjectile");
    }

    // The host's authoritative HP for a player. If it's a remote puppet, stash it (for the bar /
    // death gating). If it's OUR local V, apply the delta to the real player health so networked
    // damage actually hurts us. This is the ONLY path that changes health.
    public func OnNetPlayerHealth(netId: Uint32, hp: Float) -> Void {
        if netId == this.GetLocalNetId() {
            this.ApplyLocalHealth(hp);
            return;
        }
        let idx = this.FindPeerIndex(netId);
        if idx == -1 {
            return; // no puppet (yet); ignore
        }
        let peer = this.m_puppets[idx];
        peer.hp = hp;
        this.m_puppets[idx] = peer;
        FTLog(s"[cyber.rest] netId \(netId) hp -> \(hp)");
    }

    // Drive the local V's real Health stat pool toward the host's authoritative value. Health in
    // the StatPoolsSystem is a PERCENTAGE (0-100); our networked hp is on the same 0..MAX_HEALTH
    // (100) scale, so it maps 1:1 to a percentage — no max-point conversion needed. We request the
    // percentage delta through RequestChangingStatPoolValue with percToPoint=true, exactly the form
    // the game's own DamageManager.DrainStatPool uses (verified against decompiled statPoolsManager).
    //
    // NOTE (needs in-game validation): that a negative percentage delta with percToPoint=true damages
    // the local player (and drives the real death flow at 0) should be confirmed live; the call form
    // itself matches shipped game code.
    private func ApplyLocalHealth(hp: Float) -> Void {
        if !this.m_hasGI {
            return; // player not spawned yet -> no local health pool to drive
        }
        let player = GetPlayer(this.GetGameInstance());
        if !IsDefined(player) {
            return;
        }
        let sps = GameInstance.GetStatPoolsSystem(this.GetGameInstance());
        if !IsDefined(sps) {
            return;
        }
        let id = Cast<StatsObjectID>(player.GetEntityID());
        let curPct = sps.GetStatPoolValue(id, gamedataStatPoolType.Health, false); // 0-100
        let targetPct = hp; // hp is already 0..100 (MAX_HEALTH)
        let deltaPct = targetPct - curPct;
        if AbsF(deltaPct) < 0.5 {
            return; // already in sync (avoid fighting the game's own regen jitter)
        }
        // 5-arg form: (id, type, percentDelta, source, percToPoint=true). Matches DrainStatPool.
        sps.RequestChangingStatPoolValue(id, gamedataStatPoolType.Health, deltaPct, player, true);
        FTLog(s"[cyber.rest] local V health -> \(hp) (deltaPct \(deltaPct))");
    }

    // The host declared a player dead. Ragdoll/kill the puppet (or the local V if it's us).
    public func OnNetPlayerDeath(netId: Uint32) -> Void {
        if netId == this.GetLocalNetId() {
            // Local V death is driven by its own health hitting 0 via ApplyLocalHealth; nothing
            // extra to force here (the game runs the real death flow). Log for visibility.
            FTLog("[cyber.rest] local V died (host-declared)");
            return;
        }
        let idx = this.FindPeerIndex(netId);
        if idx == -1 {
            return;
        }
        let peer = this.m_puppets[idx];
        peer.alive = false;
        peer.hp = 0.0;

        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(peer.entityId);
        let puppet = entity as ScriptedPuppet;
        if IsDefined(puppet) {
            this.KillPuppet(puppet, peer.lastCmd);
        }
        peer.lastCmd = null; // the move (if any) was cancelled inside KillPuppet
        this.m_puppets[idx] = peer;
        FTLog(s"[cyber.rest] netId \(netId) died -> ragdoll");
    }

    // Force a puppet into its death/ragdoll state. Cancels its active move first (via the tracked
    // lastCmd, the same CancelCommand path the puppet driver uses) so it doesn't keep walking, then
    // drains its whole Health pool so the game runs the real defeat/ragdoll. Uses the percentage
    // form (drain the current percentage to reach 0), matching DamageManager.DrainStatPool.
    //
    // NOTE (needs in-game validation): draining the puppet's Health pool to 0 via the StatPoolsSystem
    // is the supported way to defeat an NPC; that it triggers the ragdoll must be confirmed live.
    private func KillPuppet(puppet: ref<ScriptedPuppet>, lastCmd: ref<AICommand>) -> Void {
        let ai = puppet.GetAIControllerComponent();
        if IsDefined(ai) && IsDefined(lastCmd) {
            ai.CancelCommand(lastCmd);
        }
        let sps = GameInstance.GetStatPoolsSystem(this.GetGameInstance());
        if IsDefined(sps) {
            let id = Cast<StatsObjectID>(puppet.GetEntityID());
            let curPct = sps.GetStatPoolValue(id, gamedataStatPoolType.Health, false); // 0-100
            if curPct > 0.0 {
                // Negative full-percentage delta -> 0 HP. percToPoint=true (percentage form).
                sps.RequestChangingStatPoolValue(id, gamedataStatPoolType.Health, -curPct, puppet, true);
            }
        }
    }

    // The host respawned a player. Restore alive state + re-place the puppet (or teleport the local
    // V). Full HP was already restored via the PlayerHealth broadcast that accompanies this.
    public func OnNetPlayerRespawn(netId: Uint32, position: Vector4) -> Void {
        if netId == this.GetLocalNetId() {
            // Local V respawn: the health broadcast already refilled us. If a real spawn point is
            // provided (non-origin), teleport there via the teleportation facility (the game's
            // supported player-teleport path, verified in the reference). Orientation is EulerAngles.
            if this.m_hasGI && (position.X != 0.0 || position.Y != 0.0 || position.Z != 0.0) {
                let player = GetPlayer(this.GetGameInstance());
                if IsDefined(player) {
                    let orient = Quaternion.ToEulerAngles(player.GetWorldOrientation());
                    GameInstance.GetTeleportationFacility(this.GetGameInstance()).Teleport(player, position, orient);
                }
            }
            FTLog("[cyber.rest] local V respawned");
            return;
        }
        let idx = this.FindPeerIndex(netId);
        if idx == -1 {
            return;
        }
        let peer = this.m_puppets[idx];
        peer.alive = true;
        peer.hp = this.MAX_HEALTH;
        peer.lastCmd = null;   // drop the stale command so the next transform re-drives cleanly
        this.m_puppets[idx] = peer;

        // Re-place the puppet. If the host sent a real point, teleport there; otherwise the next
        // OnNetPlayerTransform will hard-correct it (fresh-spawn/large-drift path).
        let entity = GameInstance.GetDynamicEntitySystem().GetEntity(peer.entityId);
        let puppet = entity as ScriptedPuppet;
        if IsDefined(puppet) && (position.X != 0.0 || position.Y != 0.0 || position.Z != 0.0) {
            this.TeleportPuppet(puppet, position, 0.0);
        }
        FTLog(s"[cyber.rest] netId \(netId) respawned");
    }

    // ---- Phase 4 (PERSISTENCE): stats + saved-position restore -----------------------------

    // The host's authoritative persistent K/D for a player (broadcast on kill/death + replayed on
    // join). Cache it: for a remote peer on its CyberRestPeer, and for our own local player in
    // m_localKills/m_localDeaths (so a scoreboard UI in a later phase can read all of it). This is
    // the ONLY path that changes stats — the host owns them, keyed by the player's stable GUID.
    public func OnNetPlayerStats(netId: Uint32, kills: Uint32, deaths: Uint32) -> Void {
        if netId == this.GetLocalNetId() {
            this.m_localKills = kills;
            this.m_localDeaths = deaths;
            FTLog(s"[cyber.rest] my stats -> \(kills)K / \(deaths)D");
            return;
        }
        let idx = this.FindPeerIndex(netId);
        if idx == -1 {
            return; // no puppet (yet); the join replay will re-send once it spawns
        }
        let peer = this.m_puppets[idx];
        peer.kills = kills;
        peer.deaths = deaths;
        this.m_puppets[idx] = peer;
        FTLog(s"[cyber.rest] netId \(netId) stats -> \(kills)K / \(deaths)D");
    }

    // The host restored OUR saved position on rejoin (returning player). Phase 5: this used to
    // teleport immediately, which is exactly the "world may not be streamed on the connect frame"
    // bug flagged here — so we now BUFFER the position through the world-ready gate. If the world is
    // already ready (mid-session rejoin), teleport now; otherwise OnLocalWorldReady fires it once the
    // player is stably world-attached, and it takes precedence over the shared-spawn teleport (a
    // returning player resumes where they left off rather than being dumped at the shared spawn).
    public func OnNetRestorePosition(netId: Uint32, position: Vector4) -> Void {
        if netId != this.GetLocalNetId() {
            return; // this message is only meaningful for our own local player
        }
        if position.X == 0.0 && position.Y == 0.0 && position.Z == 0.0 {
            return; // no real saved position
        }

        // Buffer it so OnLocalWorldReady restores it (wins over shared spawn). Also latch the shared
        // spawn as "done" so a returning player is never yanked to the shared spawn — not even for a
        // frame if this restore packet lands AFTER the world-ready tick already fired shared-spawn
        // (network timing), and not if the buffered restore is ever cleared before it applies.
        this.m_pendingRestorePos = position;
        this.m_pendingRestore = true;
        this.m_sharedSpawnDone = true;

        if this.m_worldReady {
            // Already streamed in (rejoin mid-session): apply immediately via the same gate path.
            this.OnLocalWorldReady();
        } else {
            FTLog(s"[cyber.rest] buffered saved-position restore (\(position.X), \(position.Y), \(position.Z)) until world-ready");
        }
    }

    // ---- small helpers --------------------------------------------------------------------

    // Index of the peer with this netId in m_puppets, or -1.
    private func FindPeerIndex(netId: Uint32) -> Int32 {
        let i = 0;
        while i < ArraySize(this.m_puppets) {
            if this.m_puppets[i].netId == netId {
                return i;
            }
            i += 1;
        }
        return -1;
    }
}

// One tracked remote player: its wire netId, its spawned puppet's EntityID, the last AICommand
// we issued to it (so MoveOrAnimatePuppet can avoid cancel-thrash / chain walks), and whether
// we've dressed the puppet in its current networked look (Phase 2; reset when the look changes
// so it re-applies).
public struct CyberRestPeer {
    public let netId: Uint32;
    public let entityId: EntityID;
    public let lastCmd: ref<AICommand>;
    public let appearanceApplied: Bool;
    // Phase 3 (PvP): last authoritative HP the host told us (for local health-bar / death gating)
    // and whether the puppet is currently in the dead/ragdoll state (so we don't re-drive it).
    public let hp: Float;
    public let alive: Bool;
    // Phase 4 (PERSISTENCE): this peer's host-authoritative lifetime K/D (for a scoreboard).
    public let kills: Uint32;
    public let deaths: Uint32;
    // Phase 5 (WORLD STREAMING): the puppet is created lazily, only once the peer is in the local
    // V's relevancy radius AND there's loaded ground to snap onto. Until then the slot exists but no
    // entity does. `spawned` = a live puppet entity exists (entityId valid); `deferred` = waiting to
    // (re)spawn (out of range / no streamed ground); `hasTarget`/`targetPos` = the latest networked
    // position (set by OnNetPlayerTransform) that drives the relevancy check + the spawn placement.
    public let spawned: Bool;
    public let deferred: Bool;
    public let hasTarget: Bool;
    public let targetPos: Vector4;
}

// Accessor for our native game system, mirroring the proven red-lib pattern
// (GameInstance.GetXxxSystem() for an IGameSystem-derived native class).
// Usage: GameInstance.GetCyberRestSystem().HostGame(Cast<Uint16>(7777u));
@addMethod(GameInstance)
public static native func GetCyberRestSystem() -> ref<CyberRestSystem>

// ---- Phase 5 (WORLD STREAMING): world-ready fast-path arm ------------------------------------
//
// OnMakePlayerVisibleAfterSpawn fires at the END of the post-spawn grace period (the engine's
// strongest "the player is fully spawned and visible" edge). We use it only to ARM the world-ready
// gate's fast path — the gate in CaptureLocalTransform still requires GetPlayer to be stably defined
// before it fires the shared-spawn teleport, so this can only ever shorten the debounce, never fire
// on a null player. Signature verified verbatim against the redmodding "things to hook" wiki.
//
// PITFALL (handled): this event does NOT fire when you connect while already in-world (hot-join /
// mid-session host), so it is a fast path only — the poll-until-stable GetPlayer fallback in the
// tick gate is the guaranteed trigger. Keep this wrapper body TRIVIAL: GameObject/PlayerPuppet
// spawn overrides are hot and have historically crashed CET on game reload when given heavy bodies.
@wrapMethod(PlayerPuppet)
protected cb func OnMakePlayerVisibleAfterSpawn(evt: ref<EndGracePeriodAfterSpawn>) -> Bool {
    let result = wrappedMethod(evt);
    let mp = GameInstance.GetCyberRestSystem();
    if IsDefined(mp) {
        // `this` is the local PlayerPuppet (a GameObject) here, so GetGame() hands us the session
        // GameInstance — the only place gameIGameSystem can get one. Cache it before anything else
        // so the per-frame paths (which no-op until m_hasGI) can run.
        mp.CacheGameInstance(this.GetGame());
        mp.NotifyLocalSpawned(); // just sets a bool; all real work happens in the tick gate
        mp.EnsureInputHooks();   // MVP: arm F2 = Host / F3 = Join once the player (and Codeware) exist
    }
    return result;
}

// ---- Phase 3 (PvP): local weapon-fire hook --------------------------------------------------
//
// Hook the projectile shoot event (the reference proved this @wrapMethod chains fine) and forward
// the LOCAL player's shots into CyberRestSystem so it can emit a cosmetic PlayerFire + do coarse
// hit detection. The gameprojectileShootEvent carries (via its SetUpEvent base, verified against
// the RED4ext-generated struct):
//   owner        WeakHandle<GameObject>  -- the shooter
//   weapon       WeakHandle<GameObject>  -- the weapon object
//   startPoint   Vector4                 -- world-space shot origin
//   startVelocity Vector4                -- direction * speed (we pass it as the aim direction)
//
// LOCAL-ONLY guard: OnShoot also fires for NPC/other shots. We compare eventData.owner to the
// local player. If owner can't be resolved we accept it (the host arbitrates damage, so a stray
// cosmetic tracer is harmless).
//
// NOTE (needs in-game validation): that startVelocity is a usable aim direction for hitscan-style
// weapons should be confirmed live; the field names + owner/weapon plumbing match the SDK structs.
@wrapMethod(BaseProjectile)
protected cb func OnShoot(eventData: ref<gameprojectileShootEvent>) -> Bool {
    let result = wrappedMethod(eventData);

    let mp = GameInstance.GetCyberRestSystem();
    if IsDefined(mp) && mp.IsConnected() && IsDefined(eventData) {
        // this.GetGame() is available on BaseProjectile (it extends ItemObject); use it to resolve
        // the local player without reaching into the system's game instance. Also (cheaply) refresh
        // the system's cached GameInstance here — a hot-join backup for the rare case where the
        // player-spawn hook didn't run this session (the value is stable, so re-caching is a no-op).
        mp.CacheGameInstance(this.GetGame());
        if CyberRestFire.IsLocalPlayerShot(this.GetGame(), eventData) {
            let origin = eventData.startPoint;
            let dir = eventData.startVelocity; // direction*speed; OnNetLocalShoot normalizes it
            let weaponHash = CyberRestFire.WeaponHashOf(eventData);
            mp.OnNetLocalShoot(origin, dir, weaponHash);
        }
    }
    return result;
}

// Small static helpers kept off the hooked method so the wrapper stays lean + testable.
public abstract class CyberRestFire {
    // True if this shot's owner is the local player. Defensive — accepts (returns true) if the
    // owner can't be resolved, since the host arbitrates and a stray cosmetic tracer is harmless.
    public static func IsLocalPlayerShot(gi: GameInstance, eventData: ref<gameprojectileShootEvent>) -> Bool {
        let localPlayer = GetPlayer(gi);
        if !IsDefined(localPlayer) {
            return false;
        }
        let owner = eventData.owner;
        if IsDefined(owner) {
            return owner.GetEntityID() == localPlayer.GetEntityID();
        }
        return true; // couldn't resolve owner -> accept (host arbitrates damage)
    }

    // Best-effort weapon TweakDBID hash for the fired shot (0 if unknown). Cosmetic hint only.
    public static func WeaponHashOf(eventData: ref<gameprojectileShootEvent>) -> Uint64 {
        let weapon = eventData.weapon as WeaponObject;
        if IsDefined(weapon) {
            return TDBID.ToNumber(ItemID.GetTDBID(weapon.GetItemID()));
        }
        return 0ul;
    }
}
