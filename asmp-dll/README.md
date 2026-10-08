# asmp-dll

Client DLL for the Steam executable. It is built as `asmp-diag.dll` by
[diagnostics/build.ps1](../diagnostics/build.ps1) and injected by the
diagnostic launcher; see [Build, test and review](../diagnostics/README.md).

| Path | Contents |
| --- | --- |
| [src/game/](src/game) | Engine API: verified addresses and layouts ([steam_profile.h](src/game/steam_profile.h)), probe, actor factory and combat API, menu/text bindings, and the vtable hooks (tick, world load, action, display) |
| [src/multiplayer/](src/multiplayer) | Production runtime ([runtime.c](src/multiplayer/runtime.c)), session state machine ([session.c](src/multiplayer/session.c)) and remote-player coordinator ([multiplayer.c](src/multiplayer/multiplayer.c)) |
| [src/multiplayer/client/](src/multiplayer/client) | Connection, packet validation and peer state; no native game entities |

`game/` does not include `multiplayer/` headers; it receives callbacks instead.
Layers, threading and replica lifecycle are described in
[Architecture and behavior](../docs/architecture.md).
