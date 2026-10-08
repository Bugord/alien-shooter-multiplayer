# asmp-server

Relay server, a 32-bit Windows executable built by
[diagnostics/build.ps1](../diagnostics/build.ps1) with `-Server` into
`diagnostics/build/asmp-server.exe`.

```powershell
.\diagnostics\build\asmp-server.exe 27020
```

The only argument is the UDP port. The server accepts up to four clients over
[epnet](../common/epnet/README.md), prefixes each relayed state and shot packet
with the sender ID and connection generation, and rejects invalid lengths,
versions, field ranges and older sequence numbers.

| File | Contents |
| --- | --- |
| [src/main.c](src/main.c) | Entry point and tick loop |
| [src/server.c](src/server.c) | Join handshake, validation and relay |

Packet formats are in [common/src/protocol.h](../common/src/protocol.h) and
[multiplayer_protocol.h](../common/src/multiplayer_protocol.h). Rebuild the server
together with all clients when the format changes. See
[Architecture and behavior](../docs/architecture.md) for the protocol details.
