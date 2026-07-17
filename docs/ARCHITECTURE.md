# Architecture

`cyber.rest` is a **single mod** that turns one player's game into a small multiplayer session others join. It is built from scratch for one specific shape: **host-on-your-own-PC, ~4 players, free-roam + PvP, indie-simple.** That constraint is what keeps it small.

## The model: a listen-server

There is no Cyberpunk dedicated-server binary, and we don't want one. Instead the **host's own game process runs the authoritative server in-process** (a background thread), while also being a normal client. This is the classic *listen-server* (Minecraft-LAN, most co-op indies).

- **Host is authoritative** → PvP hit/damage validation is trivial and fair; no anti-cheat needed for a friends group.
- **Session is ephemeral** → it exists while the host plays; the world is **saved locally on the host** and restored next time (that's what "persistent" means here — not always-online).
- **Golden rule:** never block the game thread. All netcode runs on a worker thread; state changes are marshaled onto the game thread.

## Three layers

```
┌───────────────────────── HOST'S GAME (also the server) ─────────────────────────┐
│  RED4ext C++ plugin                                                              │
│    • GameNetworkingSockets  — UDP transport, encryption, NAT traversal           │
│    • in-process listen-server thread — authoritative world/player state          │
│    • client — connects the local player like everyone else                       │
│  redscript                                                                       │
│    • "Multiplayer" entry on the main menu (SingleplayerMenuGameController)        │
│    • host / join UI (create a session, or join by code)                          │
│    • replication glue — spawn & drive remote-player puppets                      │
│  Codeware + DynamicEntitySystem — puppet spawn/despawn/query                      │
└───────────────────────────────────┬─────────────────────────────────────────────┘
                                     │  GNS over UDP (ICE punchthrough / relay)
                     ┌───────────────┴───────────────┐
                 friend #2                        friend #3/#4
              (client only)                     (client only)
```

## Key pieces

### 1. Start-menu entry
Hook `SingleplayerMenuGameController.PopulateMenuItemList` (redscript `@wrapMethod`) and add a menu item via `AddMenuItem("Multiplayer", n"<event>")`, then open a custom `inkWidget` screen for **Host** / **Join by code**.

### 2. In-process server
On **Host Game**, the plugin starts a GameNetworkingSockets listen socket on a worker thread and begins ticking the authoritative state (players, positions, PvP, spawned entities). The local player connects to it like any client.

### 3. Joining without port forwarding
The host registers with a small **signaling / rendezvous server** we run (cheap VPS). Friends "Join" with a short code; GNS performs **ICE NAT punchthrough** with **relay fallback** so it connects without router config.
- **Ship-now fallback:** a UDP tunnel (e.g. playit.gg) requiring zero code.
- **Target:** GNS-ICE + our signaling server for a true two-click join, no external tool.

### 4. Replication
Remote players are **puppets** spawned via `DynamicEntitySystem`, driven by AI move commands so they animate (walk/run/sprint) instead of sliding; position/velocity/locomotion-state/appearance are synced on a compact wire protocol. PvP = fire/hit events + host-validated health.

### 5. Persistence
The host keeps a local store (JSON or SQLite): player identities (stable GUID), positions, PvP stats, spawned state. Loaded on session start, saved on change. If the host is offline, that world is offline — the expected friend-hosted-world tradeoff.

## Wire protocol (planned)
A small, order-stable binary protocol shared between client and server. Serverbound: join, position+velocity+loco-state+tick, fire/hit, equip. Clientbound: spawn/despawn entity, movement sync, health/damage, appearance. Kept lean — this is a 4-player listen-server, not an MMO.

## Non-goals
Co-op missions, races, quest sync, dedicated hosting, hundreds of players, anti-cheat. Deliberately out of scope to stay simple.
