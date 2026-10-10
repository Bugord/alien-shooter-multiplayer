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

The only argument is the UDP port. The server accepts up to four clients over
[epnet](../common/epnet/README.md), prefixes each relayed state and shot packet
with the sender ID and connection generation, and rejects invalid lengths,
versions, field ranges and older sequence numbers.

| File | Contents |
| --- | --- |
| [src/main.c](src/main.c) | Entry point and tick loop |
| [src/server.c](src/server.c) | Hello/welcome handshake, roster broadcast, validation and relay |

Packet formats are in [common/src/protocol.h](../common/src/protocol.h): state, shot, hello, welcome and
roster packets, each with explicit big-endian encoding. Rebuild the server
together with all clients when the format changes. See
[Architecture and behavior](../docs/architecture.md) for the protocol details.

The opt-in monster test uses [monster_protocol.h](../common/src/monster_protocol.h).
The first registered peer to claim authority owns its ID/session until the room
is empty. Other peers cannot publish monster state or events, and reconnecting
into the old ID does not inherit authority. There is no host migration. The
server validates the host's current world, sequences and packet fields; it caches
fresh state for late join, without replaying historical attacks.
Monster protocol v3 carries a persistent death variant and final pose (72-byte
state, 52-byte event); a client can recover death presentation from state alone.
Rebuild the server and clients together when upgrading this opt-in protocol.
A dead player with a valid world can continue publishing monsters. The optional `ASMP_MAP`
environment variable changes the validated welcome map (default
`maps\Level_01.map`); the managed monster test sets `maps\survive_01.map`.

Gallery wire v3 retains72-byte states/52-byte events and bounds monster IDs1..64
and stock types40/50/60/80. The relay stores latest state, expiry and event sequence
per ID; owner/session/world guards remain shared. Late join sends the bounded
current cache, without historical attack events. Rebuild clients and relay together.
