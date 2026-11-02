# Session log

One entry per session, newest at the bottom. Keep entries short: what was done,
what was learned, where we stopped. Details belong in the other docs.

---

## 2026-10-25 — Phase 1: APK on Quest, Android texture fix, forks, docs

**Done**
- Initialized submodules; installed NDK 30.0.15729638; built the stock debug APK (no
  code changes) and installed it over wireless adb. ROM picked in-headset, extracted to
  `pm64.o2r`. Game runs as a 2D panel in passthrough.
- Textures were badly corrupted. Ruled out, in order: the GPU palette shader (forced
  CPU decode, no change), Torch extraction (decoded archive textures offline, all
  correct), unsigned `char` (`-fsigned-char`, no change), uninitialized framebuffers
  (clear on allocation, no change). Texture dump + TLUT trace led to the root cause:
  libultraship drops OTR path commands whose pointer looks like a kernel address, and
  Android tags heap pointers. Fixed in libultraship; intro and demo now render
  correctly. See `known_issues.md` → Resolved.
- Owner forked PaperBoat and libultraship under RookXR. Pushed `quest-xr` to both;
  `.gitmodules` now points at `RookXR/libultraship` (`quest-xr`). Set commit email to
  the GitHub no-reply address.
- Created this `docs/quest_vr/` folder and a root `CLAUDE.md` pointing to it.

**State at end of session**
- Branch `quest-xr` (PaperBoat and libultraship), pushed.
- Installed APK on the headset = the fix build (plus nothing else).
- Leftover diagnostic dump files on the headset (see `known_issues.md`).

**Next:** Phase 2 — immersive OpenXR screen + Touch input (`open_tasks.md`).

---

## 2026-10-25 — Phases 2 + 3a: immersive OpenXR stereo screen, Touch input

**Done**
- Ported BattleShip's Phase 3a `quest_xr.cpp` (Phase 2 + 3.0 + 3a) to
  `src/port/xr/quest_xr.cpp`, behind `-Ppaperboat.quest=true` / `PAPERBOAT_QUEST_XR`.
  Gradle, CMake, manifest and `MainActivity` changes as in BattleShip.
- Stereo: `cam_main.c` notes each perspective camera matrix
  (`port_xr_note_camera`); `GameEngine::RunCommands` draws each present once per
  eye with `port_xr_eye_replacements`. No libultraship changes.
- Touch mapping chosen for Paper Mario (X = C-down partner ability, see
  `decisions.md`). Engine touch overlay and Mods button off in Quest builds.
- On device: OpenXR ready, 120 Hz granted, 1920×1080 per eye, 30.0 presents/s with
  exactly 3 repeat frames each, 0 resyncs; stereo self-check at-plane error 1.7e-08,
  far-point disparity as predicted. Owner played to the first save point:
  "looked and felt good" apart from the 2D background at screen depth.
- Removed personal details from the docs (LAN IP, local paths, ROM location,
  private email), then rewrote the two affected commits (`filter-branch` with a
  literal-replacement script) and force-pushed `quest-xr`, so they are gone from
  the branch history too. Added a local `pre-push` hook (PaperBoat and
  libultraship) that refuses pushes matching an untracked pattern list under
  `.git/`, documented in `CLAUDE.md` and `README.md`.

**State at end of session**
- Branch `quest-xr` committed and force-pushed to `rookxr` (history rewritten).
  libultraship unchanged.
- Installed APK on the headset = this Quest build.

**Next:** Phase 3b — backgrounds at infinity first (`open_tasks.md`).

---

## 2026-11-01 — Phase 3p (passthrough, cut-out) and 3e room view (follow camera)

**Done**
- Passthrough underlay and diorama cut-out. First device try was upside down
  (this libultraship's game texture is bottom-left origin, unlike BattleShip's)
  and smeared where the sky was (game frames only cleared depth); both fixed.
  Owner: floats over the room cleanly; pause menu and battles correct.
- Room view with the follow camera (projection layer, per-eye matrices from the
  head pose). Owner: "pretty good", walked up to the stage, got through a battle.
  `Room check` NDC error ~1e-7; viewport 0,20-320,220 (letterbox) handled; 30.00
  presents/s, 0 resyncs.
- Owner chose to have both the follow camera and a fixed dollhouse camera,
  switchable; follow first.

**State at end of session**
- libultraship `quest-xr`: `8b9360c1` (`LUS_GL_GAME_FB_ALPHA`). PaperBoat `quest-xr`:
  `dbc0e5f23` (3p), `379001e11` (room view), plus this docs commit. Not pushed yet.
- Installed APK on the headset = the room view build.

**Next:** HUD on its own panel (unreadable, head-locked), then the dollhouse
camera toggle (`open_tasks.md`). Added `xr_layer.md` (code map, CVars, HUD design
notes) for the next session. Pushing was pending the owner's go-ahead at the end
of this session: check `git log rookxr/quest-xr..HEAD` (PaperBoat) and
`git -C external/libultraship log origin/quest-xr..HEAD` before starting.

---

## 2026-11-01 — Phase 3h: UI in the room view

**Done**
- Wrist panel (BattleShip Phase 6 style): built, tried, reverted at the owner's
  request (UI should be in the scene, near what it applies to).
- Picture plane: whole UI on the game camera's picture plane. Owner: bubbles over
  the speaker, hand roughly on enemies, but the top bar cut off (4:3 crop of a
  widescreen status bar), zoom with the battle camera, depth mismatch, damage
  numbers misplaced (they were 3D effects pulled into the UI layer).
- Split (see `xr_layer.md` → "UI in the room view"): steady panel for the status
  bar, battle-menu projector screen at the stage's front left, speech bubbles on the
  picture plane, target hand per eye at its enemy (moved and scaled), damage numbers
  and prompts back in the scene. Full 16:9 width.
- Fixed missing battle-menu option text (Fast3D off-screen scissor vs `invertY`) and
  an all-UI-invisible bug from the third layer's uninitialized framebuffer id.
- Placement tuned with the owner on device: panel 15 cm above / 40 cm behind the
  anchor; battle menu at stage point (-260, 0, 60), width 1.28 m, offsets
  +0.15 m right, -0.03 m up. Owner: "looks perfect"; hand size "perfect".

**State at end of session**
- Committed and pushed to `rookxr/quest-xr` (libultraship unchanged).
- Installed APK = this build.

**Next:** hide the end-of-battle star-point banner while it's outside the game
camera's view (owner chose this, in progress); then enemy HP bars as world sprites
if wanted; then the dollhouse camera (`open_tasks.md`).

---

## 2026-11-01 — UI polish: theater, transitions, tattle, world sprites

**Done** (all verified on device by the owner)
- File-select theater (`render_curtains`) on the steady panel around the menu.
  Owner: "the curtains look good for the main menu".
- Screen overlays on the picture plane; battle swirl without its prev-frame blur
  in the room view (it read the other eye's image). Owner: "fades are much more
  comfortable".
- Tattle: `CAM_TATTLE` render and the `fx_tattle_window` frame on the battle-menu
  layer. Owner: title picture "looks great".
- Enemy HP bars as world sprites; all world sprites (hand, HP bars) now quads in
  the room from a fourth UI framebuffer, upright (no roll with the head). HP bars
  face the stage front so they don't swing when the head moves. Owner: "looks perfect".
- Committed the previous session's star-point banner hiding with it (not yet
  confirmed on device).

**State at end of session**
- Committed on `quest-xr`, not pushed (libultraship unchanged).
- Installed APK = this build.

- Pushed the above, then: transitions (fades, wipes, battle swirl) now fade the
  scene into the passthrough room instead of drawing a box on the picture plane
  (`port_xr_note_scene_fade`, copy-shader `uOpacity`). Owner: "looks perfect and
  is so much better"; UI staying up during fades is fine.

- Star-point counter on a mirrored "stage" layer at the stage's front right
  (+8 cm in after the first look). End-of-battle banner: a diagnostic log plus a
  headset screenshot burst (`adb exec-out screencap` takes ~8 s a frame, too slow
  for animation) showed the world-pass check never ran: the banner is a HUD-camera
  model drawn in the back-UI pass; now drawn back into the scene. Owner: "looks
  fantastic". Level-up banner not seen yet.

- Pushed. Status icons over actors (Danger, Peril, …) anchored as stage-facing
  world sprites; sprite quads behind the stage-front screens now stack under them
  (compositor quads don't depth-test). Owner: "fantastic", fixed for HP bars and the
  Danger marker.

**Next:** level-up banner when the owner levels up; `OVERLAY_BLUR` per-eye; the
sprite budget if markers go missing; then the dollhouse camera (`open_tasks.md`).
