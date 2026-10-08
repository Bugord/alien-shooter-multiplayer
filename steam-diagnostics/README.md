# Steam diagnostic client

`build/asmp-steam-diag.dll` is a separate x86 DLL that checks loading and reads player state for the analyzed
Steam Alien Shooter executable (app 33100). It does not enable multiplayer,
install hooks, call game functions, or patch the executable. It logs game/player
pointers, coordinates, health, animation and direction every 250 ms when values
change, with a heartbeat every five seconds.

Only EXE SHA256 `4DD960458D6FFFCC9D00E9E7BA492739FB6D530D4C0B302F1C6BAA8B55D9B142`
is accepted. An in-memory player-accessor signature and expected object vtables
are checked before reading a player. Read faults are contained with Windows SEH.
All addresses are RVAs, so the module load address is taken into account.

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
Expected header: `PROFILE accepted Steam 33100; no hooks installed`.
In the menu, `no-player`/`no-army` is normal. Start a level, move the player and
check that rows with `state=player` show changing x/y and plausible health.

```powershell
Get-Content .\steam-diagnostics\build\logs\asmp-diag-<PID>.log -Wait
```

Exit the test game to stop. To stop sampling while keeping it open, create
`steam-diagnostics/build/asmp-diag.stop`. The next start removes that marker.
The copied game may still use its normal registry settings; the diagnostic DLL
does not alter those settings. Do not manually unload it while it is sampling.

## Validation

The build runs two native checks: reading synthetic game/player memory,
including startup and stale-pointer states; and loading the actual DLL into an
unsupported executable, verifying that its worker rejects it and finishes.
These checks do not substitute for observing movement in the real game.

## Git workflow

`master` is the release branch; `develop` collects integrated changes. Work on
the diagnostic stage lives in `feature/steam-diagnostics`, followed by separate
branches for the Steam layout and multiplayer port. Merge reviewed stages into
`develop`; use `release/*` when preparing a tested release and `hotfix/*` for
release fixes. Use `feature/*` for subsequent development stages.

Track source, tests, scripts and usage instructions. Reports, analysis results,
IDA databases, build outputs, copied game assets, logs, local history backups
and `analysis/vendor` are ignored. Keep test reports locally.
