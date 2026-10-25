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
 * focus). Stereo shifts that matrix per eye at draw time (perspective
 * matrices only; ortho ones are left alone). */
void port_xr_note_camera(const void* mtx, const float eye[3], const float at[3]);

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

#else

static inline void port_xr_note_camera(const void* mtx, const float eye[3], const float at[3]) {
    (void) mtx;
    (void) eye;
    (void) at;
}
static inline void port_xr_set_present_rate(int fps) { (void) fps; }
static inline int port_xr_paces_frames(void) { return 0; }
static inline int port_xr_stereo_active(void) { return 0; }
static inline void port_xr_set_eye(int eye) { (void) eye; }

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
