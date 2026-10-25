# Known issues

## Open

### 2D map backgrounds sit at screen depth in stereo
The sky/background image is drawn as screen-space texture rectangles, so both eyes
see it at zero parallax, while 3D scenery behind the convergence plane appears
behind the screen — in front of the background in depth but drawn over by
nothing. Owner noticed it (2026-10-25). Fix planned in `open_tasks.md` (Phase 3b).

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
