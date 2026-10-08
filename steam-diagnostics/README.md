# Steam diagnostic client

`build/asmp-steam-diag.dll` is a separate x86 DLL that reads player state for the
analyzed Steam Alien Shooter executable (app 33100). It intercepts slot 3 of the
MAP_STEAM vtable in process memory. The original update method is called first,
with its object pointer and return value preserved. After a zero return, the
hook reads player state on the game thread and queues a snapshot. It does not
enable multiplayer or change player state by default. The optional dummy test
below creates one engine-owned actor for lifecycle validation. The EXE on disk
is unchanged.

A separate worker writes changed snapshots: game/player pointers, coordinates,
health, weapon slot, ammunition, animation, leg/torso directions, movement intent,
velocity and update-call number
(`tick`). It checks the queue every 20 ms and writes statistics every five seconds.
Unchanged snapshots are omitted except for a heartbeat every five seconds.

Only EXE SHA256 `4DD960458D6FFFCC9D00E9E7BA492739FB6D530D4C0B302F1C6BAA8B55D9B142`
is accepted. The player accessor, tick prefix and original vtable slot are
checked before installing the hook. Expected object vtables are checked before
reading a player. Read faults are contained with Windows SEH.
All addresses are RVAs, so the module load address is taken into account.

Installation exchanges one aligned pointer atomically and restores its page
protection. Gameplay code bytes are not patched. A bounded 1024-snapshot queue
keeps file I/O off the game thread; the hook drops a snapshot instead of waiting
when the queue is full or locked. `# TICK_STATS` reports `calls`, `captured`,
`dropped` and `installed`. Drops are counted rather than silently hidden.

## Build and run

Requires Visual Studio with the x86 C++ tools and Windows SDK. From the repository
root in PowerShell:

```powershell
.\steam-diagnostics\build.ps1
.\steam-diagnostics\prepare-test-game.ps1
# Open Steam and sign in, then:
.\steam-diagnostics\start-test.ps1
```

`build.ps1 -Server` additionally compiles the server into
`steam-diagnostics/build/asmp-server.exe`, including the Steam state relay.
Network tests use localhost UDP; run the build outside a sandbox that blocks
loopback networking.

Windowed tests request a render width cap of 800 by default. The engine chooses
a supported adapter mode within that cap, favoring the desktop aspect ratio;
the chosen mode can be smaller than the requested dimensions.
`start-test.ps1 -Width 1024 -Height 768` requests a larger mode;
`-Fullscreen` uses the engine's normal fullscreen behavior. The script changes the
copied CFG defaults and `test-game/saves/options.ini`, whose saved values take precedence.
The Steam renderer normally overrides width/height with a mode chosen using a
hardcoded 1280 width cap. For a windowed launch, the launcher uses the initial
process-creation debug event to change that cap in process memory before the
renderer runs. It validates the surrounding instructions and modifies only the
four-byte immediate operand, restores page protection and flushes the instruction
cache. It then detaches before normal game initialization and DLL loading.
This does not change the EXE file or scale an already rendered frame.

After initialization, the launcher reads the actual render dimensions and matches
the visible client area to them. Resizing the window below the render dimensions
would crop the frame and is deliberately avoided. The requested height is a
preferred bound, not a forced adapter mode. The launcher prints both engine and
window dimensions; check the complete menu/shop and mouse aiming during gameplay.
Game saves also belong to the copied directory.

To start a local server, a headless observer client and the windowed game client:

```powershell
.\steam-diagnostics\build.ps1 -Server
.\steam-diagnostics\start-network-test.ps1
# After exiting the game:
.\steam-diagnostics\stop-network-test.ps1
```

The observer runs for ten minutes and writes `build/logs/observer-<timestamp>.log`.
The stop script checks executable path and process start time before stopping the
session's helper processes. The observer has no game window. To test two actual
games, run a server and use `start-test.ps1 -ServerAddress <IPv4> -Port 27020 -Name <name>`
on each PC with its own game copy and Steam account. No remote player is created
in the game at this stage: received state is logged for verification.

### Second actor lifecycle test

```powershell
.\steam-diagnostics\start-test.ps1 -DummyActor
```

Enter a campaign or survival level. After two seconds of valid gameplay, the
game thread creates one MAN through the Steam engine's factory. It follows the
local character with an X offset of 80, supplying velocity, movement intent and
independent leg/torso directions. Native MAN logic selects idle/run and advances
the actor's animation; replication never sets animation IDs, frame cursors or
animation timers. This follows the legacy `RPS_SPAWNED` implementation in
`asmp-dll/src/multiplayer/multiplayer.c`, using verified Steam methods and layout.
It is removed through the engine's destructor after 60 seconds.
Menus and the shop do not spawn an actor. Returning to the shop, changing maps,
or requesting diagnostic shutdown also ends the actor's lifetime.
The actor is a local test double; received network state does not control it.
Weapon changes, shots and shared health are not synchronized by this test.

The private 0x490-byte VID is copied from the actual local MAN, whose Steam class
number is 7. MAP's class table maps it to jump-table index 4 and arm 0x43934C,
which calls the MAN constructor at 0x434350. Its counters are reset and its creation/deletion scripts disabled
to avoid executing the local player's map scripts. Its storage remains in the
DLL while the engine owns the entity. Factory (0x440680), MAN destructor
(0x434440), movement (0x46BC20) and rotation (0x46BD50)
signatures and factory/destructor vtable entries are validated before enabling
the test. The normal EXE hash check still applies.

Before dereferencing a saved actor pointer, the code finds it in the current
map's entity list and checks its private VID. It discards an actor removed by
the engine or a pointer reused for another object. No entity is destroyed from
the logging/network worker. A shutdown marker waits for the next game tick to
remove the actor before restoring the hook; a paused game must resume for that
cleanup. An engine exception disables further dummy updates and is recorded as
`fault`; any surviving actor remains owned by the map until it unloads.

`# DUMMY event=spawned|removed|lost|rejected|fault` records lifecycle transitions,
including the failure reason and observed source class for a rejected spawn.
`# DUMMY_POSE` records applied speed/intent, native animation/frame (read only),
and torso aim once per second. ENTITY velocity is at +0x20; moving intent is
flag 0x80 at +0x28. Only this flag is changed, preserving other engine state.
MAN::action(0x82), at 0x4345F9, selects run (2) or idle (0) using velocity;
ENTITY's update at 0x46FDA0 advances and wraps frames using engine timing.
Check visually that a complete second character appears, animates its legs
while running, stops when standing, follows torso aim, disappears after a minute,
and does not replace control of the local
player. Automated lifecycle checks use synthetic engine callbacks; a live game
test is required to validate the mapped Steam methods and rendering.

The DLL sends the latest post-update snapshot from its worker at most once every
33 ms. `# NET_READY` confirms the application handshake, `# NET_REMOTE` records a
peer's state, and `# NET_STATS` counts sent, received and rejected packets.
The 100-byte version-2 packet has explicit big-endian words, IEEE 754 float
coordinates and velocity, signed 32-bit health/live ammo/weapon slot, animation,
leg/torso directions, movement intent, torso presence, tick and nine stored ammo
counters. Animation IDs are diagnostic state, not applied by the dummy test;
frame numbers and animation timers are never sent. Version 1 is rejected, so
rebuild both clients and the server together. Process pointers are never sent.
The server adds the sender ID and session generation, validates length/version,
finite coordinates/speed and pose fields,
and rejects duplicate or older state sequences, including across sequence wrap.
New sessions allow sequence restart when a client reconnects.

Menus and snapshots older than 250 ms are sent as inactive. Peers expire locally
after one second without state. The latest snapshot replaces earlier snapshots;
this transport does not preserve individual shots or short-lived events.
The server relays client-provided state; gameplay authority and remote entities
are separate subsequent stages. Automatic reconnection after a lost server is
not implemented; restart the test client to reconnect.

The game is copied into `steam-diagnostics/test-game/` with the original EXE
name and bytes preserved. `steam_appid.txt` identifies the game to Steam during
direct launch. The launcher requires the test-copy marker and checks the EXE
hash. It starts the copy and loads the diagnostic DLL using
LoadLibraryW in that process. The Steam installation is only read.

Logs: `steam-diagnostics/build/logs/asmp-diag-<PID>.log`.
Expected messages: `PROFILE accepted Steam 33100; gameplay reads after original tick`
and `# TICK_HOOK installed slot=3`. `calls` and `captured` should increase.
The CSV header starts with `milliseconds`; skip startup lines and lines starting
with `#` when importing the CSV records.
In the menu, `no-player`/`no-army` is normal. Start a level, move the player and
check that rows with `state=player` show changing x/y and plausible health.

For the combat check, fire several shots with a weapon that consumes ammo,
switch between available weapons, collect ammunition, take damage and collect
health. Compare the log with the HUD. `weapon_slot` is zero-based (0..9),
`weapon_vid` is the linked weapon definition index (10..19); -1 means unknown.
`current_ammo` is the selected weapon's live counter, computed from signed
`current_ammo_raw / 64` with truncation toward zero. `stored_ammo_slot_1` through
`stored_ammo_slot_9` are the other stored counters. The selected slot's stored
value can be stale until a weapon switch; use `current_ammo` for that weapon.
The pistol may not consume ammo. Sampling occurs after update calls; events
within one call or dropped snapshots can still be missed. Weapon identifiers
are engine slots, not keyboard shortcut numbers.

```powershell
Get-Content .\steam-diagnostics\build\logs\asmp-diag-<PID>.log -Wait
```

Exit the test game to stop. To stop sampling and restore the original vtable
entry while keeping it open, create `steam-diagnostics/build/asmp-diag.stop`.
The footer must show `hook_restore_result=0` and `installed=0`. A changed slot
owned by another hook is left untouched and reported as an error. The next
start removes the stop marker; restarting capture requires a new game process.
Keep the DLL loaded until process exit, even
after stopping: a thread may already have fetched its callback pointer.

## Validation

The build runs native checks: reading synthetic game/player memory,
including weapon switches, separate live/stored ammo, signed health and ammo,
startup and stale-pointer states; and loading the actual DLL into an
unsupported executable, verifying that its worker rejects it and finishes.
Tick-hook checks cover post-update snapshots, ECX and return preservation,
bounded queue overflow and ordering, concurrent capture/draining with 20,000
update calls, vtable protection restoration, stop,
calls through a previously fetched callback after stop, and rejection of
invalid or changed slots. These checks do not substitute for a gameplay run.
State-sync checks exercise two clients through the actual UDP server, full-width
signed values, all stored ammo, malformed handshakes/packets, registration before
relay, sequence ordering/wrap, absence of self-echo, inactive state on stale
sampling, peer expiry and reconnection with a reused client ID.
Window-mode checks cover startup instruction guards, changing only the width
operand, restoring executable page protection, rejecting changed instructions,
and reading the actual render dimensions. Fullscreen does not apply this patch.

## Git workflow

`master` is the release branch; `develop` collects integrated changes. Work on
the memory-reading stage lives in `feature/steam-diagnostics`; update interception
lives in `feature/steam-tick-hook`, based on that diagnostic work. Follow with
`feature/steam-state-sync` for state exchange, based on the tick hook, and
`feature/steam-windowed-render` for matching smaller render modes to the window.
separate branches for the Steam layout and multiplayer port. Merge reviewed stages into
`develop`; use `release/*` when preparing a tested release and `hotfix/*` for
release fixes. Use `feature/*` for subsequent development stages.

Track source, tests, scripts and usage instructions. Reports, analysis results,
IDA databases, build outputs, copied game assets, logs, local history backups
and `analysis/vendor` are ignored. Keep test reports locally.
