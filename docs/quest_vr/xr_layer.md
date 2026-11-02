# The XR layer: code map, CVars, design notes

How the Quest build's VR code fits together as of 2026-11-01 (Phases 2, 3a, 3p,
3e follow camera, 3h UI in the room view). Read this before changing `src/port/xr/quest_xr.cpp`.

## Code map

| Where | What |
|-------|------|
| `src/port/xr/quest_xr.cpp` | Everything XR: OpenXR instance/session/swapchain, Touch → virtual SDL pad, frame pacing, stereo, passthrough, cut-out presentation, room view. Only compiled with `PAPERBOAT_QUEST_XR`. Section order: setup → input → GL state helpers (`BlitState`, `DrawState`) → game texture copy (`CopyGameEye`) → `CopyGameFrame` → passthrough → `EndFrame` / `SubmitFrame` → stereo camera (`ShiftForEye`, `StereoCheck`) → room view (`RoomFov`, `SampleRoomViews`, `WorldToRoom`, `RoomClip`, `RoomEyeMatrix`, `RoomCheck`) → UI in the room view (`PatchHud`, `ShiftSprite`, `CopyHudImage`, `HudPanelPose`, `HudAnchoredLayer`, `HudPlaneFromCamera`) → `BeginStereoPair` → `extern "C"` API → `__wrap_SDL_GL_SwapWindow`. |
| `src/port/xr/quest_xr.h` | The C API the game and engine call, with no-op inline stubs for non-Quest builds (callers need no `#ifdef`). |
| `src/cam_main.c` | `port_xr_note_camera(mtx, eye, at, vpScale, vpTrans)` for every perspective camera matrix it writes (the projection-slot matrix is view×persp). |
| `src/background_gfx.c` | `gfx_draw_background`: `port_xr_background_cutout(list, 0)` at the top (frame opaque by default), `(list, 1)` in the default case right after the depth clear; if it returns 1 the backdrop, letterbox and margins are skipped. |
| `src/port/gfx_frame.c` | `Graphics_DrawFrame` (render thread) calls `port_xr_frame_background(backgroundList)` before drawing, so the XR layer knows whether this frame is a cut-out. |
| `src/main_loop.c` | `gfx_draw_frame`: `port_xr_hud_frame_start(list, frame camera matrix)`, then `XR_HUD_BEGIN/END` no-op brackets around the UI passes with a layer kind (`port_xr_note_hud`). |
| `src/battle/16C8E0.c` | `btl_draw_ui`: battle menus bracketed as `PORT_XR_HUD_PARTY`, anchored at a fixed stage point (`port_xr_note_hud_anchor`); the status bar stays outside (panel). |
| `src/hud_element.c` | `hud_element_draw_rect` wrapper: an element with a world anchor (`port_xr_hud_element_camera`) is bracketed and reported as a world sprite (`port_xr_note_world_sprite`). |
| `src/battle/states/select_target.c` | The target hand anchors its hud element to the target's world position (`port_xr_anchor_hud_element`). |
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
| `gXrHud` | 1 | UI on its own layers in the room view (0 = in the eye images, head-locked) |
| `gXrHudPanelWidth` / `Up` / `Back` | 0.9 / 0.15 / 0.4 m | steady panel (status bar, field menus): width of the 16:9 screen; center above / behind the room anchor |
| `gXrHudPartyWidth` | 1.28 m | battle-menu layer: width the 16:9 screen has on it |
| `gXrHudPartyRight` / `Up` / `Back` | 0.15 / -0.03 / 0 m | battle-menu layer: offset from its stage anchor (tuned with the owner) |
| `gXrHudPartyTilt` | 0° | battle-menu layer: lean back from vertical |
| `gXrHudScale` | 1 | picture plane (speech bubbles) shrunk about the look-at point; 1 = exact alignment |
| `gXrHudSpriteScale` | 1 | world sprites (target hand): size multiplier on top of the game's size against the scene |
| `gXrHudBacking` | 0 | debug: alpha of a black card behind every UI layer |
| `gXrHudCoverage` | 0 | debug: log each UI layer's coverage every ~5 s (GPU readback) |

Edit the config with the game force-stopped (it's only read at launch). The owner
hasn't changed any XR CVars from their defaults yet.

## Logs (`adb logcat -s paperboat.xr`)

`Passthrough active`, `Presenting from the game texture (GL n)`, `Diorama cut-out
on/off`, `Room view on/off`, `Pacing: … presents/s` (5 s), `Stereo check:` (window)
/ `Room check: … NDC error …` (room, ~10 s; one line per noted camera, duplicates
are the two display contexts). Healthy room check on 2026-11-01: NDC error ~1e-7,
`viewport 0,20-320,220`, `L eye fov x -2.40..1.86 y -1.43..0.97 (tan)`.

UI: `HUD framebuffer N for the panel|plane|party layer (1920x1080 for 320x240)`
(once; three of them), `HUD swapchain 1280x2160`, `UI on its layers (n range(s))` /
`UI in the eye images`, `HUD plane: center … size … viewport …` (~10 s),
`World sprite: anchor … game screen x,y -> eye x,y, scale f` (~10 s while the target
hand shows), `UI coverage | panel …% cols … rows …` (with `gXrHudCoverage`).
Set `adb logcat -G 16M` first: the default buffer lost most of a session.

## UI in the room view (Phase 3h)

Problem: in the room view the screen-space UI was drawn into each eye's image at
the same spot: head-locked, at the edge of the widened fov, unreadable.

**Routing.** `gfx_draw_frame` brackets UI passes with two `gDPNoOp`s each and
reports them per main display list with a kind. For a room-view pair,
`BeginStereoPair` → `PatchHud` rewrites the brackets into Fast3D's own `G_SETFB fb`
/ `G_RESETFB` (no libultraship change); after the pair (`port_xr_set_eye(-1)`) they
go back to no-ops. Ranges nest: an inner range's end switches back to the outer
range's framebuffer. What goes where:

| Pass (main_loop.c) | Kind | Shown as |
|------|------|----------|
| workers/hud elements back UI, game-mode back UI (field status bar, field menus), windows (field), CAM_HUD pass, front UI, game-mode front UI | `PORT_XR_HUD_PANEL` | steady panel above/behind the stage, facing the LOCAL origin (`HudPanelPose`) |
| battle menus (`btl_draw_ui` minus status bar), windows during battle | `PORT_XR_HUD_PARTY` | constant-scale "projector screen" facing room +z; its screen point (left edge, y 228 = foot of the menu wheel) sits at stage point (-260, 0, 60) mapped into the room like the scene, plus the offsets (`HudAnchoredLayer`) |
| `render_messages` (all three call sites) | `PORT_XR_HUD_PLANE` | the frame camera's picture plane: through its look-at point, facing it, spanning exactly what it shows there (`HudPlaneFromCamera`), so bubbles sit over the speaker |
| `render_curtains` (file-select theater) | `PORT_XR_HUD_PANEL` | around the file-select windows |
| screen overlays back/front (`xr_render_screen_overlay`, except `OVERLAY_BLUR`) | `PORT_XR_HUD_PLANE` | fades and wipes over the stage; the battle swirl skips its prev-frame blur in the room view |
| `CAM_TATTLE` render (`render_frame` loop) and the `EFFECT_TATTLE_WINDOW` UI effect | `PORT_XR_HUD_PARTY` | tattle close-up in its frame on the battle-menu layer |
| interact prompts, other `render_effects_UI` (damage numbers), `OVERLAY_BLUR` | none | stay in the eye images: 3D objects at world positions (room matrices); the blur samples an eye image |

**Framebuffers.** Four Fast3D framebuffers (the three layers + world sprites), `CreateFrameBuffer(320, 240, 320, 240,
resize=1)`: storage follows the game framebuffer (1920×1080) and the layout is the
game screen's, full 16:9 width (this port stretches the status bar to the widescreen
edges; a 4:3 crop cut off the coins). Kept non-inverted every pair
(`KeepHudUpright`, see `known_issues.md`). Cleared to (0,0,0,0) before each eye
(both eyes draw them). On the right eye's swap `CopyHudImage` copies them into
quarters of one 1280×2880 swapchain; `EndFrame` adds one quad per layer after the
projection layer, then the sprite quads (premultiplied alpha; quads always draw
over the scene).

**World sprites** (the battle target hand, enemy HP bars). `select_target.c` anchors
the hand's hud element (`port_xr_anchor_hud_element`), `btl_draw_enemy_health_bars`
each bar (`port_xr_anchor_stage_sprite`, keyed by the actor) to a world point and
the screen position the game derived; the draw is bracketed and reported with
`port_xr_note_world_sprite`. In a room-view pair the brackets become `G_SETFB`
sprite framebuffer / back to the range's framebuffer, so sprites draw unshifted at
their game-screen spot. In eye 0 `SpriteQuadFor` takes each sprite's bounding box
(`SpriteBounds`: `G_TEXRECT_WIDE`, `G_TEXRECT`, `G_FILLRECT`) as a sub-rectangle of
the sprite quarter, sized at the game's meters per pixel at the anchor (10 world
units up, `gXrHudSpriteScale`), placed so the anchor's screen point sits on the
anchor mapped into the room; upright, facing the head (hand) or room +z (stage
sprites). Log: `Sprite quad: anchor … box … -> w x h m at …` (~10 s). With
`gXrHud` 0 the old per-eye path remains (`ShiftSprite`, `TransformSpriteRects`
moves/scales texrects and fill rects in the eye images).

**Star-point banner:** its virtual entities are marked
(`port_virtual_entity_set_framed`, `level_up.c`) and skipped in the room view while
off the game camera's screen (`port_xr_room_view_active`).

**Not handled yet:** other `get_screen_coords` users (status icons, msg_api,
encounter); `OVERLAY_BLUR` (see `known_issues.md`).
