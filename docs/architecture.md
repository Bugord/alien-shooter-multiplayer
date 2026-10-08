# Architecture and behavior

How the Steam multiplayer runtime is layered and how it behaves. For building
and running it, see [Build, test and review](../diagnostics/README.md).

| Layer | Responsibilities |
| --- | --- |
| `asmp-dll/src/game/` | Verified Steam EXE layout/probe (`steam_profile.h`), actor factory/destructor, movement/combat API, map/text/menu bindings, shared slot helper, MAP tick/load, MAN action and D3D9/display hooks |
| `asmp-dll/src/multiplayer/client/state_client.*` | Original epnet connection/handshake, packet validation, peer state/names and shot queues; no native entities |
| `asmp-dll/src/multiplayer/` | Production startup/rollback/shutdown and worker loop, session and remote-player state machines, game-thread application, bounded worker handoff |
| `common/src/protocol.h` | Explicit state/shot codecs and normalized map keys; `multiplayer_protocol.h` holds the join/name handshake |
| `common/epnet/`, `asmp-server/` | Existing transport and server, with validated state/shot relays |
| `diagnostics/src/` | Test DLL configuration and logging, frame observer/queue, read-only/dummy harness, launcher and optional test peer |

Remote replicas have explicit idle, waiting, spawning, spawned, backoff and
abandoned states. The native factory owns the created MAN; its child chain owns
the attached name text and destroys it with the MAN. Private VIDs stay owned by
the coordinator until native removal succeeds or the old world unloads. Replicas
use the local player's army and receive weapons on demand. Factory failures and
torso timeouts remove and retry with a two-second delay doubling to thirty
seconds. It applies velocity,
movement intent, position and independent leg/torso directions. Native MAN logic
selects idle/run and advances animation. Frames and timers are never replicated;
the animation ID in snapshots is only diagnostic. Weapon application precedes
torso lookup because the engine can replace the attachment.

Aim is captured from native action
0x25 and ammo-consuming attacks from action 0x5D, then queued separately from
snapshots. Incoming events invoke native action 0x25 on the replica. The game
thread never performs socket or file I/O. Names use native owned strings and are
positioned again after weapon changes. Health bars use native rectangles through
D3D9 EndScene; installation waits for the renderer to create its device. Pending
display installation retries at most every thirty ticks; failure is latched
and logged once.

State packets are version 3, 112 bytes: explicit big-endian words, IEEE 754
coordinates/velocity, signed 32-bit health/live ammo/weapon slot, tick,
movement/aim fields, nine stored ammo counters, normalized 64-bit map key and
world generation. State is published at most once every 33 ms. A 32-byte shot
event carries sequence, map key/generation, weapon and aim coordinates. The
server prefixes each relayed packet with sender ID and connection generation.
Rebuild the server and all clients together; older packet versions are rejected.

The relay rejects invalid lengths, versions, field ranges and duplicate/older
sequences, including wraparound. Peers expire after one second without state;
menus, dead players and snapshots older than 250 ms are inactive. Session/map
changes discard stale attacks and remove replicas after checking native list
ownership. The probe's map key, native map-start marker and observed load
generation identify the world; a reused GAME pointer does not. Local death
publishes inactive state but preserves the current world and its living peers.
Native entity operations and UI drawing stay on the game thread.
Bounded queues drop work rather than blocking that thread.

Health belongs to each player's local owner. Local damage to its replica is
suppressed, while damage to the real local player is unchanged. This replaces
the original prototype's temporary restore-to-110/death-suppression hack (see the
commit `1a0cf25`). Bars retain the prototype's maximum of 110 and clamp larger values; maximum-health
stats are not transmitted. The ammo value used for the selected weapon is its
live counter, not its potentially stale stored slot. Weapon slots are zero-based.

The original mod does not implement monster/world synchronization or a working
scoreboard. This stage adds neither, nor respawning, Steam invitations or
server-authoritative combat. The existing transport is unreliable: separate
shot events avoid snapshot loss of short-lived attacks but do not guarantee
delivery under packet loss. Each PC still simulates its own world.

