# asmp-server

Relay server, a 32-bit Windows executable built by
[diagnostics/build.ps1](../diagnostics/build.ps1) with `-Server` into
`diagnostics/build/asmp-server.exe`.

```powershell
.\diagnostics\build\asmp-server.exe 27020
```

For a foreground relay in Git Bash, run `bash diagnostics/start-server.sh`
from the repository root (optional port argument, default 27020). Run
`bash diagnostics/start-clients.sh` in a second Git Bash terminal to launch both
prepared copies. See [quick launch](../diagnostics/README.md#quick-launch-from-git-bash).

The only argument is the UDP port. Before it opens the port, the server checks
it with an exclusive bind, because epnet binds with `SO_REUSEADDR` and would
share a port that another server already uses. It then prints one line that
the DLL's [server_process.c](../asmp-dll/src/multiplayer/server_process.c)
waits for: `ASMP_SERVER_READY port=N`, `ASMP_SERVER_PORT_IN_USE port=N`
(exit code 2) or `ASMP_SERVER_FAILED port=N` (exit code 1); a wrong argument
prints the usage (exit code 3).

The server accepts up to four clients over
[epnet](../common/epnet/README.md), prefixes each relayed state and shot packet
with the sender ID and connection generation, and rejects invalid lengths,
versions, field ranges and older sequence numbers. It keeps one room: host,
ready flags, the start and the load barrier (see
[Room and session](../docs/architecture.md#room-and-session)).

| File | Contents |
| --- | --- |
| [src/main.c](src/main.c) | Entry point and tick loop |
| [src/server.c](src/server.c) | Port check, hello/welcome handshake, roster and room broadcast, room requests, validation and relay |
| [src/lobby.c](src/lobby.c) | The room without sockets: join/leave, host, ready, settings, characters, start, load barrier and timeout |

Packet formats are in [common/src/protocol.h](../common/src/protocol.h): state, shot, hello, welcome and
roster packets, and in [common/src/lobby_protocol.h](../common/src/lobby_protocol.h): the room
snapshot and the ready, settings, start, loaded and character requests, each with explicit big-endian encoding. Rebuild the server
together with all clients when the format changes. See
[Architecture and behavior](../docs/architecture.md) for the protocol details.

The opt-in monster test uses [monster_protocol.h](../common/src/monster_protocol.h).
The first registered peer to claim authority owns its ID/session until the room
is empty. Other peers cannot publish monster state or events, and reconnecting
into the old ID does not inherit authority. There is no host migration. The
server validates the host's current world, sequences and packet fields; it caches
fresh state for late join, without replaying historical attacks.
Monster protocol v4 carries the current form, immutable original type, form
revision/phase, persistent death variant and final pose (84-byte state, 72-byte
event). A client can recover the current form and death presentation from state alone.
Rebuild the server and clients together when upgrading this opt-in protocol.
A dead player with a valid world can continue publishing monsters. The optional `ASMP_MAP`
environment variable changes the validated welcome map and the room's map (default
`maps\Level_01.map`); the managed monster test sets `maps\survive_01.map`.

The gallery bounds monster IDs1..64 and stock types40/50/60/70/80/90 plus their
frozen forms49/59/69/79/89/99. The relay rejects form rollback, changed original
types, resurrection and events for stale forms. It stores state, expiry and event sequence
per ID; owner/session/world guards remain shared. Late join sends the bounded
current cache, without historical attack events. Rebuild clients and relay together.
