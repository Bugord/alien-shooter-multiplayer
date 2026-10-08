# Steam port: build and review

The x86 `build/asmp-steam-diag.dll` now hosts the Steam adaptation of the existing
multiplayer mod. The diagnostic launcher and log worker remain the entry point
for this review stage. Starting without multiplayer options only reads player
state; `-Multiplayer` or `-ServerAddress` enables native multiplayer behavior.

Only Steam EXE SHA256
`4DD960458D6FFFCC9D00E9E7BA492739FB6D530D4C0B302F1C6BAA8B55D9B142`
is supported. Addresses use RVAs and bindings check method signatures and
vtables. Native faults are contained with Windows SEH and disable further
updates to the affected replica. The original Steam directory is only read.
The launcher runs an ignored copy with separate saves and changes hooks and the
windowed width operand in process memory. The EXE on disk remains unchanged.

## Build

Requires Visual Studio C++ x86 tools and the Windows SDK. Run from the repository
root in PowerShell; the tests need localhost UDP access:

```powershell
.\steam-diagnostics\build.ps1 -Server
.\steam-diagnostics\prepare-test-game.ps1
```

The default source is `D:\Steam\steamapps\common\Alien Shooter`; pass
`-Source <directory>` to prepare another supported installation. Preparing again
copies the original game files over the test directory; it is not needed before
each launch. Keep Steam open and signed in when launching.

## One-PC test

```powershell
.\steam-diagnostics\start-network-test.ps1 -Mirror
# After exiting the game:
.\steam-diagnostics\stop-network-test.ps1
```

This starts the original relay server on UDP 27020, a headless test peer named
Mirror and the windowed game. Connection loads the server's `maps\Level_01.map`
automatically. Mirror publishes your state with X+80 and relays your shot events
back under its own peer ID. After the native torso has appeared, the replica
follows movement/aim, weapons, ammo and health and receives its name and health
bar. Walk, stop, aim independently of movement, switch weapons and fire with an
ammo-consuming weapon. Check that the mirror animates and fires too.

`-Mirror -FireOnce` additionally asks the replica to fire one weapon-1 attack
after six seconds of active state. This exercises the incoming native attack
binding without local input. It does not prove local shot capture or a two-PC
session. The helper expires after ten minutes; this is a test peer, not a bot.
Without `-Mirror`, the helper only observes packets and creates no replica.

The stop script verifies helper executable paths and process start times.
Helper logs are in `build/logs/observer-<timestamp>.log`; game logs are in
`build/logs/asmp-diag-<PID>.log`. Game data and reports stay local and ignored.

## Multiplayer menu and two PCs

Run `build/asmp-server.exe 27020` on the host PC. On each PC build and prepare a
test copy, using its own game installation and Steam account, then either:

```powershell
# Connect directly and load the server map:
.\steam-diagnostics\start-test.ps1 -ServerAddress <host-IPv4> -Port 27020 -Name Alice
# Or open the original mod's multiplayer menu:
.\steam-diagnostics\start-test.ps1 -Multiplayer
```

The menu accepts a nickname of 1..15 bytes and an IPv4:port address. Its existing
connect/status workflow requests the connection on the worker, releases the menu
and loads the server map on the game thread. Returning to the main menu queues
disconnection. Campaign/shop menus send inactive state and remove replicas.
Reconnect through the multiplayer menu after a failed or lost connection.
Allow UDP to the server through the host firewall/network as needed.

The test launcher installs the original mod's menu assets in the copied game
and backs up its native main menu. Launching without multiplayer options restores
that backup. `steam_mainmenu.lgc` and `steam_asmp_play.lgc` adapt the original
mod's scripts to Steam's file-backed saves; Steam lacks their old registry APIs.

For review, test invalid name/address, failed connection, menu connection,
return-to-menu cleanup and reconnection. Visually check names, health bars,
weapon replacement, the complete shop, mouse aim and movement animation. An
independent two-PC game session is still required; the local mirror does not
validate latency or shared gameplay across machines.

## Architecture and behavior

| Layer | Responsibilities |
| --- | --- |
| `asmp-dll/src/game/steam/` | Verified Steam layout/probe, actor factory/destructor, movement/combat API, map/text/menu bindings, MAN action and D3D9/display hooks |
| `asmp-dll/src/multiplayer/client/steam_state_client.*` | Original epnet connection/handshake, packet validation, peer state/names and shot queues; no native entities |
| `asmp-dll/src/multiplayer/steam/` | Session commands/map loading and remote-player lifecycle, game-thread application, bounded worker handoff |
| `common/src/steam_state_protocol.h` | Explicit Steam state/shot codecs and normalized map keys |
| `common/epnet/`, `asmp-server/` | Existing transport and server, with validated Steam relays |
| `steam-diagnostics/src/` | Test DLL entry point, post-update hook/queue, logging worker, launcher and optional test peer/dummy |

The port follows `multiplayer.c`'s wait-for-player/torso, private VID, native
factory, prepare-weapons/name and spawned-state flow. It applies velocity,
movement intent, position and independent leg/torso directions. Native MAN logic
selects idle/run and advances animation. Frames and timers are never replicated;
the animation ID in snapshots is only diagnostic. Weapon application precedes
torso lookup because the engine can replace the attachment.

Shots follow the original action-hook approach: aim is captured from action
0x25 and ammo-consuming attacks from action 0x5D, then queued separately from
snapshots. Incoming events invoke native action 0x25 on the replica. The game
thread never performs socket or file I/O. Names use native owned strings and are
positioned again after weapon changes. Health bars use native rectangles through
D3D9 EndScene; installation waits for the renderer to create its device.

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
ownership. Native entity operations and UI drawing stay on the game thread.
Bounded queues drop work rather than blocking that thread.

Health belongs to each player's local owner. Local damage to its replica is
suppressed, while damage to the real local player is unchanged. This replaces
the original prototype's temporary restore-to-110/death-suppression hack. Bars
retain the prototype's maximum of 110 and clamp larger values; maximum-health
stats are not transmitted. The ammo value used for the selected weapon is its
live counter, not its potentially stale stored slot. Weapon slots are zero-based.

The original mod does not implement monster/world synchronization or a working
scoreboard. This stage adds neither, nor respawning, Steam invitations or
server-authoritative combat. The existing transport is unreliable: separate
shot events avoid snapshot loss of short-lived attacks but do not guarantee
delivery under packet loss. Each PC still simulates its own world.

## Window and diagnostic modes

Windowed startup defaults to an 800x600 bound. The engine selects a supported
adapter mode; the launcher matches the client area to the actual render size,
avoiding cropping. `-Width 1024 -Height 768` requests a larger bound;
`-Fullscreen` uses the engine's normal fullscreen selection.

`start-test.ps1` alone logs player state after the original update. Enter a
campaign or survival map to sample movement, signed health, weapon/live ammo and
stored ammo. `-DummyActor` instead creates a local X+80 actor after two seconds
and removes it after sixty seconds; it is mutually exclusive with multiplayer.
This earlier lifecycle test does not replicate network combat.

Create `build/asmp-diag.stop` to stop the mod while keeping the game open. Cleanup
waits for the next game tick, removes owned replicas and restores action,
display/window and update hooks. A paused game must resume. Expected footer:
`hook_restore_result=0`, `# HOOK_RESTORE action=1 display=1`, and installed=0.
Foreign hooks are left untouched and reported as restore errors. The DLL stays
loaded until process exit because a caller may have fetched a callback before
restoration. A new capture session requires restarting the game.

## Validation and Gitflow

The build runs synthetic layout/probe and actor lifecycle checks; update-hook
ABI, queue overflow, concurrency and restoration checks; actual-DLL rejection
in an unsupported process; and window-mode instruction/protection checks.
Coordinator checks exercise torso readiness, combat state, event ordering,
map/session generations, stale captures, expiry and rejected-actor cleanup.
Real UDP tests exercise two clients through the server, malformed packets,
signed fields, state/shot sequence wrap, no self-echo, stale/dead/menu shot
rejection and reconnect with a reused client ID. These checks do not prove the
native game's rendering or every menu/input path; use the manual review above.

Work stays on `feature/steam-remote-player` for review. Reviewed features can be
merged into `develop`; `master` remains the release branch, with `release/*` and
`hotfix/*` following Gitflow. Track code, tests, scripts and usage instructions.
Builds, test copies, logs, reports, analysis tools, IDA databases and local history
backups are ignored. No push or merge is part of this review preparation.
