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
