# asmp-dll

Client DLL for the Steam executable. It is built as `asmp-diag.dll` by
[diagnostics/build.ps1](../diagnostics/build.ps1) and injected by the
diagnostic launcher; see [Build, test and review](../diagnostics/README.md).

| Path | Contents |
| --- | --- |
| [src/game/](src/game) | Engine API: verified addresses and layouts ([steam_profile.h](src/game/steam_profile.h)), probe, actor factory and combat API, menu/text bindings, and the vtable hooks (tick, world load, action, display) |
| [src/multiplayer/](src/multiplayer) | Production runtime ([runtime.c](src/multiplayer/runtime.c)), session state machine ([session.c](src/multiplayer/session.c)) and remote-player coordinator ([multiplayer.c](src/multiplayer/multiplayer.c)) with its position interpolation ([pose_buffer.c](src/multiplayer/pose_buffer.c)) |
| [src/multiplayer/client/](src/multiplayer/client) | Connection, packet validation and peer state; no native game entities |

`game/` does not include `multiplayer/` headers; it receives callbacks instead.
Layers, threading and replica lifecycle are described in
[Architecture and behavior](../docs/architecture.md).

`game/unit_puppet.*` provides the bounded ground/flying UNIT experiments (optionally
one linked class-9 weapon part):
validated Steam bindings, a single borrowed-entity registry and selective
action/movement/destructor hooks.
`diagnostics/src/monster_probe.*` feeds its
offline test poses; the opt-in network coordinator uses the same native API. See
[Monster checkpoints](../docs/monster-sync-stages.md) for ownership and limits.

`game/unit_combat.*` owns the isolated combat fixture and network client copy: one native
VID50 UNIT with a VID108 part, created from private shallow descriptor copies.
Animation scripts, object creations, damage/death scripts and shared counters
are isolated before the factory runs. Art/resource pointers remain borrowed;
the pinned DLL retains the copies through native destruction. This is a
visual attack/death API; authoritative damage replay remains outside this checkpoint.

`multiplayer/monster_sync.*` owns the opt-in single-monster network lifecycle,
world/session guards, bounded worker handoffs and game-thread application.
Host observation leaves native AI, commands, movement and damage untouched;
attack initiation is captured from native USE_AMMO actions (including zero-cost
melee), rather than inferred from snapshots. Client copies use private sanitized
VIDs, suppressed local AI/damage, native animation progression and immediate
removal of the combat entity on authoritative death. `game/unit_death.*` then
reproduces the host's normal corpse or alternate eight-gib presentation using
nine private visual VIDs, with scripts/damage/unit collisions disabled. The
coordinator retains the final pose and deduplicates both event and state death.
Native terrain stamps last until map reload; transient effects have bounded
cleanup. Runtime startup/stop includes these hooks and
waits for game-thread removal before restoring them. Only VID50 + VID108 is
supported; no network population, player-damage or projectile replication.

The optional death-gallery fixture uses64-capacity game-thread slot pools for
UNIT observation, private combat descriptors and death visual graphs (32 active
instances). Hooks remain shared and route by registered identity. Death graphs
use the selected stock UNIT's CRE15/16, validated before cloning, with32 visual
VIDs/128 effects per slot. Worker caches and coordinator IDs are independent;
`monster_sync_gallery` is configured before the first world tick.
