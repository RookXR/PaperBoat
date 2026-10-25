# Open tasks

Keep this current: tick items off, add new ones, move finished phases to the status
table in `README.md`.

## Next up — Phase 2: immersive OpenXR screen + Touch input

Port BattleShip's Phase 2 (see its `docs/quest_vr_port.md`, "Phase 2 architecture"
and "Gotchas found in Phase 2"). Planned shape:

- [ ] Build switch: gradle property (e.g. `-Ppaperboat.quest=true`) that passes a CMake
      option (e.g. `PAPERBOAT_QUEST_XR=ON`), enables prefab + the Khronos
      `org.khronos.openxr:openxr_loader_for_android` dependency, sets a
      `BuildConfig.QUEST_XR` flag, and switches the game activity's intent category to
      `org.khronos.openxr.intent.category.IMMERSIVE_HMD`. Phone builds unchanged.
- [ ] `src/port/xr/quest_xr.cpp` (empty TU unless the option is on): OpenXR instance /
      session on SDL's EGL context, `xrWaitFrame`/`xrBeginFrame`, blit the game frame into a
      swapchain, submit one quad layer. Hook via `-Wl,--wrap=SDL_GL_SwapWindow` so
      libultraship stays untouched.
- [ ] Manifest: `com.oculus.supportedDevices` meta-data (without it Horizon OS runs a legacy
      mode: 72 Hz, no refresh-rate list).
- [ ] Touch controllers → virtual SDL gamepad (Xbox 360 VID/PID); ignore vendor `0x2833`
      devices so the virtual pad gets port 1. Decide a Paper Mario button mapping (A, B, Z,
      R, Start, C-buttons, stick).
- [ ] `MainActivity` in Quest mode: no touch overlay / Mods button, always report window
      focus, kill the process on finish (XR session is process-lifetime).
- [ ] Verify on device: screen visible, controls work, player 1, pacing.

## Phase 3 — stereo

- [ ] Per-eye projection shift. Paper Mario's camera projection is built in
      `src/cam_main.c` (`guMtxF2L(camera->mtxPerspective, &camPerspMatrix[...])` inside
      `FrameInterpolation_RecordOpenChild("camera_persp")`); the port's interpolation module
      is `src/port/interpolation/FrameInterpolation.cpp`. BattleShip did stereo by replaying
      the display list once per eye with replaced camera matrices.
- [ ] Inventory every `G_MTX_PROJECTION` load (~30 sites: `cam_main.c`, `model.c`,
      `effects.c`, `hud_element.c`, `msg.c`, battle, pause, effects/*) and decide which are
      3D cameras vs HUD/ortho.
- [ ] Render size / 120 Hz / display-clock pacing (BattleShip Phase 3.0); optimized native
      code is already the default here (Release).

## Later

- [ ] Passthrough around the screen, then the diorama cut-out (BattleShip 3p).
- [ ] Head-tracked window / room view so levels float in the room (BattleShip 3c–3e).
- [ ] Interaction: place/scale the level, HUD on the hand (BattleShip 4–7).

## Housekeeping

- [ ] Consider reporting the pointer-tag fix upstream (JeodC/libultraship, and
      Kenix3/libultraship if the check exists there) — affects all Android users.
