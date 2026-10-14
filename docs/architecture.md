# Architecture and behavior

How the Steam multiplayer runtime is layered and how it behaves. For building
and running it, see [Build, test and review](../diagnostics/README.md).

| Layer | Responsibilities |
| --- | --- |
| `asmp-dll/src/game/` | Verified Steam EXE layout/probe (`steam_profile.h`), actor factory/destructor, movement/combat API, map/text/menu bindings, shared slot helper, MAP tick/load, MAN action and D3D9/display hooks |
| `asmp-dll/src/multiplayer/client/` | `state_client.*`: hello/welcome handshake, link state, roster names, room snapshots and room requests, packet validation, peer state/names and shot queues; `transport*.c`: the transport boundary and its UDP (epnet) backend; no native entities |
| `asmp-dll/src/multiplayer/` | Production startup/rollback/shutdown and worker loop, owned relay server process, session (menu, room, load barrier) and remote-player state machines, room request retries, game-thread application, bounded worker handoff |
| `common/src/protocol.h`, `lobby_protocol.h` | Explicit state, shot, hello, welcome, roster and room codecs, and normalized map keys |
| `common/epnet/`, `asmp-server/` | Existing transport and server, with validated state/shot relays and the room (`lobby.c`) |
| `diagnostics/src/` | Test DLL configuration and logging, frame observer/queue, read-only/dummy harness, launcher and optional test peer |

Remote replicas have explicit idle, waiting, spawning, spawned, backoff and
abandoned states. The native factory owns the created MAN and its child chain. Names are not
native objects: the display hook draws them with the game's `Render::DrawText`
above the health bar. Private VIDs stay owned by
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

State packets are version 4, 116 bytes: explicit big-endian words, IEEE 754
coordinates/velocity, signed 32-bit health and maximum health (0 = unknown), live ammo, weapon slot, tick,
movement/aim fields, nine stored ammo counters, normalized 64-bit map key and
world generation. State is published at most once every 33 ms. A 32-byte shot
event carries sequence, map key/generation, weapon and aim coordinates. The
server prefixes each relayed packet with sender ID and connection generation.
Rebuild the server and all clients together; older packet versions are rejected.

Each packet family has its own version: state 4, shot 1, handshake 1, room 1. After the
transport connects, the client repeats `MPT_C_HELLO` (version, length-prefixed
name of 1..15 printable ASCII characters) until the server answers
`MPT_S_WELCOME` (version, client id, capacity, validated level path). A client
is not registered, and its state and shots are ignored, until its hello is
accepted. The server broadcasts `MPT_S_ROSTER` whenever someone joins or leaves
and every two seconds. A roster lists id, session and name of every connected
player and replaces the whole name table; the client uses a name only when the
roster session matches the peer's state session, so a replaced peer never shows
a stale name. Shots are relayed only after the sender's first valid state.

The relay rejects invalid lengths, versions, field ranges and duplicate/older
sequences, including wraparound. Peers expire after one second without state;
menus, dead players and snapshots older than 250 ms are inactive. Session/map
changes discard stale attacks and remove replicas after checking native list
ownership. The probe's map key, native map-start marker and observed load
generation identify the world; a reused GAME pointer does not. Local death
publishes inactive state but preserves the current world and its living peers.
Native entity operations and UI drawing stay on the game thread.
Bounded queues drop work rather than blocking that thread. Captured shots wait
in a game-thread staging array and move to the worker queue when its lock is
free; only a full buffer drops a shot, and drops are counted. Remote positions
are interpolated (`pose_buffer.c`): samples are placed on the sender's game-tick
timeline, played back 100 ms behind the newest one at the measured tick rate, and
extrapolated for at most 100 ms when starved. A jump over 160 units resets the
history. Both threads use the performance-counter clock (`game/clock.h`) and the
worker polls every 2 ms at 1 ms timer resolution, so packets are not batched.

Health belongs to each player's local owner. Local damage to its replica is
suppressed, while damage to the real local player is unchanged. This replaces
the original prototype's temporary restore-to-110/death-suppression hack (see the
commit `1a0cf25`). Bars retain the prototype's maximum of 110 and clamp larger values; maximum-health
stats are not transmitted. The ammo value used for the selected weapon is its
live counter, not its potentially stale stored slot. Weapon slots are zero-based.

The original mod does not implement monster/world synchronization or a working
scoreboard. This stage adds neither, nor respawning, Steam invitations or
server-authoritative combat in ordinary multiplayer. The opt-in one-monster
checkpoint is described in [Monster checkpoints](monster-sync-stages.md).
Its production `monster_sync` coordinator observes one ordinary host UNIT or
48 stationary gallery fixtures, creates sanitized client copies, and shares the runtime's world identity and
game-thread lifecycle. A separate explicit monster protocol locks server
authority to one peer/session. Attack/death are events; positions, movement and
health are snapshots. Form revisions preserve logical identity across native
freeze and armor successor allocations; armor loss is separate from final death.
Independent bounded visual slots let armor debris outlive a successor's death.
Large armor uses the medium shell fragments on both peers as a custom effect;
the host owns an extra armor visual bank without altering its native successor.
Client native hits predict HP/freeze/armor/death presentation independently of
authoritative revisions and tombstones. Confirmation adopts visuals once;
conflicts or a750ms grace plus a newer host sequence cause owned cleanup and
recreation. Armor pending immediately displays the unarmored successor.
See [client impact prediction](monster-sync-stages.md#client-impact-prediction)
for the heuristic and projectile provenance limits.
Full authoritative client-origin combat validation remains deferred; existing
player shot replay can create native projectiles and needs end-to-end testing.
The existing transport is unreliable: separate
shot events avoid snapshot loss of short-lived attacks but do not guarantee
delivery under packet loss. Each PC still simulates its own world.

## Room and session

A player either joins an address or presses Create in the multiplayer menu. Create
starts `asmp-server.exe` from the DLL's directory (`server_process.c`): hidden,
suspended until it is placed in a kill-on-close job object, with only its output
pipe inherited. The server first checks that the port is free with an exclusive
bind (epnet itself binds with `SO_REUSEADDR`, which would share the port
silently), then prints `ASMP_SERVER_READY port=N`, `ASMP_SERVER_PORT_IN_USE` or
`ASMP_SERVER_FAILED`. The DLL connects to 127.0.0.1 only after the ready line.
When the host leaves, the server stays up for 500 ms so guests still receive the
closed room, and is then terminated through the DLL's own handle.

The relay keeps one room (`asmp-server/src/lobby.c`). The first registered
player hosts; only the host starts. The host picks the mode and mission in the room
menu; the map travels in `MPT_C_LOBBY_SETTINGS` (host only) and every change resets
the ready flags. Each player picks a character (`MPT_C_LOBBY_PROFILE`, male or
female); a ready guest is locked until it cancels ready, and nobody changes it
during a start.
A start freezes the participants and issues a new run epoch; the room stays
STARTING until every participant reports its level loaded (`MPT_C_LOBBY_LOADED`), then
it is IN_GAME. A 30-second load timeout or a participant leaving cancels the
start back to OPEN. The host leaving closes the room; when the last player
leaves the server opens a fresh room with a new id. The server broadcasts the
room snapshot (`MPT_S_LOBBY`) on every change and every second. A snapshot
carries room id, revision and settings revision, phase, host id and session,
run epoch, mode, compatibility id, a notice (load timeout, player left),
capacity, the map and every member's id, session, character, name and
ready/participant/loaded flags (room protocol version 2).

A room request is not sent just once. The game thread records what the
room should become (`LobbyIntent`: ready value, start, loaded run, the host's map,
the character), and the
worker repeats each request every 500 ms until the snapshot shows it
(`lobby_requests.c`). A request made for an older settings revision or room
revision is dropped, so a stale click can not start a changed room.

`session.c` drives the menu and the level load. The menu scripts and the DLL
talk through the health field of the status text (VID 2, direction byte 0) that
both menus have. The room menu is told apart by its action label (VID 3, the
button label font, direction byte 5), which the script never looks up. Codes are
`enum MenuStatus` in `session.h` and `SPMS_*` in `asmp_play.lgc`. The session
waits in the room until the host's start reaches it, loads the map, reports it
loaded and keeps gameplay off until the room is IN_GAME: only then does the
runtime publish state and shots. A cancelled start returns everyone to the room
through the main menu; the host leaving or a lost link returns to the form with
the reason. Failures are told apart so players can act on them: no answer
(address, port forwarding or firewall), refused, lost, incompatible build,
`asmp-server.exe` missing, port in use and server start failure.

The menus look like the game's own: every visible string is a menu text in the
game's cp1251, generated into `asmp_play.men` and `asmp_lobby.men` by
`analysis/build_asmp_menus.py`. The DLL never holds Russian text. It fills the
room's texts through the native `ACT_SET_TEXT` of each menu text, found by font
VID and direction byte (`menu_layout.h`, which the generator reads), and composes
them from the menu's text bank (off-screen captions) plus ASCII names, numbers and
addresses. `menu_view.c` builds what the menus show from the snapshot (pure,
tested); `menu_texts.c` applies it. The mode and mission catalogue is
`map_catalog.c`: campaign `maps\Level_NN.map`, the two add-ons under `maps\addon`
and `maps\addon2` (offered only when this copy has them; the script checks the
files) and survival `maps\survive_01.map`. A mission's briefing is its
`Text\*.txt`: the game loads the file into a hidden menu text (`ACT_SET_FILE`),
and the DLL shows its title and description paragraphs. Before a start the room
code names the mode and mission, so the script saves the `GameType` and
`LevelNumber` the level scripts read.

The room menu's clicks (start or ready, mode and mission arrows, character) are
request codes in the status entity; a guest's settings arrows are removed by the
script. Player names travel in capitals: the menu fonts draw lower case Latin in
another style. The status font (VID 2) has no Cyrillic, so the status line is a
caption text and the status entity only carries codes. Not done yet: a remote
player's replica still copies the local player's model, so the character choice
shows only in the room.

The network client reaches the relay only through `transport.h` (update, poll,
send, local id, destroy). `transport_udp.c` is the epnet backend; a Steam
networking backend can be added behind the same boundary without touching the
session or the room.
