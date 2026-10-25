# PaperBoat on Meta Quest 3 — start here

Goal (owner): play Paper Mario (PaperBoat port) on a standalone Quest 3 with the
levels floating in the room in stereoscopic 3D, like the SSB64 Quest port in the
sibling repo `../BattleShip`. Path: 2D APK → immersive OpenXR
screen with Touch input → stereo → passthrough / diorama.

## Resuming in a new session

1. Read this file, then [`open_tasks.md`](open_tasks.md) (what's next) and the
   latest entry in [`session_log.md`](session_log.md) (where we stopped).
2. Skim [`decisions.md`](decisions.md) before changing direction on anything,
   and [`known_issues.md`](known_issues.md) before debugging.
3. [`build_and_deploy.md`](build_and_deploy.md) has the build/install/log
   commands and the device gotchas.
4. Reference implementation: BattleShip's `docs/quest_vr_port.md` (phase-by-phase
   write-up, ~125 KB; read it in chunks) and `port/xr/quest_xr.cpp`.

## Current status (update at the end of every session)

| Phase | Scope | Status |
|-------|-------|--------|
| 1 | Stock Android APK running on Quest as a 2D panel | **Done 2026-10-25.** Renders correctly after the libultraship pointer-tag fix. Touch controllers can't drive the game in 2D mode. |
| 2 | Immersive OpenXR app: game on a floating quad + Touch controller input | Not started — next. |
| 3 | Stereo 3D (per-eye camera matrices) | Not started. |
| 4+ | Passthrough, diorama / room view, interaction | Not started. |

## Repos and branches

All work goes on branch **`quest-xr`** in both repos.

| Repo | Remotes | Notes |
|------|---------|-------|
| PaperBoat (this repo) | `origin` = HarbourMasters/PaperBoat (upstream), `rookxr` = RookXR/PaperBoat (push here) | `quest-xr` tracks `rookxr/quest-xr`. |
| `external/libultraship` | `origin` = RookXR/libultraship (push here), `upstream` = JeodC/libultraship | `.gitmodules` points at the RookXR fork, branch `quest-xr` (based on JeodC `lus-converge`). |
| `external/torch` | unchanged (HarbourMasters/Torch) | |

Changing libultraship: commit inside the submodule, push to its `origin`, then
commit the submodule pointer bump in PaperBoat and push to `rookxr`.

Commit email is the GitHub no-reply address
`336722952+RookXR@users.noreply.github.com` (the private address is rejected on push).

## Files in this folder

| File | Purpose |
|------|---------|
| `README.md` | This index, status table, repo layout. |
| `build_and_deploy.md` | Toolchain, build, install, ROM, logs, device gotchas. |
| `decisions.md` | Decisions made and why (append-only). |
| `known_issues.md` | Open problems and resolved bugs with root causes. |
| `open_tasks.md` | Phase plan and the concrete next steps. |
| `session_log.md` | One entry per session: what was done, what's next. |
