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

Commits use `rookXR <336722952+RookXR@users.noreply.github.com>` (set globally, and
in Majoras-Mask-VR's local config). **Why:** GitHub rejects pushes that expose the
account's private email address.
