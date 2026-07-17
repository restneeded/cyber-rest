#pragma once

// cyber.rest — Phase 1 wire protocol.
//
// A deliberately tiny, order-stable binary protocol: every packet is
//
//     [ uint8_t type ][ fixed-size trivially-copyable payload struct ]
//
// with the payload memcpy'd straight out of / into the C++ struct. No varints, no
// strings, no schema library — every Phase-0..3 message is a POD of fixed layout, so
// (a) the same struct definition on both ends guarantees an identical layout under the
// same MSVC/x64 ABI (host and clients are the same DLL), and (b) a receiver validates a
// packet purely by matching its byte length against sizeof(struct) for the tag.
//
// IN-GAME ASSUMPTION (flagged): host and every client run this exact same plugin build,
// so struct layout/padding/endianness match. That holds for our own single-DLL project;
// it would NOT hold against a differently-compiled peer. If we ever ship mixed builds we
// must switch to an explicit little-endian field serializer. For now #pragma pack(1)
// removes padding so the layout is at least compiler-flag-independent within MSVC.
//
// netId is the host-assigned per-connection id (see NetCore). The host is authoritative:
// it stamps/relays these; clients only ever originate their OWN PlayerTransform (the host
// overwrites the netId on relay so a client cannot spoof another player's id).

#include <cstdint>
#include <cstring>
#include <type_traits> // std::is_trivially_copyable_v used by Serialize/Deserialize
#include <vector>

namespace cr
{
// One byte on the wire, first byte of every packet. Order is stable — only ever append.
enum class MsgType : uint8_t
{
    Invalid = 0,
    PlayerJoin = 1,       // a remote player exists / just joined  (payload: PlayerJoin)
    PlayerLeave = 2,      // a remote player left / disconnected   (payload: PlayerLeave)
    PlayerTransform = 3,  // a remote player's movement sample     (payload: PlayerTransform)
    PlayerAppearance = 4, // a remote player's look (gender + worn) (payload: PlayerAppearance)

    // Phase 3 (PvP). Host-authoritative combat.
    PlayerFire = 5,     // a player fired a shot (origin + direction) (payload: PlayerFire)
    PlayerHit = 6,      // shooter reports it hit a victim            (payload: PlayerHit)
    PlayerHealth = 7,   // host broadcasts a player's authoritative HP (payload: PlayerHealth)
    PlayerDeath = 8,    // host declares a player dead                (payload: PlayerDeath)
    PlayerRespawn = 9,  // host respawns a player at a position       (payload: PlayerRespawn)

    // Phase 4 (PERSISTENCE). Identity + host-saved world.
    PlayerHello = 10,      // client->host, FIRST msg after connect: stable player GUID (payload: PlayerHello)
    PlayerStats = 11,      // host broadcasts a player's persistent PvP stats (payload: PlayerStats)
    PlayerRestorePos = 12, // host->returning-client ONLY: teleport the local V to its saved position.
                           // REUSES the PlayerRespawn payload (netId + px,py,pz) — distinct tag so the
                           // receiver teleports the LOCAL V instead of treating it as a combat respawn.
};

// Phase 2 (APPEARANCE): body gender carried in PlayerAppearance.bodyGender. Mirrors the
// redscript BodyGender mapping in CyberRestSystem.reds. Kept as a raw u8 for a stable ABI.
//   0 unknown (fall back to the default record), 1 female, 2 male.
// "Body gender" here is the puppet's body type, NOT the character's voice/pronoun choice.
enum class BodyGender : uint8_t
{
    Unknown = 0,
    Female = 1,
    Male = 2,
};

// Number of fixed clothing slots we sync per player. Order is a FIXED, order-stable list of
// gamedataEquipmentArea -> AttachmentSlots pairs, defined once in CyberRestSystem.reds and
// mirrored by this index layout (verified against the decompiled equipmentSystem.script
// InitializeClothingSlotsInfo):
//   0 Outfit     (AttachmentSlots.Outfit)   -- a full outfit overrides the pieces below
//   1 OuterChest (AttachmentSlots.Torso)
//   2 InnerChest (AttachmentSlots.Chest)
//   3 Legs       (AttachmentSlots.Legs)
//   4 Feet       (AttachmentSlots.Feet)
//   5 Head       (AttachmentSlots.Head)
//   6 Face       (AttachmentSlots.Eyes)
// Each entry is a raw TweakDBID hash (TDBID.ToNumber on the wire; RED4ext::TweakDBID(value)
// reconstructs it losslessly on the receiver). 0 means "nothing equipped in this slot".
constexpr uint32_t kAppearanceSlots = 7;

// Phase 4 (PERSISTENCE). A stable per-player identity GUID string travels in PlayerHello and is
// the KEY the host persists a player's saved world state under. It is fixed-length on the wire (a
// char buffer, always NUL-terminated within the buffer) to keep every packet a trivially-copyable
// POD — same trick the reference's InitAuthServerBoundCSharp used for its fixed username[255].
// 40 bytes comfortably holds our "cr-<16 hex>" GUIDs (and a raw Steam64 or a UUID) with room to
// spare; the client always writes a terminator so a receiver can treat it as a C string safely.
constexpr uint32_t kGuidLen = 40;

// Bumped whenever the wire layout changes incompatibly. The host compares a client's PlayerHello
// protocolVersion against its own; a mismatch is logged (Phase 4 does not yet hard-reject, but the
// field is in place so a later phase can). Keep in lockstep across host + clients (same DLL).
constexpr uint32_t kProtocolVersion = 4;

// Compact locomotion classification carried in PlayerTransform.locoState. Mirrors the
// redscript LocoState mapping in CyberRestSystem.reds (kept in lockstep):
//   0 idle, 1 walk, 2 run, 3 sprint, 4 crouch, 5 jump, 6 fall.
enum class LocoState : uint8_t
{
    Idle = 0,
    Walk = 1,
    Run = 2,
    Sprint = 3,
    Crouch = 4,
    Jump = 5,
    Fall = 6,
};

#pragma pack(push, 1)

// A remote player appeared. Sent by the host: broadcast to everyone when a client joins,
// and also sent once per already-present player to a fresh newcomer so it sees the roster.
struct PlayerJoin
{
    uint32_t netId;
};

// A remote player disappeared (disconnect). Broadcast by the host to the survivors.
struct PlayerLeave
{
    uint32_t netId;
};

// A single movement sample for one player. A client sends this for ITSELF with netId
// left 0 (the host fills in the sender's real netId on relay); the host relays it to the
// OTHER clients with netId set. Fields:
//   px,py,pz  world position (metres)
//   yaw       facing yaw (degrees; convention validated in reds, facing-only)
//   vx,vy,vz  world-space linear velocity (m/s); .z is vertical
//   locoState one of LocoState, as a raw u8 for a stable ABI
//   tick      sender's monotonic tick (ordering/dedupe only; NOT a synced clock)
struct PlayerTransform
{
    uint32_t netId;
    float px, py, pz;
    float yaw;
    float vx, vy, vz;
    uint8_t locoState;
    uint64_t tick;
};

// A player's silhouette: body gender + the raw TweakDBID hashes of the clothing worn in each
// synced slot (see kAppearanceSlots / the slot layout above). Sent ONCE by a client when it
// connects (netId left 0 -> host stamps the real sender id, same anti-spoof rule as
// PlayerTransform). The host stores the latest per player and (a) forwards a newcomer's
// appearance to everyone else and (b) replays every already-present player's appearance to
// the newcomer, so everyone converges on the same look. Full face-morph is out of scope for
// Phase 2 — gender picks the base body record and the clothing items dress the puppet.
struct PlayerAppearance
{
    uint32_t netId;
    uint8_t bodyGender;               // one of BodyGender, raw u8
    uint8_t pad[3];                   // explicit padding so clothing[] is 8-byte aligned + layout is pinned
    uint64_t clothing[kAppearanceSlots]; // raw TweakDBID hashes; 0 = slot empty
};

// ---- Phase 3 (PvP) --------------------------------------------------------------------
//
// Combat is HOST-AUTHORITATIVE. Clients OBSERVE their local V shooting/hitting and REPORT it
// to the host (PlayerFire for the visual + PlayerHit for the damage claim); the host is the
// sole authority on HP, death and respawn (PlayerHealth/PlayerDeath/PlayerRespawn), which it
// broadcasts to everyone. A client never trusts another client's damage — only the host's HP.

// A player fired a weapon. Sent by the shooter (netId left 0 on a client -> host stamps the
// real sender id; anti-spoof, same rule as PlayerTransform) purely so the OTHER clients can
// play a muzzle/tracer VFX from the right puppet. Carries the shot ray so the receiver can aim
// the tracer:
//   weaponTweakID  raw TweakDBID hash of the weapon record (0 if unknown) — VFX/selection hint
//   ox,oy,oz       world-space muzzle/origin point of the shot
//   dx,dy,dz       normalized world-space shot direction
// This is a COSMETIC event: it never applies damage (that is PlayerHit -> host -> PlayerHealth).
struct PlayerFire
{
    uint32_t shooterNetId;
    uint64_t weaponTweakID;
    float ox, oy, oz;
    float dx, dy, dz;
};

// The shooter reports that its shot hit a victim. Sent by the shooter to the host (netId left 0
// on a client -> host stamps the authoritative shooterNetId). The host SANITY-CHECKS it loosely
// (known victim, damage within a per-shot cap, victim currently alive) then applies the damage
// to its authoritative HP and broadcasts the resulting PlayerHealth (and PlayerDeath if it hit
// 0). Clients NEVER apply damage directly from a PlayerHit — only from the host's PlayerHealth.
//   victimNetId  who the shooter claims to have hit
//   damage       claimed damage amount (host clamps to [0, kMaxHitDamage])
struct PlayerHit
{
    uint32_t shooterNetId;
    uint32_t victimNetId;
    float damage;
};

// The host's authoritative HP for a player, broadcast to everyone whenever it changes (after a
// hit, on respawn, etc.). hp is absolute health in [0, kMaxHealth]. The receiver applies it to
// the matching puppet (or, if netId == our own, to the local V). This is the ONLY message that
// moves a health bar — it is always host-originated.
struct PlayerHealth
{
    uint32_t netId;
    float hp;
};

// The host declares a player dead (hp reached 0). Broadcast to everyone. The receiver ragdolls/
// kills the matching puppet (or blacks out / kills the local V if it's us). A PlayerRespawn
// follows after kRespawnDelaySeconds.
struct PlayerDeath
{
    uint32_t netId;
};

// The host respawns a player at a position. Broadcast to everyone (HP is restored via a
// separate PlayerHealth the host sends alongside). The receiver re-places the puppet there (or
// teleports the local V if it's us). px,py,pz is the world-space respawn point.
struct PlayerRespawn
{
    uint32_t netId;
    float px, py, pz;
};

// ---- Phase 4 (PERSISTENCE) ------------------------------------------------------------
//
// Identity + persistent stats. The host keys everything it saves under a player's GUID.

// The FIRST message a client sends after its connection reaches Connected. Carries the client's
// stable, persistent identity GUID (a NUL-terminated string inside a fixed buffer) + the protocol
// version. The host maps the connection's netId <-> this GUID, then restores that GUID's saved
// world state (last position, appearance, PvP stats) if it has seen the GUID before. netId is left
// 0 by the client and is irrelevant here (the host already knows which connection this arrived on);
// it exists only so the struct has room for the host to note the assigned id if it ever echoes one.
struct PlayerHello
{
    uint32_t netId;                 // unused on the wire (host knows the connection); reserved
    uint32_t protocolVersion;       // == kProtocolVersion; mismatch is logged
    char guid[kGuidLen];            // stable identity, NUL-terminated within the buffer
};

// A player's persistent PvP stats, broadcast by the host whenever they change (a kill/death) and
// replayed to a newcomer for every present player (so scoreboards converge). Host-authoritative,
// exactly like PlayerHealth. kills/deaths are lifetime counters the host loads from / saves to the
// persistence store keyed by that player's GUID.
struct PlayerStats
{
    uint32_t netId;
    uint32_t kills;
    uint32_t deaths;
};

#pragma pack(pop)

// ---- Phase 3 combat tuning (host-side; shared so both ends agree on the constants) --------
// Full health for every player. HP is tracked absolute in [0, kMaxHealth]. 100 keeps the wire
// value human-readable and matches the game's default player health scale closely enough for a
// minimal PvP loop (real V health varies with build; exact value flagged for in-game tuning).
constexpr float kMaxHealth = 100.0f;
// Loose per-hit damage sanity cap the host clamps a claimed PlayerHit.damage to. Generous on
// purpose (host-authoritative already prevents HP forgery; this only blunts absurd claims).
constexpr float kMaxHitDamage = 100.0f;
// Seconds the host waits after a PlayerDeath before broadcasting the PlayerRespawn (+ full HP).
constexpr double kRespawnDelaySeconds = 5.0;

// ---- serialize / deserialize ----------------------------------------------------------
//
// Serialize = write the tag byte then the raw struct bytes into a fresh buffer.
// Deserialize = validate length against sizeof(T) for the given tag, then memcpy out.
// Both are header-only + trivial so NetCore (worker thread) and any test can share them.

template <typename T>
inline MsgType MsgTypeOf(); // specialized below, one per payload struct

template <>
inline MsgType MsgTypeOf<PlayerJoin>()
{
    return MsgType::PlayerJoin;
}
template <>
inline MsgType MsgTypeOf<PlayerLeave>()
{
    return MsgType::PlayerLeave;
}
template <>
inline MsgType MsgTypeOf<PlayerTransform>()
{
    return MsgType::PlayerTransform;
}
template <>
inline MsgType MsgTypeOf<PlayerAppearance>()
{
    return MsgType::PlayerAppearance;
}
template <>
inline MsgType MsgTypeOf<PlayerFire>()
{
    return MsgType::PlayerFire;
}
template <>
inline MsgType MsgTypeOf<PlayerHit>()
{
    return MsgType::PlayerHit;
}
template <>
inline MsgType MsgTypeOf<PlayerHealth>()
{
    return MsgType::PlayerHealth;
}
template <>
inline MsgType MsgTypeOf<PlayerDeath>()
{
    return MsgType::PlayerDeath;
}
template <>
inline MsgType MsgTypeOf<PlayerRespawn>()
{
    return MsgType::PlayerRespawn;
}
template <>
inline MsgType MsgTypeOf<PlayerHello>()
{
    return MsgType::PlayerHello;
}
template <>
inline MsgType MsgTypeOf<PlayerStats>()
{
    return MsgType::PlayerStats;
}

// Build a [tag][payload] byte buffer for a POD payload struct.
template <typename T>
inline std::vector<uint8_t> Serialize(const T& aPayload)
{
    static_assert(std::is_trivially_copyable_v<T>, "wire payloads must be trivially copyable");
    std::vector<uint8_t> buf(1 + sizeof(T));
    buf[0] = static_cast<uint8_t>(MsgTypeOf<T>());
    std::memcpy(buf.data() + 1, &aPayload, sizeof(T));
    return buf;
}

// Peek the message type of a received buffer (0-length -> Invalid).
inline MsgType PeekType(const void* aData, size_t aSize)
{
    if (aData == nullptr || aSize < 1)
    {
        return MsgType::Invalid;
    }
    return static_cast<MsgType>(static_cast<const uint8_t*>(aData)[0]);
}

// Try to read a payload struct out of a received buffer. Returns false if the buffer's
// length doesn't exactly match [tag + sizeof(T)] (guards against truncation / wrong tag).
template <typename T>
inline bool Deserialize(const void* aData, size_t aSize, T& aOut)
{
    static_assert(std::is_trivially_copyable_v<T>, "wire payloads must be trivially copyable");
    if (aData == nullptr || aSize != 1 + sizeof(T))
    {
        return false;
    }
    std::memcpy(&aOut, static_cast<const uint8_t*>(aData) + 1, sizeof(T));
    return true;
}
} // namespace cr
