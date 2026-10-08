# Documentation index

Start with the [project README](../README.md) for an overview, then pick the
page for your task.

## Using and testing the mod

| Page | Contents |
| --- | --- |
| [Build, test and review](../diagnostics/README.md) | Build, preparing test copies and isolated DLL pairs, Git Bash server/two-client launch, one-PC Mirror test, two-copy launch verification, multiplayer menu and two-PC sessions, window and diagnostic modes, what the automated tests cover |

## How it works

| Page | Contents |
| --- | --- |
| [Architecture and behavior](architecture.md) | Layers, remote-player states, shots and aim, wire protocol, relay validation, world identity, health ownership, known limits |
| [asmp-dll](../asmp-dll/README.md) | Client DLL source layout: engine API (`game/`) and multiplayer runtime (`multiplayer/`) |
| [asmp-server](../asmp-server/README.md) | Relay server: running it and what it validates |
| [steam_profile.h](../asmp-dll/src/game/steam_profile.h) | Supported EXE hash, verified RVAs, offsets and layouts (comments record where each was confirmed) |
| [protocol.h](../common/src/protocol.h) | State and shot packet codecs |

## Libraries

| Page | Contents |
| --- | --- |
| [epnet](../common/epnet/README.md) | UDP transport used by the client and the relay server (submodule; run `git submodule update --init` if empty) |

## Local-only notes

These live in `analysis/`, which Git ignores; they exist only in a working copy
that has them. See `analysis/README.md` there for the full list.

- `analysis/steam-port-handoff.md`: current progress, untested changes, next
  step and the native address reference.
- `analysis/plans/steam-port-architecture-plan.md`: refactor phases and accepted
  decisions.
- `analysis/notes/steam-comparison.md`: comparison of the 2003 and Steam executables
  (in Russian).
