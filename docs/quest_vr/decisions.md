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

### 2026-11-01 — Cut-out decided per frame by the game, alpha kept by libultraship

The backdrop is skipped in `gfx_draw_background`'s default case (fields, battles)
rather than hidden after the fact, and the decision is remembered per background
display list so the render thread presents exactly the frames that were built
without one (`port_xr_background_cutout` / `port_xr_frame_background`). libultraship
gets an opt-in `LUS_GL_GAME_FB_ALPHA` (Quest build only): RGBA8 game framebuffer,
"over" alpha blending, color cleared to alpha 0 at the start of every game frame
(this libultraship only cleared depth, so without it old frames smeared where the
backdrop used to be), and the game always draws into its framebuffer so its
texture can be read. **Why:** same approach as BattleShip 3p, proven; a fill rect
can't clear alpha (Fast3D's opaque shader writes alpha 1).

### 2026-11-01 — Room view: follow camera first, dollhouse camera later as a toggle

Owner's choice ("both, switchable"). The follow-camera room view maps the world
into the room by the noted camera's look-at point and heading (level, fixed
scale), not by decoding the matrix's view: Paper Mario passes eye/at with each
camera note anyway. The per-eye matrix is built in double from the images of the
origin and unit axes (the map is affine), keeping the game's depth mapping, and
the eye fov is widened across so pixels stay square (screen-space sprites/HUD).

### 2026-11-01 — UI in the scene, not on the wrist

A BattleShip-style panel over the left wrist was built first (whole UI layer on
one quad at the left controller's aim pose). The owner tried it and asked to revert:
UI belongs in the scene, roughly where it applies (bubbles over the speaker).
**Why:** Paper Mario's UI is large and contextual (dialogue, battle menus,
target pointer), unlike SSB64's few counters.

### 2026-11-01 — Split the UI by what it belongs to

A single picture plane (the whole UI on the game camera's image plane through its
look-at point) put bubbles and the hand roughly right, but zoomed with the battle
camera and put everything at one depth. Now: fixed UI on a steady panel (no zoom),
battle menus on a constant-scale screen anchored to the stage, messages on the
picture plane, the target hand per eye at its enemy's depth, 3D UI (damage numbers,
prompts) left in the scene. **Why:** owner feedback on each step; placement values
(panel 15 cm above / 40 cm behind; menu anchor and offsets) tuned with the owner on
device.

### 2026-11-01 — Battle menus as a projector screen at the stage's front left

Anchored at a fixed battle-world point (-260, 0, 60) rather than at Mario: anchored
at Mario it overlapped him and, facing the LOCAL origin, sat at a diagonal. It
faces room +z, has twice the panel's scale (`gXrHudPartyWidth` 1.28), and moves
with the stage when the battle camera pans. **Why:** owner: "it seems like it's
supposed to be like a projector … attached to the front left of the stage".

### 2026-11-01 — Transitions on the picture plane, theater on the panel

Screen overlays (fades, iris/Mario/star wipes, battle swirl, darkness stencils) are
drawn on the picture plane: over the stage like the TV they cover, not over the
whole view. The file-select theater (curtains) goes on the steady panel around the
file-select windows. **Why:** in the eye images they were head-locked, and shapes
at the same pixel in both eyes (asymmetric fovs) were seen double; the owner asked
for the border "to be the main menu". Trade-off accepted: a fade covers only what
the game camera shows, not scene parts outside that frame or the UI layers.
`OVERLAY_BLUR` and the battle swirl's previous-frame blur are left out (they sample
an eye image).

### 2026-11-01 — Tattle close-up with its window on the battle-menu layer

`CAM_TATTLE` (not a noted camera) and the `EFFECT_TATTLE_WINDOW` frame are drawn
into the party layer: a flat picture in its frame, not a stereo view. **Why:** in
the eye images both were head-locked; owner: the picture "looks good".

### 2026-11-01 — World sprites as quads in the room

The target hand and enemy HP bars draw into a fourth UI framebuffer and are each
shown on their own quad at their world anchor, sized like the game shows them
against the scene. The hand faces the head; HP bars face the stage front (room +z)
like the battle menus. **Why:** texture rectangles in the eye images can't rotate,
so they rolled with the head; HP bars facing the head swung under their enemy when
the owner moved. Owner: "looks perfect".

### 2026-11-01 — Transitions fade the scene into the room

In the room view a front-layer transition (color fade, Mario/star wipe, battle
swirl) isn't drawn: its coverage (progress/255) lowers the scene image's opacity,
so the world fades out to the passthrough room and back in. Wipe shapes and fade
colors are dropped; UI layers stay up. **Why:** the overlay on the picture plane
left the scene visible around a box; owner: "looks perfect and is so much better",
fine with the status bar staying up.

### 2026-11-01 — Star points at the stage's front right

The star-point counter gets its own layer (`PORT_XR_HUD_STAGE`), the battle-menu
layer's mirror image: same scale and height, anchored at the stage's front right,
8 cm further in than the mirrored menu offset (`gXrHudStageRight` -0.23 m).
**Why:** owner asked for it "on the opposite side of the stage from the battle
menu"; the 8 cm came from the first try on device. The end-of-battle tally moves
the counter toward the screen's middle on that layer.

### 2026-11-01 — End-of-battle banners are part of the scene

The "N Star Points" / "Level Up" banners are 3D HUD-camera models; they are drawn
back into the eye images with the room matrices (a 3D sign at their world
position), not on a UI layer. **Why:** drawn in the back-UI pass they landed,
room-projected, on the panel ("zooms in, very small on the HUD panel"); owner on
the fix: "looks fantastic".

### 2026-11-01 — Status icons as stage-facing sprites; sprite stacking by plane side

Status icons over actors are anchored like the HP bars (stage-facing). Sprite quads
behind the battle-menu/star-point screens (seen from the head) are submitted before
those layers. **Why:** owner: Danger marker "floats randomly in that HUD panel",
then (placed) "renders through the battle menu"; quads have no mutual depth test.
Owner on the fix: "fantastic … fixed both the health bars and the danger status".

### 2026-11-01 — 90 fps at 90 Hz

Frame interpolation in the Quest build is driven by `gXrFps` (default 90), not the
game's `InterpolationFPS`, and 90 runs the display at 90 Hz. **Why:** at 120 the GPU
sat at 91 % (top clock) and battles dropped to 93–118 presents/s (translation
judder); at 90: 90.00/s, GPU 70 %. Owner: 90 "seems just as smooth to me as 120".
