# Decisions

Append-only. Newest at the bottom. Each entry: date, decision, why, and what it
rules out. If a decision is reversed, add a new entry rather than editing the old one.

---

### 2026-10-25 — Follow the BattleShip phase plan

Build up in the same order as the SSB64 Quest port: 2D APK → immersive OpenXR quad
with Touch input → stereo → passthrough / diorama. Reuse BattleShip's
`port/xr/quest_xr.cpp` design (wrap `SDL_GL_SwapWindow`, reuse SDL's EGL context for
the OpenXR session, virtual SDL gamepad for Touch input) rather than inventing a new
architecture. **Why:** it is proven on the same headset and the same engine
(libultraship / Fast3D).

### 2026-10-25 — Everything lives in RookXR forks, branch `quest-xr`

PaperBoat changes go to `RookXR/PaperBoat`, libultraship changes to
`RookXR/libultraship` (forked from JeodC's fork, which PaperBoat upstream pins on
`lus-converge`). `.gitmodules` on `quest-xr` points at the RookXR libultraship fork.
**Why:** owner wants all changes under their account. **Rejected:** a patch file
applied to the submodule at build time (breaks on every submodule update).

### 2026-10-25 — Fix libultraship bugs in libultraship, not around them

The Android texture corruption was fixed at its root in libultraship's `gfx_step`
(strip the AArch64 pointer tag before the OTR address range check) rather than
working around it in the game (e.g. putting path strings in non-heap memory) or
disabling Android's heap pointer tagging in the manifest. **Why:** the root fix also
covers every other heap-allocated path and is small enough to upstream.

### 2026-10-25 — Keep fixes minimal and proven

Speculative changes tried during the texture investigation (`-fsigned-char` for
Android, clearing new framebuffers in `gfx_opengl.cpp`, keying libultraship's
`mDrawTextureCache` by path text as well as pointer) were reverted once the real
cause was found, because none of them was shown to be needed. They are recorded in
`known_issues.md` as latent risks instead.

### 2026-10-25 — Commit identity

Commits use `rookXR <336722952+RookXR@users.noreply.github.com>` (set globally).
**Why:** GitHub rejects pushes that expose the account's private email address.

### 2026-10-25 — Phases 2 and 3a in one step, from BattleShip's 3a code

Ported BattleShip's `port/xr/quest_xr.cpp` as of its Phase 3a commit (`1d5dafa`:
Phase 2 + 3.0 pacing + stereo cinema screen) rather than its current 6.7k-line
file, into `src/port/xr/quest_xr.cpp`. **Why:** the owner asked for immersive
stereo; the 3a file is the smallest proven unit that gives it, and the later
phases (room view, diorama, hands) depend on game-specific hooks that need
their own design here.

### 2026-10-25 — Stereo hooks: camera notes by Mtx pointer, eye loop in RunCommands

PaperBoat's interpolation (SoH-style `FrameInterpolation.cpp`) has no tagged
camera records like BattleShip's, so `cam_main.c` reports each perspective
camera matrix it writes (`port_xr_note_camera(&camPerspMatrix[id], eye, at)`).
At draw time `port_xr_eye_replacements` adds a per-eye shifted copy of every
noted matrix to the present's interpolation map (shifted from the interpolated
value if the map has one, else decoded from game memory). The eye loop is in
`GameEngine::RunCommands` (inner loop over eyes per interpolated present). No
libultraship changes. **Why:** Paper Mario loads view×perspective into the
projection slot like SSB64's battle camera, so BattleShip's matrix shift applies
unchanged; keying by pointer keeps the interpolation module untouched.
Ortho cameras and the HUD camera are not noted and stay at screen depth.

### 2026-10-25 — Touch button mapping

Left stick = stick, right stick = C buttons, A/B = A/B, X = C-down (partner
ability), Y = L, left trigger/grip = Z (spin dash), right trigger/grip = R,
left menu = Start. Same virtual Xbox 360 pad trick as BattleShip. No
`IgnoreVendorIdGlobally` (this libultraship doesn't have it, and feeds every
recognized pad to port 1 anyway).

### 2026-10-25 — Screen 0.9 m tall at 1.3 m, render 1920×1080 per eye

BattleShip's tuned default screen size/distance; infinity disparity cap scaled
to 2.5 cm for the smaller screen. PaperBoat's widescreen follows the window, so
the fixed 16:9 surface gives a widescreen game.
