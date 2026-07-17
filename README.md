<div align="center">

# ▚ cyber.rest

### Night City, together.

**A lightweight, from-scratch multiplayer framework for Cyberpunk 2077.**
Add a **Multiplayer** option to your start menu → host a quick session on your own PC → your friends join in two clicks. No dedicated server. No port forwarding.

[![site](https://img.shields.io/badge/site-cyber.rest.ac-fcee0a?style=flat-square)](https://cyber.rest.ac)
[![license](https://img.shields.io/badge/license-MIT-00f0ff?style=flat-square)](LICENSE)
![status](https://img.shields.io/badge/status-early%20%C2%B7%20building-ff2e88?style=flat-square)

</div>

---

> [!NOTE]
> **Early / building the skeleton.** This is a from-scratch framework in active early development — not playable yet. Friends-only, non-commercial. You'll need a legal PC copy of Cyberpunk 2077.

## What it is

`cyber.rest` is our **own** Cyberpunk 2077 multiplayer framework — built from scratch, not a fork of anything. It's designed to feel like a small indie game: you don't rent a server or forward ports, you just **host a game and send your friends a code**.

- 🎮 **Multiplayer on the start screen** — one click from the main menu
- 🖥️ **Host on your own PC** — the host's game *is* the server; no dedicated box, no Docker, no port forwarding
- 👥 **Quick ~4-player sessions** — create or join like a normal co-op indie
- 🌃 **Free-roam + PvP** — no co-op missions, no races; just mess around in Night City together
- 💾 **Persistent** — your world (host-saved) remembers you between sessions
- 🧩 **From scratch** — our framework, our code, MIT

## How it works

```
Main menu  ──▶  "Multiplayer"  ──▶  Host Game ───────────────┐
                                     (in-process server        │
                                      spins up in YOUR game)    │
                                                                ▼
Friends  ──▶  "Join" (paste a code)  ──▶  NAT punchthrough  ──▶  same Night City
                                          (relay fallback,        up to ~4 players
                                           no port forwarding)     free-roam + PvP
                                                                   world saves locally
```

## Architecture

A **single mod**, three layers (details in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)):

| Layer | Tech | Job |
|---|---|---|
| Transport + server | **RED4ext** C++ plugin + **GameNetworkingSockets** | UDP netcode, NAT traversal, the in-process **listen-server** thread, and the client |
| Game logic + UI | **redscript** | the start-menu **Multiplayer** entry, host/join screens, in-game replication glue |
| Engine helpers | **Codeware**, `DynamicEntitySystem` | spawn & drive remote-player puppets |

## Roadmap

| Phase | Milestone | Status |
|------:|-----------|:------:|
| 0 | **Skeleton** — Multiplayer menu entry + host/join + in-process server handshake | ✅ Compiles |
| 1 | **Presence** — see each other walk / run / sprint | 🧪 Code in |
| 2 | **Appearance** — look like your own V | 🧪 Code in |
| 3 | **PvP** — shoot each other (host-authoritative, no anti-cheat needed) | 🧪 Code in |
| 4 | **Persistence** — the host's world remembers everyone | 🧪 Code in |
| ✦ | **Spawn/streaming manager** — auto-converge spawn, ground-snap, no "load-the-same-save" chore | 🧪 Code in |

> **Latest:** the full stack is written and cross-verified — Phase-0 skeleton (compiles on CI) plus Phases 1-4 (presence, appearance, PvP, host-saved persistence) and a **spawn/streaming manager** that auto-converges players to a shared spawn and ground-snaps remote puppets (so nobody falls through the map — no manual "everyone load the same save" ritual). All of it compiles together; **pending in-game bruteforce validation.** See [BUILD.md](BUILD.md).

## Requirements

Retail **Cyberpunk 2077** on PC, plus [RED4ext](https://github.com/WopsS/RED4ext) · [Redscript](https://github.com/jac3km4/redscript) · [Codeware](https://github.com/psiberx/cp2077-codeware).

## Why

The existing Cyberpunk multiplayer projects are invite-only, run-on-their-servers, and perpetually "soon." `cyber.rest` is the opposite: **open, self-hostable, and dead simple.** Host a game, send a code, mess around in Night City with your friends. That's the whole pitch.

## License

MIT © 2026 restneeded. Unaffiliated with, and not endorsed by, CD PROJEKT RED. Cyberpunk 2077 and all related marks belong to their respective owners — you must own a legal copy. No game assets are distributed here.
