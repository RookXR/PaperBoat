# Open tasks

Keep this current: tick items off, add new ones, move finished phases to the status
table in `README.md`.

## Done — Phase 2 + 3a (2026-10-25)

- [x] Build switch `-Ppaperboat.quest=true` → CMake `PAPERBOAT_QUEST_XR`, OpenXR loader
      prefab, `BuildConfig.QUEST_XR`, IMMERSIVE_HMD category on `MainActivity`.
- [x] `src/port/xr/quest_xr.cpp`: OpenXR on SDL's EGL context, `--wrap=SDL_GL_SwapWindow`,
      quad layer, 120 Hz, display-clock pacing, fixed 1920×1080 render size.
- [x] Manifest `com.oculus.supportedDevices`.
- [x] Touch controllers → virtual SDL gamepad (mapping in `decisions.md`).
- [x] `MainActivity` in Quest mode: no Mods button, always report focus, kill the
      process on finish, `setXrRenderSize`. Engine touch overlay off.
- [x] Stereo 3a: `port_xr_note_camera` in `cam_main.c`, eye loop in
      `GameEngine::RunCommands`, two quads (LEFT/RIGHT), self-check in the log.
- [x] Verified on device: screen, controls, pacing, depth (owner, to the first save point).

## Done — Phase 3p + 3e follow-camera room view (2026-11-01)

- [x] Passthrough underlay (`XR_FB_passthrough`, manifest feature, `gXrPassthrough`).
- [x] Cut-out: `background_gfx.c` skips backdrop/letterbox when
      `port_xr_background_cutout` says so; `gfx_frame.c` tells the XR layer which
      frame is drawn (`port_xr_frame_background`). libultraship `LUS_GL_GAME_FB_ALPHA`
      (RGBA8 game FB, clear to alpha 0 each frame, "over" alpha, always draw to the FB).
      Eye images copied from the game texture with alpha; quads/projection blend.
- [x] Room view (`gXrRoomView`, follow camera): `RoomEyeMatrix`, projection layer,
      `Room check` log. Placement CVars `gXrRoomDistance` / `gXrRoomHeight` /
      `gXrRoomUnitsPerMeter`.

## Done — Phase 3h UI in the room view (2026-11-01)

- [x] UI drawn into its own Fast3D framebuffers (brackets in `gfx_draw_frame`,
      `G_SETFB`/`G_RESETFB`), three layers: steady panel (status bar), battle-menu
      projector screen at the stage's front left, speech bubbles on the picture plane.
- [x] Target hand moved and scaled per eye onto its enemy (world sprite).
- [x] Damage numbers and prompts left in the scene (3D at world positions).
- [x] Battle menu option text (scissor fix: framebuffers kept non-inverted).

## Next up — room view polish (owner chose "both, switchable", 2026-11-01)

- [x] **End-of-battle star-point banner** (2026-11-01): drawn in the scene at its
      world position, hidden while off the frame camera's screen. Owner: "looks
      fantastic".
- [ ] **Level-up banner**: same path (`InitLevelUpModelFlags`), not seen yet; check
      at the owner's next level up.
- [x] Star-point counter on its own layer at the stage's front right, mirroring the
      battle menus (2026-11-01).
- [x] Enemy HP bars as world sprites (2026-11-01): stage-facing quads under each
      enemy.
- [x] Status icons over actors (Danger, Peril, statuses, boosts) as stage-facing
      world sprites; sprites behind the stage-front screens stack under them
      (2026-11-01). Other `get_screen_coords` UI (msg_api, encounter) still lands on
      the steady panel; same treatment if it matters.
- [ ] Sprite budget: 10 quads (hand + HP bars + status icons). Crowded battles with
      many statuses may drop some; merge per actor if the owner sees it.
- [x] Screen overlays (fades, wipes, battle swirl), file-select theater, tattle
      close-up + frame off the eye images (2026-11-01).
- [x] Transitions fade the scene into the passthrough room instead of a box
      (2026-11-01).
- [ ] `OVERLAY_BLUR` (prev-frame motion blur: Kammy, Mega Quake, slow-blur
      transition) still draws in the eye images and samples the other eye's image
      (one shared prev-frame mirror). Fix: a mirror per eye, or skip it in the room view.
- [ ] Speech bubbles on the picture plane are only exact at the look-at depth, and
      the plane follows the battle camera's zoom. Owner: good enough so far.
- [ ] **Fixed "dollhouse" camera per map** as a toggle (owner's choice "both"):
      camera fixed on the map's bounds so the area stands still on the table and
      Mario walks inside it; fallback to follow for maps that don't fit.
- [ ] Smoothness: 30 fps means head translation judders between presents
      (rotation-only reprojection). BattleShip fixed it with 120 fps frame
      interpolation + `XR_EXT_performance_settings`. Not yet reported by the owner.
- [ ] Sharpness: the 16:9 render target wastes ~half its pixels on the widened fov
      (`L eye fov x -2.40..1.86`). BattleShip: eye-shaped render target +
      `LUS_WIDESCREEN_ALLOW_NARROW`.
- [ ] Tuning: scale / distance / height over adb (`paperboat.cfg.json`).

## Phase 3b leftovers (window mode only)

- [ ] **2D map backgrounds at infinity.** `background.c` `appendGfx_background_texture`
      (via `background_gfx.c` `gfx_draw_background` / `port_appendGfx_background_texture`)
      draws the sky as screen-space texture rectangles, so it sits at screen depth
      while far scenery is behind the screen. Do what BattleShip 3b.3 did for its
      wallpaper: note the background's DL range, and per eye shift its rects by where
      a point at infinity lands (P00·sep/conv/2 in NDC, times any widescreen clip-x
      scale), restoring them after the draw. Check `BackgroundPatches.cpp` first:
      the port already rewrites the background for widescreen.
- [ ] **Projection-load inventory.** Only `cam_main.c`'s camera load is shifted.
      Check `battle/16C8E0.c` (tattle camera `fpDoPreRender`) and the
      `matrixStack` projection loads in `effects/*.c`, `draw_box.c`, `msg.c`,
      `pause/*.c`, `screen_overlays.c`, `screen_render_util.c`,
      `status_star_shimmer.c`, `curtains.c`, `Credits.inc.c`, `model.c:4889`
      (`G_MTX_MUL` onto the projection): ortho/2D (fine flat) or 3D (needs a note)?
- [ ] **CPU-projected sprites** (`get_screen_coords` users: "!" markers, damage
      numbers, speech bubbles anchored to actors) sit at screen depth. Owner hasn't
      noticed any yet; look once 3b's other items are done.
- [ ] Freeze frames / idle presents (BattleShip 3b.1): check whether PaperBoat ever
      presents without a new DL (pause, loading) and whether depth is lost then.
- [ ] Separation/convergence tuning over adb (`gXrStereoSeparation` 0.01 →
      infinity 0.7 cm; cap allows ~0.035). Owner is happy with the default for now.

## Later

- [ ] Render rate: game presents at 30 fps (`InterpolationFPS` default). Try 60
      (interpolation) for smoother motion; pacing already handles 120/60.
- [ ] Passthrough around the screen, then the diorama cut-out (BattleShip 3p).
- [ ] Head-tracked window / room view so levels float in the room (BattleShip 3c–3e).
- [ ] Interaction: place/scale the level (BattleShip 4–5). (HUD on the hand was
      tried 2026-11-01 and dropped, see `decisions.md`.)
- [ ] Engine menu (ImGui) access in VR: it was opened by a touch button on phones.

## Housekeeping

- [ ] Consider reporting the pointer-tag fix upstream (JeodC/libultraship, and
      Kenix3/libultraship if the check exists there) — affects all Android users.
