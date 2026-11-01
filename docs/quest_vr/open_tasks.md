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

## Next up — room view polish (owner chose "both, switchable", 2026-11-01)

- [ ] **HUD on its own panel.** In the room view the HUD (HP/FP/coins, battle menus)
      is screen-space texrects in each eye's image: head-locked, at the edge of a
      widened field of view, unreadable (owner). Needs the HUD separated from the
      scene: BattleShip Phase 6 drew its HUD into its own Fast3D framebuffer
      (decomp brackets + `G_SETFB`) and showed it on a quad. In PaperBoat the HUD is
      drawn after `render_frame(false)` in `gfx_draw_frame` (`main_loop.c`).
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
- [ ] Interaction: place/scale the level, HUD on the hand (BattleShip 4–7).
- [ ] Engine menu (ImGui) access in VR: it was opened by a touch button on phones.

## Housekeeping

- [ ] Consider reporting the pointer-tag fix upstream (JeodC/libultraship, and
      Kenix3/libultraship if the check exists there) — affects all Android users.
