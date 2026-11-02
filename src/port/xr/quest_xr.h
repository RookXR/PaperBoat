#ifndef PORT_XR_QUEST_XR_H
#define PORT_XR_QUEST_XR_H

/* Frame pacing and stereo hand-off between the render loop and the Quest
 * OpenXR layer (src/port/xr/quest_xr.cpp). Other builds get the inline no-op
 * stubs below, so callers need no #ifdefs. */

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__ANDROID__) && defined(PAPERBOAT_QUEST_XR)

/* Called by the camera render (cam_main.c) each time it writes a camera's
 * projection-slot matrix: `mtx` is the Mtx it wrote, `eye` and `at` the
 * camera's world-space eye and look-at points (their distance is the stereo
 * focus), `vpScale`/`vpTrans` its viewport (Vp_t, quarter pixels). Stereo
 * replaces that matrix per eye at draw time (perspective matrices only; ortho
 * ones are left alone). */
void port_xr_note_camera(const void* mtx, const float eye[3], const float at[3], const short vpScale[4],
                         const short vpTrans[4], int camID);

/* Where the next present lies between the previous game tick and the current
 * one (1 = the tick itself; interpolated presents are below 1). */
void port_xr_set_present_fraction(float fraction);

/* The render loop's present rate (presents per second), set every frame. */
void port_xr_set_present_rate(int fps);

/* Nonzero while the headset's display clock paces presents: the display
 * refresh rate is a whole multiple of the present rate, and each present
 * blocks in xrWaitFrame until the refresh it is due on. libultraship's frame
 * limiter must not sleep as well. */
int port_xr_paces_frames(void);

/* Nonzero while the scene is rendered once per eye: XR is presenting, the
 * session is running and the gXrStereo CVar is on (the default). */
int port_xr_stereo_active(void);

/* The eye the next frame is rendered for: 0 = left, 1 = right, -1 = both
 * (flat). The left eye's swap is held until the right eye's, which submits
 * the pair; presents made with -1 show one image to both eyes. */
void port_xr_set_eye(int eye);

/* Called by the game thread while it builds a frame's background display
 * list (background_gfx.c): first with candidate 0, then again with
 * candidate 1 if the scene is about to draw its usual backdrop. Nonzero =
 * draw no backdrop: the room shows wherever the scene draws nothing
 * (passthrough running, gXrDiorama on). The last answer is remembered for
 * `list` (the display context's background list). */
int port_xr_background_cutout(const void* list, int candidate);

/* Called on the render thread before a frame is drawn, with the background
 * list it starts with (NULL if none): the frame is presented as a cut-out
 * if that list was built without its backdrop. */
void port_xr_frame_background(const void* list);

/* UI in the room view. While the game thread builds a frame's main display
 * list (main_loop.c gfx_draw_frame), screen-space UI passes are bracketed by
 * two placeholder no-op commands: port_xr_hud_frame_start(list, camMtx)
 * first (drops that list's brackets from its previous frame; `camMtx` is the
 * frame camera's projection-slot matrix, the one noted with
 * port_xr_note_camera, which the UI is laid out against), then one
 * port_xr_note_hud(list, begin, end, kind) per bracket pair, `begin` and
 * `end` being the two no-ops. For room-view pairs the XR layer turns them
 * into a switch to one of its UI framebuffers and back; otherwise they stay
 * no-ops. Kinds: */
#define PORT_XR_HUD_PANEL 0 /* fixed UI: a steady panel above the scene */
#define PORT_XR_HUD_PLANE 1 /* messages: the game camera's picture plane, over the speaker */
#define PORT_XR_HUD_PARTY 2 /* battle menus: at the stage's front left (needs an anchor) */
#define PORT_XR_HUD_STAGE 3 /* battle star points: at the stage's front right, the menus' mirror (needs an anchor) */
void port_xr_hud_frame_start(const void* list, const void* camMtx);
void port_xr_note_hud(const void* list, void* begin, void* end, int kind);
/* Places a layer by a world point: `world` (frame camera's world space) and
 * where the game shows it on screen (native px); the layer keeps its screen
 * layout around that point at a constant scale. Needed for PORT_XR_HUD_PARTY
 * and PORT_XR_HUD_STAGE. */
void port_xr_note_hud_anchor(const void* list, int kind, const float world[3], int screenX, int screenY);

/* World-anchored UI elements (the battle target hand). The game places them
 * on screen from a world point through a camera (get_screen_coords); in the
 * room view the XR layer draws them in the scene instead, moved per eye to
 * where that eye sees the point. port_xr_anchor_hud_element(elem, camID, x,
 * y, z, screenX, screenY) records the point, the camera and the screen
 * position the game derived from it (game thread; it holds until two frames
 * pass without a new call). port_xr_hud_element_camera(elem) is the anchored
 * camera id, or -1: the element's draw is then bracketed by two no-ops and
 * reported with port_xr_note_world_sprite (`mtx`: that camera's
 * projection-slot matrix in the display context being built). */
void port_xr_anchor_hud_element(const void* elem, int camID, float x, float y, float z, int screenX, int screenY);
/* The same for a sprite that stands on the stage facing its front (enemy HP
 * bars) instead of turning toward the viewer. */
void port_xr_anchor_stage_sprite(const void* elem, int camID, float x, float y, float z, int screenX, int screenY);
int port_xr_hud_element_camera(const void* elem);
/* A screen transition (fade, wipe) between two no-ops, covering `coverage`
 * (0..1) of the screen: in the room view it is skipped and the scene fades
 * into the room by that much instead. */
void port_xr_note_scene_fade(const void* list, void* begin, void* end, float coverage);
/* 3D drawn inside a UI pass (HUD-camera models such as the star-point banner)
 * between two no-ops: in the room view it goes back into the scene (the eye
 * images, room matrices) instead of the UI layer around it. */
void port_xr_note_scene_range(const void* list, void* begin, void* end);
void port_xr_note_world_sprite(const void* list, void* begin, void* end, const void* elem, const void* mtx);

/* Called on the render thread before a frame is drawn, with its main list. */
void port_xr_frame_main(const void* list);

/* Nonzero while frames are presented as a room view (as of the latest stereo
 * pair); readable from the game thread. */
int port_xr_room_view_active(void);


#else

static inline void port_xr_note_camera(const void* mtx, const float eye[3], const float at[3],
                                       const short vpScale[4], const short vpTrans[4], int camID) {
    (void) camID;
    (void) mtx;
    (void) eye;
    (void) at;
    (void) vpScale;
    (void) vpTrans;
}
static inline void port_xr_set_present_rate(int fps) { (void) fps; }
static inline void port_xr_set_present_fraction(float fraction) { (void) fraction; }
static inline int port_xr_paces_frames(void) { return 0; }
static inline int port_xr_stereo_active(void) { return 0; }
static inline void port_xr_set_eye(int eye) { (void) eye; }
static inline int port_xr_background_cutout(const void* list, int candidate) {
    (void) list;
    (void) candidate;
    return 0;
}
static inline void port_xr_frame_background(const void* list) { (void) list; }
static inline void port_xr_hud_frame_start(const void* list, const void* camMtx) {
    (void) list;
    (void) camMtx;
}
#define PORT_XR_HUD_PANEL 0
#define PORT_XR_HUD_PLANE 1
#define PORT_XR_HUD_PARTY 2
#define PORT_XR_HUD_STAGE 3
static inline void port_xr_note_hud_anchor(const void* list, int kind, const float world[3], int screenX,
                                           int screenY) {
    (void) list;
    (void) kind;
    (void) world;
    (void) screenX;
    (void) screenY;
}
static inline void port_xr_note_hud(const void* list, void* begin, void* end, int kind) {
    (void) list;
    (void) begin;
    (void) end;
    (void) kind;
}
static inline void port_xr_anchor_hud_element(const void* elem, int camID, float x, float y, float z, int screenX,
                                              int screenY) {
    (void) elem;
    (void) camID;
    (void) x;
    (void) y;
    (void) z;
    (void) screenX;
    (void) screenY;
}
static inline void port_xr_anchor_stage_sprite(const void* elem, int camID, float x, float y, float z, int screenX,
                                               int screenY) {
    port_xr_anchor_hud_element(elem, camID, x, y, z, screenX, screenY);
}
static inline int port_xr_hud_element_camera(const void* elem) {
    (void) elem;
    return -1;
}
static inline void port_xr_note_scene_range(const void* list, void* begin, void* end) {
    (void) list;
    (void) begin;
    (void) end;
}
static inline void port_xr_note_scene_fade(const void* list, void* begin, void* end, float coverage) {
    (void) list;
    (void) begin;
    (void) end;
    (void) coverage;
}
static inline void port_xr_note_world_sprite(const void* list, void* begin, void* end, const void* elem,
                                             const void* mtx) {
    (void) list;
    (void) begin;
    (void) end;
    (void) elem;
    (void) mtx;
}
static inline void port_xr_frame_main(const void* list) { (void) list; }
static inline int port_xr_room_view_active(void) { return 0; }
#endif

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <unordered_map>

#include <fast/types.h>

#if defined(__ANDROID__) && defined(PAPERBOAT_QUEST_XR)
/* The matrix replacements for one eye's draw: `base` (this present's
 * interpolated matrices) plus that eye's copy of every noted perspective
 * camera matrix, shifted from its interpolated value if `base` has one, else
 * from game memory. eye: 0 = left, 1 = right. Valid until the next call. */
const std::unordered_map<Mtx*, MtxF>& port_xr_eye_replacements(const std::unordered_map<Mtx*, MtxF>& base, int eye);
#else
static inline const std::unordered_map<Mtx*, MtxF>& port_xr_eye_replacements(const std::unordered_map<Mtx*, MtxF>& base,
                                                                            int eye) {
    (void) eye;
    return base;
}
#endif
#endif

#endif /* PORT_XR_QUEST_XR_H */
