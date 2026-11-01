# Known issues

## Open

### HUD is head-locked and unreadable in the room view
The HUD and battle menus are screen-space texrects, drawn into each eye's image at
the same spot: they ride on the face, at the edge of the widened field of view, too
big/close to read (owner, 2026-11-01). Fix: HUD on its own panel (`open_tasks.md`).

### 2D map backgrounds sit at screen depth in stereo (window mode only)
With passthrough and the cut-out (default) there is no backdrop in fields and
battles. With `gXrDiorama` 0 or no passthrough, the sky is screen-space texrects at
zero parallax (owner noticed 2026-10-25).

### Touch controllers don't control the game in the phone (2D) build
Only the Quest build (`-Ppaperboat.quest=true`) reads them, through OpenXR. In the
2D panel build they are laser pointers. Horizon OS's own Touch "gamepads" (vendor
`0x2833`, GUID `05007e98416e64726f69642000000300`) aren't recognized by SDL as
gamepads, and this libultraship feeds every recognized pad to port 1, so they
don't take the port as they did in BattleShip.

### Phone APK touch overlay / Mods button are off in the Quest build
Intended (nothing on the Android surface is visible in immersive mode). Not yet
checked whether the engine's ImGui menu can still be opened in VR (on phones it
has an on-screen toggle); the virtual pad has no Back/Select button mapped.

### Leftover diagnostic files on the headset
`files/logs/td_*.rgba` (~5 MB, ~800 files) from the 2026-10-25 texture dump. adb
can't delete app-owned files; clearing app data or uninstalling removes them (but
also removes the extracted `pm64.o2r`). Harmless.

## Latent risks (seen during investigation, not shown to cause a bug yet)

- **Pointer-keyed resource cache.** libultraship's `Interpreter::mDrawTextureCache`
  maps a path *pointer* to a resolved resource. If a path string is freed and another
  is allocated at the same address (Android's Scudo reuses chunks eagerly), the cache
  would return the old resource. A fix (store the path text with the entry, compare on
  hit) was written and reverted because it wasn't the cause of the texture bug.
- **Other TBI-unaware pointer checks.** Android arm64 heap pointers carry a tag in the
  top byte (`0xB4…`). Any code that range-checks, masks or compares pointer values
  numerically can misbehave only on Android. `GameEngine_OTRSigCheck` (`< 0x10000`
  only) is fine; search for other `> 0x0000FFFFFFFFFFFF`-style checks when something
  is Android-only broken.
- **Uninitialized framebuffers.** `GfxRenderingAPIOGL::UpdateFramebufferParameters`
  allocates color/depth storage with no data and doesn't clear it. Desktop drivers
  usually zero it; Adreno returns stale memory. Not observed to matter yet.
- **Plain `char` is unsigned on Android arm64** (signed on x86, wasm and Apple
  arm64). Not shown to matter; `-fsigned-char` was tried and reverted.

- **Enemy HP bars in battle sit on the steady UI panel** at their game-screen
  positions, not over the enemies (`btl_draw_enemy_health_bars` uses
  `get_screen_coords`). Not reported by the owner yet.
- **End-of-battle star-point banner slides through the room** (3D entities that
  the TV frame normally hides). Being fixed (open_tasks).

## Resolved

### 2026-10-25 — Garbled textures on Android (map palettes, sprites)
**Symptom:** map textures had the right shapes but garbage colors; character sprites
and some surfaces were colored noise. Curtains, logos and other statically named
textures were fine. Extracted data (`pm64.o2r`) was correct.
**Root cause:** `gfx_step` in libultraship (`src/fast/interpreter.cpp`) drops OTR
filepath commands whose pointer is above `0x0000FFFFFFFFFFFF` ("kernel/sentinel
addresses"). Android's allocator tags every heap pointer in the top byte, so every
heap-allocated `__OTR__` path was silently skipped: the map textures' `<name>_tlut`
palette paths (malloc'd by `port_tex_named_level`, `src/port/patches/MapTexturePatches.cpp`)
— so the raster's own pixels were loaded as its palette — and the sprite raster
paths (`SpriteImagePath`, heap strings) — so sprites drew with the previous image.
String literals (static memory, untagged) worked.
**Fix:** RookXR/libultraship `quest-xr` commit `3e06f3cc` — mask the top byte on
`__aarch64__` before the range check.
**How it was found:** dumped every texture uploaded by `UploadBaseTexture` to the
app's logs folder (showed CPU-side data was already wrong), then traced
`GfxDpLoadTlut` (showed palette loads carrying the raster's resource), then traced
the OTR filepath handler (showed `_tlut` commands never reached it).

### 2026-11-01 — Battle menu option names missing in the room view
**Symptom:** with the UI in its own framebuffer, battle menu lists showed their
descriptions but not the option names (still selectable).
**Root cause:** Fast3D creates off-screen framebuffers with `invertY` on (geometry
rendered upside down), but `AdjustVIewportOrScissor` computes an active
framebuffer's scissor with the non-inverted formula (`orig_height - y`), so every
scissor landed mirrored vertically. The lists draw their text inside tight
scissors (`OTRGetScissorCoordX`), so it was clipped away.
**Fix:** `KeepHudUpright` sets the UI framebuffers to non-inverted every pair (same
size → only a rebind); the copy needs no flip any more. Upstream libultraship has
the same mismatch for any game that scissors inside a `G_SETFB` framebuffer.

### 2026-11-01 — All UI invisible after adding the third layer
**Root cause:** `int fb[kHudKinds] = { -1, -1 }` with `kHudKinds` 3 left the third id
0; the "already created" check looked at the last slot, so no framebuffer was ever
created and the UI drew into invalid ids. **Fix:** all slots -1, check all of them.
Found with the `UI coverage` diagnostic.

