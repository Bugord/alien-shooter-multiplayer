# alien-shooter-multiplayer
Alien Shooter is a single-player isometric top-down shooter released in 2003. This repository contains the results of reverse engineering and development in C, which enables network play for the game.

The mod targets the Steam release of Alien Shooter. It supports only the
executable with SHA256
`4DD960458D6FFFCC9D00E9E7BA492739FB6D530D4C0B302F1C6BAA8B55D9B142`
(`STEAM_EXE_SHA256` in [steam_profile.h](asmp-dll/src/game/steam_profile.h)).
Connection/map loading, remote players, movement and torso aim, weapons, ammo,
health, shot events, names and health bars work. Animations run in the native
engine from movement intent. Steam invitations, monster/world synchronization and
a scoreboard are not implemented. The DLL still uses the diagnostic entry point
during review.

The client for the original 2003 executable (GCC/MinGW build, EXE patcher,
`asmp.dll`) has been removed. It remains available in commit `1a0cf25`,
for example `git show 1a0cf25:asmp-dll/src/multiplayer/multiplayer.c`.

## Navigation
- [Repository navigation](#Repository-navigation)
- [Build and test](#Build-and-test)
- [Server-client architecture](#Server-client-architecture)
- [Demonstration](#Demonstration)

## Repository navigation
- [/asmp-dll/src/game](/asmp-dll/src/game) - Engine API for the Steam executable: verified addresses and layouts ([steam_profile.h](/asmp-dll/src/game/steam_profile.h)), probe, actor factory and combat API, menu/text bindings and vtable hooks.
- [/asmp-dll/src/multiplayer](/asmp-dll/src/multiplayer) - Production runtime: startup/shutdown, session and remote-player state machines, game-thread application.
  - [/asmp-dll/src/multiplayer/client](/asmp-dll/src/multiplayer/client) - Connection, packet validation and peer state; no native game entities.
- [/asmp-server](/asmp-server) - Relay server. The server is a 32-bit Windows executable.
- [/common](/common) - Code shared by the server and the client.
  - [/common/epnet](/common/epnet) - Networking library (submodule).
  - [/common/src/protocol.h](/common/src/protocol.h) - State and shot packet codecs.
  - [/common/src/multiplayer_protocol.h](/common/src/multiplayer_protocol.h) - Join and player-name packets.
- [/diagnostics](/diagnostics) - Build script, test launcher, unit tests and a headless test peer.
- [/game](/game) - Modified game files: menu markup (.men) and menu logic (.lgc).

## Build and test
Requires Visual Studio C++ x86 tools and the Windows SDK. From the repository
root in PowerShell:

```powershell
git clone --recursive https://github.com/ep1h/alien-shooter-multiplayer
cd alien-shooter-multiplayer
.\diagnostics\build.ps1 -Server
```

This builds `asmp-diag.dll`, the launcher, the test peer and `asmp-server.exe`
into `diagnostics/build/` and runs all tests. See
[build and test instructions](diagnostics/README.md) for preparing the test game
copy, one-PC and two-PC sessions.

## Server-client architecture
The server-client architecture is divided into two parts:
### 1. Low-level server-client
The [epnet](/common/epnet) library handles connection, disconnection and
connection maintenance. It contains no project-specific logic.
### 2. Top-level server-client
Extends the low-level server-client to provide functionality for online play.
Top-level server implementation: [/asmp-server](/asmp-server)
Top-level client implementation: [/asmp-dll/src/multiplayer/client](/asmp-dll/src/multiplayer/client)

## Demonstration
These recordings show the original 2003 version.
### Menu
https://user-images.githubusercontent.com/46194184/227988748-8c160bc6-3c59-44e1-b92a-78d5bff4617c.mp4

### Gameplay
https://user-images.githubusercontent.com/46194184/230725748-09ea9940-c1c2-4e04-9b91-7db1ca6da255.mp4
