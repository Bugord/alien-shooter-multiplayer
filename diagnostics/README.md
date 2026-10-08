# Build, test and review

The x86 `build/asmp-diag.dll` hosts the Steam multiplayer runtime. The
diagnostic launcher configures it and the harness logs its frame observations.
Starting without multiplayer options only reads player
state; `-Multiplayer` or `-ServerAddress` enables native multiplayer behavior.

Only Steam EXE SHA256
`4DD960458D6FFFCC9D00E9E7BA492739FB6D530D4C0B302F1C6BAA8B55D9B142`
is supported. Addresses use RVAs and bindings check method signatures and
vtables. Native faults are contained with Windows SEH and disable further
updates to the affected replica after one guarded removal attempt. A failed
removal is reported and abandoned until the world changes. The original Steam directory is only read.
The launcher runs an ignored copy with separate saves and changes hooks and the
windowed width operand in process memory. The EXE on disk remains unchanged.

Layers, replica lifecycle, protocol and threading are described in
[Architecture and behavior](../docs/architecture.md). All documentation is
listed in the [documentation index](../docs/README.md).

## Build

Requires Visual Studio C++ x86 tools and the Windows SDK. Run from the repository
root in PowerShell; the tests need localhost UDP access:

```powershell
.\diagnostics\build.ps1 -Server
.\diagnostics\prepare-test-game.ps1
```

The default source is `D:\Steam\steamapps\common\Alien Shooter`; pass
`-Source <directory>` to prepare another supported installation. Preparing again
copies the original game files over the test directory; it is not needed before
each launch. Keep Steam open and signed in when launching.

## One-PC test

```powershell
.\diagnostics\start-network-test.ps1 -Mirror
# After exiting the game:
.\diagnostics\stop-network-test.ps1
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

## Two game copies on one PC: launch verification

The first two implementation checkpoints support preparing and manually launching
two isolated copies, with a new pair of DLL directories for each test. Both Steam
processes have been launched together; the user confirmed both windows and closed
them. A shared-server launch/stop scenario is the managed session below; bidirectional
gameplay checks are the live checklist at its end.

Each copy has its own CFG, saves, menu backup, DLL directory, logs and stop file.
The default `test-game` and the Mirror commands above retain their paths.
Exit both pair windows before preparing new runtime directories below. Leave
games belonging to another test session open and defer this test until they exit.

```powershell
.\diagnostics\prepare-test-game.ps1 -Instance client-a
.\diagnostics\prepare-test-game.ps1 -Instance client-b
# Run after building and with Steam open. This probe starts read-only clients.
$pair = .\diagnostics\prepare-test-runtimes.ps1
$pair | Format-List
.\diagnostics\start-test.ps1 -Instance client-a -RuntimeDirectory $pair.ClientA -Name LocalA
.\diagnostics\start-test.ps1 -Instance client-b -RuntimeDirectory $pair.ClientB -Name LocalB
```

The launcher preserves the engine's actual render size (720×480 in the verified
run, with an 800×600 requested bound). Use Alt+Tab to switch windows. Close them
normally after the probe. Each `build/two-client/<unique-id>/client-a` or
`client-b` directory contains `logs/asmp-diag-<PID>.log`; an `asmp-diag.stop` beside that client's DLL
stops only its diagnostic hooks. The old `build/asmp-diag.stop` does not affect
these isolated DLLs.

`prepare-test-runtimes.ps1` validates both game copies, checks space for both DLLs
and copies/verifies them before returning `ClientA`, `ClientB`, `Directory` and
`DllSha256`. It creates fresh directories and preserves previous DLLs and logs.
It prepares files only; the two launch commands above run sequentially.
Game preparation checks space for each full physical copy.

Preparation and launch share an exclusive file lock. Preparing an active copy
and relaunching the same copy are refused. A pair launch permits only the other
pair copy to be open; unknown process paths are refused. Use a shell with the
same permissions as the games. Pair DLL directories must end with the selected
instance name, carry the runtime marker and contain the current build's DLL.
Game EXE/marker and runtime DLL/marker validation happen before settings/menu or
stop-file writes. Junctions and symlinks in the copies are refused. Each launch
restores its caller's environment variables, including on launcher failure.
Do not rebuild while a game or helper is using the build output.

The script checks run as part of `build.ps1` and can also run independently:

```powershell
.\diagnostics\tests\test_instance_scripts_test.ps1
.\diagnostics\tests\test_resource_isolation_test.ps1
```

The resource tests use temporary game files and a substituted launcher. They
check separate settings, saves, menus and stop files, restored environments,
hash/marker rejection, active-copy protection and disk-capacity failures. They
also create a temporary junction, which requires a shell allowed to create it.
These checks do not prove native save writes or live independent hook shutdown.

## Quick launch from Git Bash

Requires Git Bash on Windows and the compiled Windows binaries. Prepare
`client-a` and `client-b` once with the commands above, keep Steam open, and close
the previous game windows before launching again. The Bash scripts resolve all
project paths relative to themselves and call the existing PowerShell checks.

In one Git Bash terminal, from the repository root:

```bash
bash diagnostics/start-server.sh
```

On Windows, double-click `diagnostics\start-server.bat` for the same thing in its own window;
closing that window stops the server. It takes an optional UDP port argument.

The relay runs in that terminal on UDP 27020; Ctrl+C stops it. An occupied port
is reported without replacing or stopping its owner. If the matching relay is
already running, use it and proceed directly to the clients command.

In another Git Bash terminal, from the repository root:

```bash
bash diagnostics/start-clients.sh
```

This prepares two fresh DLL directories, then launches `LocalA` and `LocalB`
sequentially with automatic connection/map loading at `127.0.0.1:27020`.
Logs are printed before launch. For localhost, the port owner must be this
checkout's relay; a running diagnostic peer is refused. Successful launchers
still require checking the windows/logs for completed connection and map loading.
If B fails, A remains open and both diagnostic directories are preserved.

Optional positional arguments:

```bash
bash diagnostics/start-server.sh 27021
bash diagnostics/start-clients.sh 127.0.0.1 27021 Alice Bob
# A server on another PC:
bash diagnostics/start-clients.sh 192.168.1.10 27020 Alice Bob
```

Use `--help` on either script. From another directory, pass the full script path,
for example `bash /d/Projects/Mods/alien-shooter-multiplayer/diagnostics/start-clients.sh`.
Close the games normally when finished. These are manual launch conveniences;
the managed session below adds the manifest, readiness checks, per-client restart
and paired shutdown.

`tests/bash_wrappers_test.ps1` checks Bash syntax and real Bash-to-PowerShell
argument forwarding with temporary stubs, including paths with spaces, literal
names, invalid ports and propagated exit codes. It runs in `build.ps1` when Git
Bash is installed. No games or relay are started by that test.

## Managed two-client session

One command starts the relay and both real clients; another stops them. Prepare
`client-a` and `client-b` once, build with `-Server`, keep Steam open and close any
other game of this project first.

```powershell
.\diagnostics\start-two-client-test.ps1 [-NameA LocalA] [-NameB LocalB] [-Port 27020]
.\diagnostics\start-two-client-test.ps1 -RestartClient A   # relaunch one exited client
.\diagnostics\stop-two-client-test.ps1 [-CloseGames [-Force]] [-TimeoutSeconds 15]
```

Start refuses an older `network-session.json`, a running Mirror peer, a busy UDP
port, any running `AlienShooter` or a previous session whose processes are still
alive or cannot be identified. It prepares fresh DLL directories, starts the relay
and waits until it owns the UDP port, then launches A and B one at a time. A client
counts as started only when its log shows `NET_READY` and an accepted map load
(`# SESSION map_load_result=1`) within `-TimeoutSeconds` (default 90); loader
success alone is not enough. The launcher records its result as JSON
(`ASMP_LAUNCH_RESULT`, written beside each client's DLL as `launch-result.json`)
with the PID, EXE path and creation time, so no output is parsed. Windows are
never found by name; identity is always PID + image path + creation time.

State lives in `build/two-client/session.json` (written through a temporary file
and replaced atomically): session id, relay and client identities, runtime
directories, DLL hash, ports, names and a per-process state
(`starting/running/failed/mod-stopped/stopped`). Relay logs are `server.out.log`
and `server.err.log` in the session directory; game logs are in each
client's `logs/`. If anything fails, the mod is rolled back in the clients that
did start, the relay is stopped, the games and manifest stay for diagnosis, and
the command reports the failing stage.

Stop writes both clients' `asmp-diag.stop` files, waits for `cleanup=1` in each
log and only then stops the relay. The game windows stay open unless `-CloseGames`
is given (a normal close request first; `-Force` terminates only a process whose
identity still matches). A game in the background may not tick: if cleanup times
out, bring that window to the front and run stop again; the relay is kept until
both clients are clean. Stop is repeatable, skips processes that already exited,
and never touches a process whose PID was reused or whose path/time cannot be
read; such items are listed and remain in the manifest.

`-RestartClient A|B` relaunches an exited client against the live relay in the same
session and leaves the other client alone. A client stopped with `-CloseGames`
requires a full new start (the relay is stopped by then).

`tests/two_client_session_test.ps1` (part of `build.ps1`) covers the manifest,
identity and PID reuse, readiness parsing, launcher failure/timeout paths,
partial and repeated stop, `-CloseGames -Force`, concurrent commands and the JSON
launcher result, using substituted launch operations and harmless fake processes.
Real native starts are covered only by the live checklist.

### Live checklist (two windows, one keyboard and mouse)

1. Start the pair; both clients show a different peer ID in their logs and each
   sees exactly one remote replica.
2. Move, stop, aim and change weapons in both directions; keep one window in the
   background for at least 60 seconds and confirm ticks and packets continue.
3. Only the active window reacts to input: switch with a held key, check cursor
   capture and Alt+Tab; no input reaches both windows.
4. Ammo-consuming shots replay on the other side; compare local ammo use, received
   events and `SHOTS applied` with what is visible.
5. Damage/healing of an owner shows on the replica; owner death removes it while
   the survivor stays in its own world.
6. A leaves to the menu: B loses A; A reconnects: exactly one replica again.
   Repeat for B and for closing and restarting one process (`-RestartClient`).
7. Stop the relay: both return to Multiplayer with a lost-connection message;
   after a new start both can connect again.
8. A stop file for A alone unloads only A's hooks. Full and repeated stop report
   cleanup truthfully.
9. No stray helpers or settings crossover afterwards; the original Steam install
   is unchanged.

None of this has been run live yet; mark each item after a real session.

## Multiplayer menu and two PCs

Run `build/asmp-server.exe 27020` on the host PC. On each PC build and prepare a
test copy, using its own game installation and Steam account, then either:

```powershell
# Connect directly and load the server map:
.\diagnostics\start-test.ps1 -ServerAddress <host-IPv4> -Port 27020 -Name Alice
# Or open the original mod's multiplayer menu:
.\diagnostics\start-test.ps1 -Multiplayer
```

The menu accepts a nickname of 1..15 bytes and an IPv4:port address. Its existing
connect/status workflow requests the connection on the worker, releases the menu
and loads the server map on the game thread. Returning to the main menu queues
disconnection. Campaign/shop menus send inactive state and remove replicas.
Losing the connection in a level loads the main menu, opens Multiplayer and
shows `Connection lost!`; the connect button allows retry. Connect and map-load
confirmation each have a ten-second timeout.
Allow UDP to the server through the host firewall/network as needed.

The test launcher installs the original mod's menu assets in the copied game
and backs up its native main menu. Launching without multiplayer options restores
that backup. `mainmenu.lgc` and `asmp_play.lgc` adapt the original
mod's scripts to Steam's file-backed saves; Steam lacks their old registry APIs.

For review, test invalid name/address, failed connection, menu connection,
return-to-menu cleanup and reconnection. Visually check names, health bars,
weapon replacement, the complete shop, mouse aim and movement animation. An
independent two-PC game session is still required; the local mirror does not
validate latency or shared gameplay across machines.

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
waits up to five seconds for game-thread replica removal, then restores display,
action, map-load and tick hooks. Startup failures use this same cleanup path in
reverse order. Expected footer: `cleanup=1` and
`# HOOK_RESTORE action=1 display=1 world=1 tick=0`. A paused game must resume;
timeout keeps the runtime stopping and retains hooks, so cleanup can be retried.
Foreign hooks are preserved and reported. The DLL is pinned before publishing
hooks or private VIDs and stays loaded until process exit, including when a
caller has already fetched a callback. A new diagnostic capture session requires
restarting the game.

## Validation and Gitflow

The build runs synthetic layout/probe and actor lifecycle checks; update-hook
ABI, queue overflow, concurrency and restoration checks; actual-DLL rejection
in an unsupported process; and window-mode instruction/protection checks.
Coordinator checks exercise torso readiness, combat state, event ordering,
map/session generations, stale captures, expiry and rejected-actor cleanup.
Real UDP tests exercise two clients through the server, malformed packets,
signed fields, state/shot sequence wrap, no self-echo, stale/dead/menu shot
rejection and reconnect with a reused client ID. Runtime/session checks cover
rollback at every startup stage, active-replica rollback, paused cleanup,
foreign-hook preservation, reinstall, display throttling, menu validation,
connection loss/retry and stale generations. Native hook checks exercise the
load-map and EndScene calling conventions and restored page permissions.
These checks do not prove the
native game's rendering or every menu/input path; use the manual review above.

Refactor work stays on `feature/*` branches for review. Reviewed features can be
merged into `develop`; `master` remains the release branch, with `release/*` and
`hotfix/*` following Gitflow. Track code, tests, scripts and usage instructions.
Builds, test copies, logs, reports, analysis tools, IDA databases and local history
backups are ignored. No push or merge is part of this review preparation.
