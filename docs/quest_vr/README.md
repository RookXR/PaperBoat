# PaperBoat on Meta Quest 3 — start here

Goal (owner): play Paper Mario (PaperBoat port) on a standalone Quest 3 with the
levels floating in the room in stereoscopic 3D, like the SSB64 Quest port in the
sibling repo BattleShip (checked out next to this one, `../BattleShip`). Path: 2D
APK → immersive OpenXR screen with Touch input → stereo → passthrough / diorama.

## Resuming in a new session

1. Read this file, then [`open_tasks.md`](open_tasks.md) (what's next) and the
   latest entry in [`session_log.md`](session_log.md) (where we stopped).
2. Skim [`decisions.md`](decisions.md) before changing direction on anything,
   and [`known_issues.md`](known_issues.md) before debugging.
3. [`build_and_deploy.md`](build_and_deploy.md) has the build/install/log
   commands and the device gotchas.
4. [`xr_layer.md`](xr_layer.md): code map of the VR code, frame flow, CVars,
   log lines, and how the UI is placed in the room view.
5. Reference implementation: BattleShip's `docs/quest_vr_port.md` (phase-by-phase
   write-up, ~125 KB; read it in chunks) and `port/xr/quest_xr.cpp`.

## Current status (update at the end of every session)

| Phase | Scope | Status |
|-------|-------|--------|
| 1 | Stock Android APK running on Quest as a 2D panel | **Done 2026-10-25.** Renders correctly after the libultraship pointer-tag fix. Touch controllers can't drive the game in 2D mode. |
| 2 | Immersive OpenXR app: game on a floating quad + Touch controller input | **Done, verified on device 2026-10-25.** `-Ppaperboat.quest=true`; 0.9 m screen at 1.3 m, 1920×1080/eye, 120 Hz, display-clock pacing (30.0 presents/s, 0 resyncs). Owner played to the first save point. |
| 3a | Stereo cinema screen (per-eye camera matrices, one quad per eye) | **Done, verified on device 2026-10-25.** Owner: looks and feels good. Self-check passes. |
| 3b | Stereo polish: 2D map backgrounds at infinity, CPU-projected sprites, projection-load inventory | Backgrounds: superseded by 3p (no backdrop in cut-out scenes). Rest not started. |
| 3p | Passthrough + diorama cut-out | **Done, verified on device 2026-11-01.** Room around the screen (`gXrPassthrough`); field and battle scenes draw no backdrop and float over the room (`gXrDiorama`). Menus, pause, title opaque. Needs libultraship `LUS_GL_GAME_FB_ALPHA`. |
| 3e | Room view: the scene as an object in the room | **Follow-camera version done, verified on device 2026-11-01** (`gXrRoomView`): per-eye matrices from the tracked head pose, projection layer, world level and at 300 units/m, camera look-at 1.0 m ahead / 0.35 m below eye level. Owner: "pretty good", walked up to it, played a battle. HUD still head-locked and unreadable. Fixed "dollhouse" camera per map: next, as a toggle. |
| 3h | UI in the room view (HUD) | **Done, verified on device 2026-11-01.** UI drawn into its own Fast3D framebuffers and shown as layers in the room: status bar on a steady panel above the stage, battle menus as a "projector screen" at the stage's front left, speech bubbles on the game camera's picture plane, battle target hand moved and scaled per eye onto its enemy; damage numbers and prompts stay 3D in the scene. Owner: menu placement "perfect", hand size "perfect". A wrist panel was tried first and dropped. **Polish 2026-11-01 (verified):** file-select theater on the panel, fades/wipes on the picture plane, tattle close-up and its frame on the battle-menu layer, world sprites (hand, enemy HP bars) as upright quads in the room (HP bars face the stage front). |
| 4+ | Dollhouse camera, interaction | Not started. |

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

## Before pushing: no personal info

The forks are public. Docs, comments and commit messages must not contain the
owner's local file paths (Windows user folder, drive letters), LAN IP addresses
(the headset's wireless-adb address), ROM locations, or private email addresses.
Write `<repo>`, `<headset-ip>` and relative paths instead. The GitHub no-reply
address above is fine. Keep machine-specific values out of the repo (local notes
or the assistant's memory are the place for them).

The check is automatic on the owner's machine: a `pre-push` hook (in this repo
and in `external/libultraship`) scans the commits being pushed, contents and
messages, against a list of patterns and refuses the push on a match. The hook
and the pattern list live under `.git/` (`hooks/pre-push`, `info/private-patterns`),
so they are never committed — the patterns themselves are personal info. On a
fresh clone they don't exist: re-create them (ask the owner for the values)
before pushing, or review the diff by hand.

If the hook blocks a push, fix the commits (amend, or rewrite older ones) and
push again. Never bypass it with `--no-verify`. If something was already pushed,
rewrite the affected commits and force-push (done once, 2026-10-25).

## Files in this folder

| File | Purpose |
|------|---------|
| `README.md` | This index, status table, repo layout. |
| `build_and_deploy.md` | Toolchain, build, install, ROM, logs, device gotchas. |
| `xr_layer.md` | VR code map, frame flow, CVars, logs, next-task design notes. |
| `decisions.md` | Decisions made and why (append-only). |
| `known_issues.md` | Open problems and resolved bugs with root causes. |
| `open_tasks.md` | Phase plan and the concrete next steps. |
| `session_log.md` | One entry per session: what was done, what's next. |
