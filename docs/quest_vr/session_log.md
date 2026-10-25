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
