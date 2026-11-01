# The XR layer: code map, CVars, design notes

How the Quest build's VR code fits together as of 2026-11-01 (Phases 2, 3a, 3p,
3e follow camera). Read this before changing `src/port/xr/quest_xr.cpp`.

## Code map

| Where | What |
|-------|------|
| `src/port/xr/quest_xr.cpp` | Everything XR: OpenXR instance/session/swapchain, Touch → virtual SDL pad, frame pacing, stereo, passthrough, cut-out presentation, room view. Only compiled with `PAPERBOAT_QUEST_XR`. Section order: setup → input → GL state helpers (`BlitState`, `DrawState`) → game texture copy (`CopyGameEye`) → `CopyGameFrame` → passthrough → `EndFrame` / `SubmitFrame` → stereo camera (`ShiftForEye`, `StereoCheck`) → room view (`RoomFov`, `SampleRoomViews`, `RoomClip`, `RoomEyeMatrix`, `RoomCheck`, `BeginStereoPair`) → `extern "C"` API → `__wrap_SDL_GL_SwapWindow`. |
| `src/port/xr/quest_xr.h` | The C API the game and engine call, with no-op inline stubs for non-Quest builds (callers need no `#ifdef`). |
| `src/cam_main.c` | `port_xr_note_camera(mtx, eye, at, vpScale, vpTrans)` for every perspective camera matrix it writes (the projection-slot matrix is view×persp). |
| `src/background_gfx.c` | `gfx_draw_background`: `port_xr_background_cutout(list, 0)` at the top (frame opaque by default), `(list, 1)` in the default case right after the depth clear; if it returns 1 the backdrop, letterbox and margins are skipped. |
| `src/port/gfx_frame.c` | `Graphics_DrawFrame` (render thread) calls `port_xr_frame_background(backgroundList)` before drawing, so the XR layer knows whether this frame is a cut-out. |
| `src/port/Engine.cpp` | `GameEngine::RunCommands`: per interpolated present, eye loop `port_xr_set_eye(0/1)` + `DrawAndRunGraphicsCommands(…, port_xr_eye_replacements(…))`; `ProcessGfxCommands` hands the present rate to pacing. |
| `CMakeLists.txt` | Quest block: OpenXR loader, `--wrap=SDL_GL_SwapWindow`, `LUS_GL_GAME_FB_ALPHA` on the `libultraship` target. |
| `android/app/src/main/AndroidManifest.xml` | `com.oculus.feature.PASSTHROUGH` (required=false), supported devices. |
| libultraship `quest-xr` | `3e06f3cc` Android pointer-tag fix; `8b9360c1` `LUS_GL_GAME_FB_ALPHA` (`gfx_opengl.cpp`: RGBA8 FBs, "over" alpha blend, clear alpha 0; `interpreter.cpp`: game frame clears color too, always render to the game FB). |

## Frame flow (stereo, cut-out, room view)

1. Game thread builds background + main display lists. `gfx_draw_background`
   asks `port_xr_background_cutout` → remembered per background list.
2. Render thread: `Graphics_DrawFrame` → `port_xr_frame_background` sets
   `gXr.cutoutDraw` for this frame.
3. `RunCommands` → `port_xr_set_eye(0)` → `BeginStereoPair`: room view if the
   frame is a cut-out, `gXrRoomView` is on and `xrLocateViews` gives valid poses
   for the refresh the pair is due on (`nextPresentTime`). Fov widened across
   (`RoomFov`) so pixels are square on the 16:9 image.
4. `port_xr_eye_replacements(eye)` replaces every noted camera matrix with
   `RoomEyeMatrix` (room pairs) or `ShiftForEye` (window stereo).
5. Each eye's swap: left → `StashLeftEye` (copy of the game texture); right →
   `SubmitFrame` → `CopyGameFrame` (both halves into one swapchain image, records
   `imageStereo/imageCutout/imageRoom/imageViews`) → `EndFrame`: passthrough
   underlay, then a projection layer (room) or one quad per eye (window),
   alpha-blended when the image is a cut-out.

Facts that cost time:
- This libultraship's game FB texture has the **GL bottom-left origin** (no v flip
  in the copy shader). BattleShip's needed a flip.
- Fast3D maps a full 320×240 viewport over the whole framebuffer and squeezes 3D
  vertex x by (4/3)/aspect = 0.75 at 16:9 (`AdjXForAspectRatio`). Texrects keep
  their native coordinates scaled to the framebuffer.
- Field cameras use a letterboxed viewport (0,20–320,220); `RoomClip` maps through
  each camera's viewport.
- Matrix replacements pass through 16.16 via 32-bit `int` in `GfxSpMatrix`
  (`interpreter.cpp`): entries must stay under ±32768 (BattleShip hit an overflow
  with large z/w; not an issue at the current values).
- A Fast3D fill rect can't clear alpha (opaque render modes output alpha 1);
  that's why the clear is in libultraship.

## CVars (in `paperboat.cfg.json` under `CVars`; all read live)

| CVar | Default | Meaning |
|------|---------|---------|
| `gXrStereo` | 1 | 0 = flat (one image to both eyes); first A/B when stereo renders wrong |
| `gXrStereoSeparation` / `gXrStereoConvergence` | 0.01 / 0.85 | window stereo only |
| `gXrStereoSwapEyes` | 0 | debug |
| `gXrPassthrough` | 1 | room visible around/behind the game |
| `gXrDiorama` | 1 | cut-out: fields and battles draw no backdrop (needs passthrough) |
| `gXrAlphaView` | 0 | debug: show the game's alpha as grey |
| `gXrRoomView` | 1 | room view for cut-out frames (0 = window screen) |
| `gXrRoomDistance` / `gXrRoomHeight` | 1.0 / -0.35 m | camera look-at point ahead of / above the LOCAL origin (recenter: hold Meta) |
| `gXrRoomUnitsPerMeter` | 300 | model scale (Mario ≈ 12 cm) |

Edit the config with the game force-stopped (it's only read at launch). The owner
hasn't changed any XR CVars from their defaults yet.

## Logs (`adb logcat -s paperboat.xr`)

`Passthrough active`, `Presenting from the game texture (GL n)`, `Diorama cut-out
on/off`, `Room view on/off`, `Pacing: … presents/s` (5 s), `Stereo check:` (window)
/ `Room check: … NDC error …` (room, ~10 s; one line per noted camera, duplicates
are the two display contexts). Healthy room check on 2026-11-01: NDC error ~1e-7,
`viewport 0,20-320,220`, `L eye fov x -2.40..1.86 y -1.43..0.97 (tan)`.

## Next: HUD panel — design notes

Problem: in the room view all screen-space UI (HP/FP/coins, battle command menus,
message boxes, popups) is drawn into each eye's image at the same spot: head-locked,
at the edge of the widened fov, unreadable.

What is known so far (nothing built yet):
- `gfx_draw_frame` (`src/main_loop.c`) draws in passes, each wrapped in
  `FrameInterpolation_RecordOpenChild`: `gfx_world` = `render_frame(false)` (the 3D
  world), then `gfx_interact_prompts`, `gfx_back_ui` (screen overlays, workers,
  hud elements, `render_effects_UI`, game mode back UI), `gfx_window_root`,
  `gfx_world_hud` = `render_frame(true)`, `gfx_messages_mid`, then front UI. So,
  unlike SSB64, most of the UI is in a few contiguous passes after the world —
  candidate: bracket everything after `render_frame(false)` as one HUD range.
  Check what `render_frame(true)` draws and whether some "UI" passes are
  world-anchored (e.g. effects UI, interact prompts, damage numbers) and should
  stay in the scene.
- BattleShip Phase 6 (`../BattleShip/docs/quest_vr_port.md`, "Phase 6") did it
  with **no libultraship change**: decomp emits `gDPNoOp` brackets around the HUD
  and reports them (`port_xr_note_hud(begin, end)`); per room-view pair the XR
  layer patches the brackets into Fast3D's `G_SETFB fb` / `G_RESETFB` so the HUD
  draws into its own framebuffer (created with
  `Interpreter::CreateFrameBuffer(320, 240, 320, 240, resize=1)`; `resize` on is
  essential, see their 6b "Framebuffer fix"), cleared to (0,0,0,0) each eye pass,
  copied into a HUD swapchain and shown on a quad (premultiplied alpha). Their
  first placement was a test quad above the stage; final was the left hand.
- Placement options to ask the owner about: floating panel near/above the scene,
  fixed in front, or on the left hand like BattleShip. Battle menus are interactive
  and large in Paper Mario, so a fixed readable panel may suit better than the wrist.
- Window mode / menus (non-room frames) should keep the HUD in the image.
