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

## Stage 3: isolated combat fixture

```powershell
.\diagnostics\build.ps1 -Server
.\diagnostics\start-test.ps1 -MonsterCombat
```

This first combat checkpoint covers one ground UNIT VID50 and its single
linked SPRITE VID108. It exercises visual attack and native death15, with
local gameplay effects suppressed. It does not complete projectile/melee
damage replication, death16, mission integration or other monster types.

The copied survival script initializes the player but creates no monsters and
never calls the wave tick. After map initialization settles for two seconds,
the diagnostic module makes private 0x490-byte copies of the two loaded VIDs
and invokes the verified native factory once. Both copies disable all 17
animation scripts and creation links, delete/damage/collision scripts and
native radius damage. Private live/killed counters start at zero. The shared
loaded descriptors are compared byte-for-byte throughout the test.

Art, geometry and borrowed engine resource pointers remain intact; root links
to the private child descriptor. Unknown per-VID service callbacks, alternate
factory paths and longer child chains are rejected. The engine owns native
entity allocations. The fixture owns the private buffers, retained in the
pinned DLL until the complete native destructors finish; it never frees the
borrowed resource pointers. Normal death removes both parts through the native
animation update. Early stop removes the registered fixture once; a native
removal fault is abandoned until a world with no private entities is observed.

Observe without moving or firing during the automated sequence:

1. Idle for three seconds: no independent movement or attacks.
2. Inject local damage and a local HP mutation into root and part: both blocked.
3. Replay visual attack8 once; submit the same event again and reject it.
   Native attack frames must advance and both parts return to idle.
4. At ten seconds, replay native `SET_HP(0)` once and reject its duplicate.
   This selects death15; native death selection need not write the HP field.
5. Wait for both parts to disappear, with a twenty-second overall timeout.

Acceptance: terminal `MONSTER state=4 phase=5 cleanup=1 violations=0`, and
`MONSTER_COMBAT damage_blocked=1 replayed=2 duplicates=2 attack_frames>0
private_entities=0 unexpected_entities=0 sources_unchanged=1 fault=0`.
No new non-fixture objects may appear, and player health must stay unchanged.
Existing ambient objects may expire; a bounded baseline of entity/descriptor
identities detects new objects even if another object disappears.
A separate stop footer must
show `hook_restore_result=0 cleanup=1`. An interruption proves cleanup only.
Visual acceptance still requires watching the attack/death in the copied game.

`unit-puppet-test.exe`, `unit-combat-test.exe` and
`monster-combat-probe-test.exe` cover event deduplication, native animation
ownership, private descriptors before the factory, topology/identity checks,
source preservation, extra world effects, teardown and one-attempt fault
cleanup. The resource isolation suite covers script switching/rollback and
exclusive launch modes. Synthetic tests do not replace a live run.

The survival script backup follows the flying-mode mechanism. Switching
between flying/combat preserves the original backup; a non-fixture launch or
failed launch restores it. The original Steam installation is untouched.

Live result (2026-10-09, PID100948): terminal state4/phase5/cleanup1 with zero
violations, blocked local damage, two replayed events and two rejected
duplicates. Attack8 advanced four native frames and returned to idle. Native
death15 removed root and private part; read-only world scan confirmed both
identities absent and no remaining UNIT. No new objects/shared VID changes
were observed. Death disposal was immediate; multi-frame death art and death16
are not validated. Stop restored hooks successfully (`hook_restore_result=0
cleanup=1`). Visual appearance remains for user acceptance.

## First network checkpoint: one ordinary host monster and one client copy

```powershell
.\diagnostics\build.ps1 -Server
.\diagnostics\start-monster-network-test.ps1
# Or automate native host death after twenty seconds of host observation:
# .\diagnostics\start-monster-network-test.ps1 -KillAfterSeconds 20
# .\diagnostics\start-monster-network-test.ps1 -KillAfterSeconds 20 -DeathVariant gibs
.\diagnostics\stop-two-client-test.ps1
```

The managed pair runs copied survival maps. A's script creates one stock VID50
UNIT with its original VID108 part; native AI, targeting, movement, attacks and
damage continue normally. B's script creates no monsters. Neither script runs
survival waves. A claims server authority before B launches. The server locks
the first claim to that peer ID and connection generation until the room empties;
reconnecting does not transfer ownership, and there is no host migration.

`multiplayer/monster_sync.*` maps this one monster to ID1 within the host world
epoch. Host observation does not clear its behavior, goals or commands. Native
USE_AMMO actions from root/part capture attack initiation, including zero-cost
melee; animation snapshots never create attack events. Destruction/death generates
a separate event, and repeated absent snapshots also retain the authoritative
death. State includes map key, host epoch, ID, VID, position, direction, movement
intent/velocity, signed health, an event watermark and the host's death variant.
Monster protocol v3 uses 72-byte states and 52-byte events. Events carry their
own sequence, world/ID, tick, kind, position, direction and death variant (15/16).
The final death pose survives deletion of the host entity. Frames and timers stay native.

After the copied world settles, B creates the sanitized private VID50/108 fixture
through the validated native factory and attaches that specific entity. Positive
host health is applied through the native action. Local AI, damage and scripted
gameplay effects are suppressed. Explicit attacks replay native animation8; the
engine advances its frames and returns to idle/run. Death removes the combat
root and part, then creates a separate native visual presentation once. Normal
death uses VID51's falling animation and VID52's corpse stamp; alternate death
uses VID910 and eight VID151 gibs. A death tombstone prevents an earlier alive
snapshot from recreating the combat monster. A dead late join or lost event uses
the persistent death pose/variant from state, without replaying historical attacks.

`game/unit_death.*` owns nine shallow private visual descriptors. Its exact
observed creation DAG is validated before the factory runs; all scripts, damage,
health and unit collision masks are cleared. Art and native particle physics
remain borrowed. Successors are separately registered map entities and are all
tracked by private VID identity. Forced cleanup suppresses further creations,
checks topology and dispatches only the verified class5/8/9 destructors. Native
faults abandon after the guarded attempt; live effects expire after 30 seconds.
Native terrain stamps remain part of the map until reload, including after mod
stop. Buffers remain in the pinned DLL. Particle trajectories use local native
randomness; individual gibs are not network entities. The parent's ordinary
death cry is not replayed by this visual path.

Worker networking/logging and game-thread native operations use bounded queues
and latest-state handoffs. Captured events remain staged through lock contention;
overflow is counted. Failed creation/application cleans up and retries after
2,4,8,16,30 seconds, then every30 seconds. A failed guarded native removal abandons
the copy until a changed world no longer contains its private identities.
Runtime stop waits for game-thread cleanup before restoring UNIT/SPRITE hooks.
The pinned DLL retains the shallow private descriptors; borrowed art/resources
are never freed by the mod. The host monster is game-owned and only observed.

The worker/server validate lengths, versions, ranges, authority/session, world
and sequence. Fresh snapshots allow late join; historical attack events are not
replayed. B rejects stale/duplicate events and removes its copy when host data
expires after one second, on leaving the map, or on a world/session change.
Local player death is independent of world presence. A late join establishes
its event baseline from the initial snapshot. Attack packets use the current
transport without delivery guarantees; a lost attack may be absent visually,
whereas repeated dead state also reconstructs death presentation once.

Live checklist:

- Both windows show exactly one UNIT50; no waves or client AI decisions.
- A's native AI moves and turns; B follows with matching health and ID/epoch.
- Host native attack captures increment `MONSTER_NETWORK attacks`; B replays
  those events with native frame progression and returns to idle/run.
- Kill on A (or use the optional timed native death trigger): B removes both
  combat parts and reproduces the host's corpse/gib variant at its final pose.
  Neither duplicate events nor older alive snapshots recreate it.
- On separate runs, late join/restart B must create the current living copy or
  the dead monster's visual presentation once;
  expire/disconnect A and reload/stop must remove B's copy cleanly.
- Stop logs `cleanup=1` and successful hook restoration on both clients.

The normal launcher leaves games running for observation. The stop command
restores their survival scripts after verified cleanup; `-CloseGames` closes
only the managed pair. `-RestartClient B` preserves the mode/map in the manifest.
Tests: `monster-sync-test.exe`, `monster-transport-test.exe`, extended native
watch tests, `unit-death-test.exe`, resource isolation and managed session script suites.

Offline death validation: `start-test.ps1 -MonsterDeath` runs the isolated
combat sequence, then normal corpse and alternate gibs, with no survival waves.
Keep the player still for about35 seconds. `MONSTER_VISUAL` records must show a
corpse, eight gibs, fault0, violations0 and final cleanup1. Synthetic tests do
not establish rendering; compare both variants visually in the managed pair.

Death presentation validation (2026-10-10): full server/DLL build and all suites
pass. The offline fixture produced one animated body and eight gibs with zero
violations and successful cleanup. Native host/client runs reproduced normal
death and dead late join (one corpse, no living replica or historical attacks),
then alternate death (eight client gibs, matching final host pose/direction).
Both clients in the alternate run acknowledged cleanup1, hook restore0 and
installed0. Final terrain-stamp rendering still awaits the user's visual check;
allocation/lifecycle evidence is not a screenshot or visual acceptance.

The ordinary single-monster mode remains VID50 + one VID108. The separately enabled death gallery below expands that fixture only.
Client damage requests, authoritative damage to players, projectile/effect
replication, missions, population, other compositions and host migration are
deferred. Ordinary multiplayer launches do not enable this mode.

Historical network checkpoint (bd635e3, 2026-10-09, before death visuals): one native host UNIT50 and one sanitized client UNIT50
with registered VID108 parts were confirmed by read-only world inventories.
Position, direction and HP matched; native attack frames progressed on both.
The first pair captured29/replayed24 attacks after the client's late join and
settling interval, then timed native host death removed the client copy without
resurrection. A second untimed run recorded host35/client32 attacks and another
death/removal. No queue drops or native faults occurred. A final full build and
all suites passed, including absent-player/army world-preservation tests; the
user confirmed the repeated two-window behavior visually. Separate live stop,
reload and disconnection checklists are not established by these runs.

## Death gallery: different types and weapons

```powershell
.\diagnostics\start-monster-network-test.ps1 -Gallery
```

This opt-in fixture creates32 stationary monsters on host A, without survival
waves: four rows of eight copies, stock UNIT VIDs40,50,60,80 (row order follows
increasing Y). Copies in a row increase X,80 units apart; rows are100 units apart.
The first row starts at the player's initial X+160,Y-150. Move the camera/player
along the rows. All weapons/ammo are granted by the multiplayer runtime.
Use one copy for each weapon slot2..9. Fire in host A; client B displays the
same IDs, health and deaths. Client damage requests are a separate feature.
Neither side runs monster AI; native idle/death animation still advances.

Each instance owns independent bounded native identity, replica and death state.
The wire v3 admits IDs1..64 and these four stock types, with independent state
and event sequences in server/client caches. Runtime publishes the current
states as a bounded batch every player-state interval. Death effects clone that
monster's actual CRE15/16 graph; only acyclic, bounded class5/8/9 visuals are
admitted, all scripts/damage/collision disabled before factory. No other type's
corpse is substituted on failure. Art resources remain borrowed and DLL-pinned.
The effects setting may choose two or eight gibs. Each graph is limited to32
visual VIDs and128 live effects; graph faults use the existing recovery policy.

Known coverage gaps are visible test findings, not a successful result:
weapon6 freezing changes VID to49/59/69/89 and is not replicated by the current
15/16 death protocol. Armored types70/90 transform into another living UNIT and
are excluded. Individual gib trajectories remain native local randomness, and
the ordinary death cry is still absent at B. This fixture validates normal/gib
death coverage; it does not establish freeze, armor transformations, loot or score.

Logs include `MONSTER_GALLERY total=32 active=32 dead=0 faults=0` and per-ID
`MONSTER_GALLERY_UNIT` records with VID, behavior, health, variant and effect
counts. Kill one ID and verify B changes only that copy; try multiple simultaneous
deaths and restart B after deaths. A kill never resurrects from older live data.
For automated native lifecycle checks only, add `-KillAfterSeconds 25` (normal)
or `-KillAfterSeconds 25 -DeathVariant gibs`; these use native HP actions, not
weapon hits. Default launch leaves all32 alive for the user's weapon tests. The timed helper requires the first copy to stay alive until its timer fires; killing it early is a manual run, not an automated full-gallery result.
Restarting a client preserves gallery mode in the managed manifest. Stop uses
`stop-two-client-test.ps1`; copied survival scripts are restored after cleanup.

Gallery validation (2026-10-10): full server/DLL build and all suites passed,
including32 independent coordinator lifecycles and realUDP multi-ID/latejoin
checks. Both native clients began with32 living copies and behavior0; after
user weapon tests both reported32 deaths, zero native visual faults and zero
shot/monster queue drops. The user confirmed the two-window result visually.
This does not expand the freeze/armor transformation coverage described above.
