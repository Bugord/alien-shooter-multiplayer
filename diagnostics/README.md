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

The first implementation checkpoint supports preparing and manually launching
two isolated copies. Both Steam processes have been launched together; the user
confirmed both windows and closed them. A shared-server launch/stop scenario and
bidirectional gameplay checks are subsequent checkpoints.

Each copy has its own CFG, saves, menu backup, DLL directory, logs and stop file.
The default `test-game` and the Mirror commands above retain their paths.
Exit both pair windows before recreating the runtime directories below. Leave
games belonging to another test session open and defer this test until they exit.

```powershell
.\diagnostics\prepare-test-game.ps1 -Instance client-a
.\diagnostics\prepare-test-game.ps1 -Instance client-b
# Run after building and with Steam open. This probe starts read-only clients.
foreach ($instance in @('client-a', 'client-b')) {
    $runtime = Join-Path $PWD "diagnostics\build\two-client\probe\$instance"
    New-Item -ItemType Directory -Path $runtime -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $runtime 'asmp-runtime.marker') -Value 'ASMP isolated runtime'
    Copy-Item -LiteralPath '.\diagnostics\build\asmp-diag.dll' -Destination (Join-Path $runtime 'asmp-diag.dll') -Force
    .\diagnostics\start-test.ps1 -Instance $instance -RuntimeDirectory $runtime
}
```

The launcher preserves the engine's actual render size (720×480 in the verified
run, with an 800×600 requested bound). Use Alt+Tab to switch windows. Close them
normally after the probe. Each `probe/client-a` or `probe/client-b` directory
contains `logs/asmp-diag-<PID>.log`; an `asmp-diag.stop` beside that client's DLL
stops only its diagnostic hooks. The old `build/asmp-diag.stop` does not affect
these isolated DLLs.

Preparation and launch share an exclusive file lock. Preparing an active copy
and relaunching the same copy are refused. A pair launch permits only the other
pair copy to be open; unknown process paths are refused. Use a shell with the
same permissions as the games. Pair DLL directories must end with the selected
instance name, carry the runtime marker and contain the current build's DLL.
Do not rebuild while a game or helper is using the build output.

The script checks run as part of `build.ps1` and can also run independently:

```powershell
.\diagnostics\tests\test_instance_scripts_test.ps1
```

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
