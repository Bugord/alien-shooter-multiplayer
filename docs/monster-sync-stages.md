# Monster synchronization checkpoints

The chosen design is an authoritative **game host**: its native game simulates
monsters, and the relay distributes that host's world events and poses. The
standalone relay currently has no game simulation. Monster networking is not
implemented by the first checkpoint below.

Each checkpoint needs its own live result before implementing the next one.

| Stage | Scope and acceptance |
| --- | --- |
| 1: ground UNIT control | One borrowed class-2 UNIT, optionally its single linked class-9 weapon part, in an offline copied game. Suppress decisions and autonomous locomotion while native animation advances. Check idle/run/stop/turn, pose drift, target/command/stack isolation and cleanup. |
| 2: flying and other composite units | Live VID inventory, real flying UNIT drone and actual child classes/chains. PLANE was found in the EXE but no loaded class-4 VID was found. Verify each supported composition separately, including disposal/reload. |
| 3: combat effects | Trace and isolate attack, damage, death, loot, score and mission scripts. Replay each event once without duplicate effects. |
| 4: host world and identity | Explicit host authority/epoch, monster IDs/generations and native spawn/despawn ownership. Verify targeting both players, disconnect and death. |
| 5: one networked monster | Host plus one client, one validated type, reliable lifecycle events and pose/intent snapshots. Verify late join, stale data rejection, reload and teardown. |
| 6: population and coverage | All verified types, measured batching/rates, map scripts, stress and recovery. |

## Stage 1 usage

Build with `./diagnostics/build.ps1 -Server`, then run:

```powershell
.\diagnostics\start-test.ps1 -MonsterPuppet
```

Use the default test copy with Steam open and no other game/managed network
session. This mode is exclusive with `-DummyActor`, multiplayer and paired
instances. It automatically loads `maps\survive_01.map` after two seconds,
then searches for one living ground UNIT in idle/run. It accepts either no
child or exactly one linked class-9 weapon part, with no grandchildren. Other
classes/chains are excluded. The experiment
times out if none appears within 45 seconds. Keep the player alive.

The selected monster moves near the player's initial position (X+80), using
six five-second phases: idle, east run, stop, west run, stationary turn, idle.
Other monsters continue normal gameplay. This is a scripted source of poses
for the experiment; it sends no network packets and performs no collision
prediction. Walk near an obstacle to observe that the controlled monster does
not independently steer. Authoritative collision handling belongs to the host
in a later checkpoint.

Logs are `diagnostics/build/logs/asmp-diag-<PID>.log`. `# MONSTER` records show
the native VID, animation/frame, intercepted decision/movement counts, blocked
actions and violations. Samples are taken **before** applying the next pose.
State 4 (`MONSTER_PROBE_DONE`), phase 6, `cleanup=1`, `violations=0`, and positive
`decisions`, `movements`, `frame_changes` are the automated success criteria.
State 5 is failure; investigate it before advancing. A stop before phase 6 is
cleanup only and does not establish successful validation.

The game owns the borrowed entity and its unchanged VID. The test consumes its
existing goal/command/stack with native actions; it does not save or recreate
those commands. At completion it restores the original behavior, stops it and
selects idle so normal AI can acquire a new target. It neither creates nor
deletes monsters. During control, damage/HP and script mutations to this UNIT
are blocked; this is an experimental movement policy, not combat authority.
The native common update, idle/run scripts and frame-linked effects still run.
This stage does not establish safe client replay of attacks or deaths.

Initial live acceptance (2026-10-09): VID50 plus linked VID108/class9 completed
all six phases with zero violations, 169 native frame changes, 5297 intercepted
decisions and 4944 intercepted autonomous movement calls. The user confirmed
that movement and animation looked normal. Release and hook stop succeeded.
Separate interruption/reload/obstacle runs below remain manual follow-up checks;
this result covers one type and a normal completion only.

Create `diagnostics/build/asmp-diag.stop` to finish hook cleanup while keeping
the game open. The footer must report `hook_restore_result=0 cleanup=1`.
The DLL remains pinned until game exit. If game-thread cleanup cannot finish,
hooks are retained and the footer reports failure.

## Live checklist for stage 1

- Observe all six phases; idle and run animate, and stop has no sliding.
- Approach the controlled monster: it neither selects nor attacks the player.
- Other monsters still chase/attack; the local MAN and remote-player code are
  unaffected by this offline-only option.
- Check the final phase-6 counters and the successful stop footer.
- On separate runs, interrupt during control and reload/leave the map; there
  must be no writes through the old pointer. Reload ends this experiment.

Synthetic `unit-puppet-test.exe` checks entity filtering, native-action setup,
pose inputs, frame preservation, destructor invalidation, same-pointer reuse,
foreign hooks, rollback and reinstall. `monster-probe-test.exe` checks phases,
pre-apply drift detection, epoch changes, timeout, interruption and cleanup
failures. They do not prove native animation or rendering.

## Stage 2: one flying UNIT, without survival waves

```powershell
.\diagnostics\build.ps1 -Server
.\diagnostics\start-test.ps1 -MonsterFlying
```

This separately enabled checkpoint loads native `maps\survive_01.map` with
the dedicated `diagnostics/monster-flying.lgc` script in the default test copy.
The original script is saved once under `asmp-monster-backup/survive_01.lgc`.
The next launch in any other mode restores it; a failed flying launch restores
it as well. The map geometry and original Steam installation are unchanged.
The script creates one VID290 drone and never calls the survival wave tick.
Native map objects, player parts and visual effects still exist.

Live inventory (2026-10-09) found all 27 stock monster VIDs and the drone VID290
use class 2 (UNIT), including the visually flying drone. No class-4 PLANE VID
was found in loaded descriptor slots 0..1099. The user chose the real flying
drone experiment instead of constructing an artificial PLANE. The test script
checks VID290's native class before creating it. The borrowed object may have
no child or one linked class-9 part;
other child classes and longer chains remain excluded pending their own
bindings and live evidence. This does not validate all monster compositions.

The same six 5-second phases drive position, direction, velocity and moving
intent. Altitude comes from the captured drone rather than the ground player.
The existing UNIT action/movement/destructor hooks apply; the common animation
update is untouched. Frame numbers and timers are never
supplied by the controller. Damage and scripted command mutations remain
blocked during this movement-only experiment. Combat replay is stage 3.

Automated acceptance: `vid=290 class=2`, terminal `state=4 phase=6 cleanup=1`, zero
violations and positive decisions/movements. VID290 and its linked VID291 have
single-frame idle/run art: zero frame changes is expected. `MONSTER_FRAMES`
logs show their native ranges. Multi-frame art must still progress, whether
the root or child owns it; direction-based range changes do not count as frame
progression. Idle/run state is checked before the next pose is applied.
A separate successful
stop footer must show `hook_restore_result=0 cleanup=1`. An interrupted run
proves cleanup only, not completion of the phase sequence.

Live checklist:

- Observe one flying unit, native idle/run animation, stops and stationary turn.
- Approach it: no independent steering, following or attacks during control.
- Confirm there are no survival waves throughout and after the 30-second test.
- On a separate run, interrupt during control; release and hook restoration
  must succeed, and the game must keep running.
- On a separate run, reload/leave the map during control; the old entity must
  be invalidated and cleanup must finish without writing through a stale pointer.

For the repeatable same-map reload case, use
`start-test.ps1 -MonsterFlying -MonsterReload`. It reloads once after ten
seconds and finishes without applying another old pose. Expected result:
`MONSTER_RELOAD completed cleanup=1 violations=0`, terminal phase 2, then a
successful stop footer. This checks disposal across reload, not the full
six-phase sequence. The reloaded map still has one drone and no waves.

`unit-puppet-test.exe` and `monster-probe-test.exe flying` cover child ownership,
lifecycle, foreign-hook preservation/rollback, altitude, phases, drift and world
epoch. The full build runs the ground and flying controller cases. Live
validation is required.

Live result (2026-10-09): VID290 + linked VID291/class9 completed all six
phases in PID117864 with zero violations, altitude20, decisions846,
movements4947 and blocked213. The native world scan after completion contained
exactly one UNIT VID290 and no other UNIT/PLANE. Separate same-map reload
(PID93896) and active-control interruption (PID57992) passed with cleanup1
and successful hook restoration. Visual appearance remains for user acceptance;
these results do not establish combat replay or other child-class coverage.
