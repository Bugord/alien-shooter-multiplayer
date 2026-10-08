# Steam diagnostic client

`build/asmp-steam-diag.dll` is a separate x86 DLL that reads player state for the
analyzed Steam Alien Shooter executable (app 33100). It intercepts slot 3 of the
MAP_STEAM vtable in process memory. The original update method is called first,
with its object pointer and return value preserved. After a zero return, the
hook reads player state on the game thread and queues a snapshot. It does not
enable multiplayer or change player state. The EXE on disk is unchanged.

A separate worker writes changed snapshots: game/player pointers, coordinates,
health, weapon slot, ammunition, animation, direction and update-call number
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

`build.ps1 -Server` additionally compiles the existing server into
`steam-diagnostics/build/asmp-server.exe`. Its clients still require a ported
multiplayer DLL; the diagnostic DLL does not connect to it.

The game is copied into `steam-diagnostics/test-game/` with the original EXE
name and bytes preserved. `steam_appid.txt` identifies the game to Steam during
direct launch. The launcher requires the test-copy marker and checks the EXE
hash. It starts the copy normally and loads the diagnostic DLL using
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
The copied game may still use its normal registry settings; the diagnostic DLL
does not alter those settings. Keep the DLL loaded until process exit, even
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

## Git workflow

`master` is the release branch; `develop` collects integrated changes. Work on
the memory-reading stage lives in `feature/steam-diagnostics`; update interception
lives in `feature/steam-tick-hook`, based on that diagnostic work. Follow with
separate branches for the Steam layout and multiplayer port. Merge reviewed stages into
`develop`; use `release/*` when preparing a tested release and `hotfix/*` for
release fixes. Use `feature/*` for subsequent development stages.

Track source, tests, scripts and usage instructions. Reports, analysis results,
IDA databases, build outputs, copied game assets, logs, local history backups
and `analysis/vendor` are ignored. Keep test reports locally.
