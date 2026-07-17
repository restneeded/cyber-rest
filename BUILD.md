# Building cyber.rest (Phase 0 client plugin)

`cyber.rest` is an original, from-scratch Cyberpunk 2077 multiplayer framework
(MIT © restneeded). Phase 0 is a **compilable skeleton**: it adds a "Multiplayer"
entry to the main menu, exposes redscript-callable `HostGame` / `JoinGame` /
`IsConnected` natives, and runs an in-process GameNetworkingSockets listen-server (host)
or client (join) on a worker thread that performs a tiny hello/handshake and logs the
connection-state changes. There is **no** replication, puppets, or PvP yet.

## Layout

```
client/red4ext/
  CMakeLists.txt          top-level: SDK + red-lib + GNS, adds src/
  CMakePresets.json       ninja-vcpkg configure/build/test presets
  vcpkg.json              declares the gamenetworkingsockets dependency
  .gitmodules             deps/RED4ext.SDK, deps/red-lib
  deps/
    RED4ext.SDK/          git submodule (WopsS/RED4ext.SDK)
    red-lib/              git submodule (psiberx/cp2077-red-lib)
  src/
    CMakeLists.txt        the CyberRest SHARED target (-> CyberRest.dll)
    Main.hpp / Main.cpp   RED4ext entry (Query/Main/Supports) + RTTI registration
    PluginContext.hpp     tiny logging facade over the SDK logger
    NetCore.hpp / .cpp    GNS listen-server + client + hello handshake on a worker thread
    Protocol.hpp          compact [tag][POD] wire protocol (presence / appearance / PvP / identity)
    CyberRestSystem.hpp / .cpp   native IGameSystem exposing the natives to redscript
    Identity.hpp / .cpp   (Phase 4) stable per-player GUID + plugin-dir/save-file paths
    HostStore.hpp / .cpp  (Phase 4) HOST-only JSON persistence (GUID -> pos/appearance/K-D)
scripts/CyberRest/
  CyberRestSystem.reds    native class declaration + GetCyberRestSystem() accessor
  MainMenu.reds           the "Multiplayer" menu entry + Phase-0 click hook
.github/workflows/build.yml   windows-latest vcpkg + CMake CI, uploads the built DLL
```

## Prerequisites

- **Windows** (the plugin is a Windows DLL loaded by RED4ext).
- **CMake ≥ 3.21**, **Ninja**, and the **MSVC** toolchain (Visual Studio 2022 Build
  Tools). `lukka/get-cmake` provides CMake+Ninja in CI.
- **vcpkg** with `VCPKG_ROOT` set. The `gamenetworkingsockets` port and its transitive
  deps (OpenSSL, protobuf) are fetched from the manifest automatically.

## First checkout — initialise submodules

The build needs both submodules populated (`deps/RED4ext.SDK`, `deps/red-lib`):

```sh
git clone https://github.com/restneeded/cyber-rest.git
cd cyber-rest
git submodule update --init --recursive
```

If you already cloned without `--recursive`:

```sh
git submodule update --init --recursive
```

## Build (matches CI)

From `client/red4ext/`:

```sh
# Configure (uses the vcpkg toolchain via the preset; VCPKG_ROOT must be set)
cmake --preset ninja-vcpkg

# Build
cmake --build --preset ninja-vcpkg
```

The output DLL is written to
`client/red4ext/build/ninja-vcpkg/src/CyberRest.dll`.

> CI runs the exact same two preset steps on `windows-latest` and uploads
> `CyberRest.dll` as the `CyberRest-plugin` artifact. Compiling in CI is the real
> correctness gate for this phase.

## Installing into the game (manual, for local testing)

1. Copy `CyberRest.dll` to
   `Cyberpunk 2077\red4ext\plugins\CyberRest\CyberRest.dll`.
2. Copy `scripts/CyberRest/*.reds` to
   `Cyberpunk 2077\r6\scripts\CyberRest\`.
3. Requires [RED4ext](https://github.com/WopsS/RED4ext) and
   [redscript](https://github.com/jac3km4/redscript) installed.

Launch the game, open the main menu, and click **Multiplayer**. Watch
`red4ext/logs/` — hosting logs the listen server coming up and, on a client connect,
the hello/hello-ack handshake and `state -> Connected`.

## Notes / limitations (Phase 0)

- The menu click currently auto-hosts on port `7777` and reuses the base-game
  `OnBuyGame` event to guarantee a live handler. A real host/join screen and a custom
  menu event are deferred to a later phase.
- `FTLog` in the redscript may require the small `Logs.reds` snippet in `r6/scripts` on
  game ≥ 2.01 to resolve at compile time (redscript stdlib quirk).
- GameNetworkingSockets is single-threaded; all socket calls happen on the one NetCore
  worker thread. Do not call into it from other threads.
