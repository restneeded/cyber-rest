module CyberRest.Config

// ============================================================================
//  cyber.rest - multiplayer config.  Edit this file, then relaunch the game.
// ----------------------------------------------------------------------------
//  HOW TO PLAY (friends, free-roam):
//    1. Everyone launches the game and loads a save (get into Night City).
//    2. ONE person is the HOST: in-game, press  F2  to start hosting on 7777,
//       then give friends your address (see JOIN below).
//    3. Everyone else JOINS: put the host's address in CyberRestHostAddress()
//       below, relaunch, load a save, and press  F3  in-game to connect.
//       On connect you are teleported onto the host so you can see each other.
//
//  ADDRESS:
//    - same network (LAN):  the host's LAN IP, e.g. "192.168.1.50"
//    - over the internet:   the host's PUBLIC IP -- and the host must make
//                           UDP 7777 reachable: forward 7777/udp on the router,
//                           or run a free playit.gg UDP tunnel and share that.
//    "127.0.0.1" only works on the host's own machine (loopback test).
// ============================================================================

public func CyberRestHostAddress() -> String {
    return "127.0.0.1";
}

public func CyberRestPort() -> Uint16 {
    return Cast<Uint16>(7777u);
}
