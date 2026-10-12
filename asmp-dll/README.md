# asmp-dll

Client DLL for the Steam executable. It is built as `asmp-diag.dll` by
[diagnostics/build.ps1](../diagnostics/build.ps1) and injected by the
diagnostic launcher; see [Build, test and review](../diagnostics/README.md).

| Path | Contents |
| --- | --- |
| [src/game/](src/game) | Engine API: verified addresses and layouts ([steam_profile.h](src/game/steam_profile.h)), probe, `world_list.h` (read-only native list access), `entity_class.h` (entity class to vtable), actor factory and combat API, menu/text bindings, and the vtable hooks (tick, world load, action, display) |
| [src/multiplayer/](src/multiplayer) | Production runtime ([runtime.c](src/multiplayer/runtime.c)), session state machine ([session.c](src/multiplayer/session.c)) and remote-player coordinator ([multiplayer.c](src/multiplayer/multiplayer.c)) with its position interpolation ([pose_buffer.c](src/multiplayer/pose_buffer.c)); retry timing shared by the coordinators is in [retry_policy.h](src/multiplayer/retry_policy.h) |
| [src/multiplayer/client/](src/multiplayer/client) | Connection, packet validation and peer state; no native game entities |

`game/` does not include `multiplayer/` headers; it receives callbacks instead.
Layers, threading and replica lifecycle are described in
[Architecture and behavior](../docs/architecture.md).

`game/unit_puppet.*` provides the bounded ground/flying UNIT experiments (optionally
one linked class-9 weapon part):
validated Steam bindings, 64 fixed `UnitPuppet` slots (each borrows one entity) and
selective action/movement/destructor hooks that find the owning slot by entity.
`diagnostics/src/monster_probe.*` feeds its
offline test poses; the opt-in network coordinator uses the same native API. See
[Monster checkpoints](../docs/monster-sync-stages.md) for ownership and limits.

`game/unit_combat.*` owns the isolated combat fixture and network client copy: one native
VID50 UNIT with a VID108 part, created from private shallow descriptor copies.
Animation scripts, object creations, damage/death scripts and shared counters
are isolated before the factory runs. Art/resource pointers remain borrowed;
the pinned DLL retains the copies through native destruction. This is a
visual attack/death API; authoritative damage replay remains outside this checkpoint.

`game/unit_puppet.*`, `game/unit_combat.*` and `game/unit_death.*` share one
return convention, `enum UnitResult` in `game/unit_result.h`: `UNIT_OK`,
`UNIT_REFUSED` (not done) and `UNIT_FAULT` (native fault).

Slots are handles: `unit_puppet_slot(i)`, `unit_combat_slot(i)` and
`unit_death_slot(i)` return opaque `UnitPuppet*`, `UnitCombat*` and `UnitDeath*`,
and every function takes its slot as the first argument. There is no "current
slot" global or macro. `MonsterSlot` (monster_sync.c), `UnitPuppet`, `UnitCombat`
and `Remote` (multiplayer.c) group their fields into nested structs such as
`world`, `retry`, `form`, `prediction` and `stats`. Every `.c` file starts with a
comment naming its module and layer.

`multiplayer/monster_sync.*` owns the opt-in single-monster network lifecycle,
world/session guards, bounded worker handoffs and game-thread application.
Host observation leaves native AI, commands, movement and damage untouched;
attack initiation is captured from native USE_AMMO actions (including zero-cost
melee), rather than inferred from snapshots. Client copies use private sanitized
VIDs, suppressed local AI/scripts, native animation progression and immediate
removal of the combat entity on authoritative death. `game/unit_death.*` then
reproduces the host's normal corpse or alternate eight-gib presentation using
nine private visual VIDs, with scripts/damage/unit collisions disabled. The
coordinator retains the final pose and deduplicates both event and state death.
Native terrain stamps last until map reload; transient effects have bounded
cleanup. Runtime startup/stop includes these hooks and
waits for game-thread removal before restoring them. The default single-monster
fixture uses VID50 + VID108; the gallery below covers additional forms.
Network population, player-damage and projectile replication remain separate.

The optional death-gallery fixture uses64-capacity game-thread slot pools for
UNIT observation, private combat descriptors and death visual graphs (48 active
instances). Hooks remain shared and route by registered identity. Death graphs
use the selected stock UNIT's CRE15/16, validated before cloning, with32 visual
VIDs/128 effects per slot. Worker caches and coordinator IDs are independent;
`monster_sync_gallery` is configured before the first world tick.
Form revisions keep a frozen form and an armor successor under the same network
ID. Host armor graphs own bounded private successor descriptors; client armor
graphs omit living successors. Frozen class9 ice roots retain their sanitized
birth effects. Private behavior records disable AI before host successor creation.
Large armor loss borrows the medium shell's14-fragment visual graph on both
peers, independently of the native90->80 successor. The host owns the extra
effect in its armor bank; medium host armor already emits native fragments.

Client copies opt into native incoming damage for visual prediction, with zero
creation links and outgoing gameplay effects. The coordinator keeps provisional
HP/forms/death separate from host authority, deduplicates confirmation and owns
correction cleanup. Armor pending states display60/80 immediately. The passive
offline probes still block damage. See [impact prediction](../docs/monster-sync-stages.md#client-impact-prediction)
for the750ms grace, confirmation heuristic and remaining combat limitations.
