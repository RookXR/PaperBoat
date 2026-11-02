// quest_xr.cpp — OpenXR presentation, stereo and Touch controller input for the
// Meta Quest build.
//
// Only compiled into the Quest APK (gradle -Ppaperboat.quest=true → CMake
// PAPERBOAT_QUEST_XR=ON); an empty TU elsewhere. Ported from BattleShip's
// port/xr/quest_xr.cpp (Phases 2, 3.0 and 3a), see docs/quest_vr/.
//
// Display:
//   The game keeps rendering exactly as on a phone: Fast3D draws into SDL's
//   GLES default framebuffer, then libultraship calls SDL_GL_SwapWindow. The
//   Quest link step passes -Wl,--wrap=SDL_GL_SwapWindow, so that call lands
//   in __wrap_SDL_GL_SwapWindow below. There we blit the finished back buffer
//   into an OpenXR swapchain image and submit it as a quad layer — a "cinema
//   screen" floating in front of the player. The real swap is skipped while XR
//   is active: in an immersive activity the Android surface is never
//   composited, and eglSwapBuffers on it would only add a stall.
//
// Stereo (port_xr_stereo_active): the render loop (GameEngine::RunCommands)
//   draws each present twice, once per eye, with that eye's camera matrices
//   (port_xr_eye_replacements), and says which eye each swap is for
//   (port_xr_set_eye). The left eye's swap only stashes the image; the right
//   eye's swap copies both into the two halves of one swapchain image and
//   submits one quad layer per eye at the same screen pose. Flat presents
//   (eye -1) use the left half for both eyes.
//
// Passthrough (gXrPassthrough, default on): when the runtime offers
//   XR_FB_passthrough, a passthrough layer is submitted under the screen, so
//   the room is visible around it.
//
// Diorama cut-out (gXrDiorama, default on, needs passthrough): field and
//   battle scenes draw no backdrop (background_gfx.c asks
//   port_xr_background_cutout), so the game framebuffer stays at alpha 0
//   wherever the scene draws nothing (libultraship's LUS_GL_GAME_FB_ALPHA,
//   set for the Quest build: RGBA8 game framebuffer cleared to alpha 0).
//   The eye images are copied from the game framebuffer's texture with their
//   alpha, and the quads blend over the room. Frames with a backdrop (menus,
//   pause, title) stay opaque.
//
// UI in the room view (gXrHud, default on): the screen-space UI would sit
//   head-locked at the edge of each eye's widened image. gfx_draw_frame
//   brackets it (port_xr_note_hud); for room-view pairs it draws into our
//   own Fast3D framebuffers instead: fixed UI on a steady panel above the
//   scene, battle menus at the stage's front left, speech bubbles on the game camera's
//   picture plane (over the speaker), and the battle target hand moved and
//   scaled per eye onto its enemy.
//
// Input:
//   Touch controllers are read through OpenXR actions (the system does not
//   deliver them to the app as gamepads in immersive mode) and pushed into an
//   SDL virtual Xbox-layout gamepad, so libultraship's default SDL→N64
//   mapping applies unchanged:
//     left stick → control stick       right stick → C buttons
//     A → A        B → B               X → C-down (partner ability)
//     Y → L        left menu → Start
//     left trigger / left grip   → Z (spin dash)
//     right trigger / right grip → R
//
// Threading: everything but port_xr_note_camera runs on the SDL_main/render
// thread, inside the swap or the render loop. Failure at any init step logs,
// disables XR for the process, and falls back to the ordinary SDL swap.

#if defined(__ANDROID__) && defined(PAPERBOAT_QUEST_XR)

#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>

#include <fast/Fast3dWindow.h>
#include <fast/interpreter.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "quest_xr.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define LOG_TAG "paperboat.xr"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" void __real_SDL_GL_SwapWindow(SDL_Window* window);

namespace {

// Screen placement in LOCAL space (origin = head pose at session start /
// last recenter). 0.9 m tall at 1.3 m: the same angular size as BattleShip's
// tuned default (a 16:9 screen is 1.6 m wide, ~63° — a big TV).
constexpr float kScreenDistance = 1.3f;
constexpr float kScreenHeight = 0.9f;

// The game renders at a fixed 16:9 size instead of the immersive activity's
// surface, which spans both eye panels (4128x2208 on Quest 3, ~1.87:1): that
// is the wrong aspect and ~4x the pixels the screen can resolve. PaperBoat's
// widescreen follows the window aspect, so this also picks 16:9 for the game.
constexpr int kRenderWidth = 1920;
constexpr int kRenderHeight = 1080;

// One UI layer in the HUD swapchain: the 16:9 screen (native 426.7x240) at 3x.
constexpr int kHudImageW = 1280;
constexpr int kHudImageH = 720;
// World sprites (target hand, enemy HP bars), each shown on its own small quad
// in the room.
// The fifth UI framebuffer (after PORT_XR_HUD_PANEL/PLANE/PARTY/STAGE).
constexpr int kHudSpriteKind = 4;
constexpr int kMaxSpriteQuads = 10; // the compositor takes 16 layers: passthrough, scene, 4 UI, sprites

constexpr XrDuration kStatsPeriod = 5000000000; // 5 s

// Longest stall, in presents, that pacing makes up for (see SubmitFrame).
constexpr int kMaxCatchUpPresents = 3;

// Presents between stereo self-checks (StereoCheck), ~10 s at 30 fps.
constexpr int kStereoCheckPresents = 300;

constexpr GLenum kFramebufferSrgbExt = 0x8DB9; // GL_FRAMEBUFFER_SRGB_EXT

// Stereo tunables, read live (edit the CVars in paperboat.cfg.json over adb).
// Separation is the distance between the eyes as a fraction of the game
// camera's eye->lookAt distance; convergence is the zero-parallax distance as
// a multiple of it. Below 1, Mario (the camera's usual look-at point) sits
// slightly behind the screen plane, where the HUD is.
constexpr const char* kStereoCVar = "gXrStereo";
constexpr const char* kStereoSeparationCVar = "gXrStereoSeparation";
constexpr const char* kStereoConvergenceCVar = "gXrStereoConvergence";
constexpr const char* kStereoSwapEyesCVar = "gXrStereoSwapEyes"; // debug: check the depth sign
constexpr float kDefaultSeparation = 0.01f;
constexpr float kDefaultConvergence = 0.85f;

// Largest distance on the screen between the two images of a point at
// infinity. Past the viewer's IPD (~6.3 cm) the eyes would have to diverge,
// so separation is capped to stay well under it. Scaled with the screen from
// BattleShip's 5 cm on a 3.2 m screen.
constexpr float kMaxInfinityDisparity = 0.025f; // m

// Room and cut-out switches, read live.
constexpr const char* kPassthroughCVar = "gXrPassthrough"; // room visible around the screen
constexpr const char* kDioramaCVar = "gXrDiorama";         // scenes float in the room (needs passthrough)
constexpr const char* kAlphaViewCVar = "gXrAlphaView";     // debug: show the game's alpha as grey

enum class Mode { Untried, Active, Disabled };

struct PassthroughFns {
    PFN_xrCreatePassthroughFB create = nullptr;
    PFN_xrDestroyPassthroughFB destroy = nullptr;
    PFN_xrPassthroughStartFB start = nullptr;
    PFN_xrPassthroughPauseFB pause = nullptr;
    PFN_xrCreatePassthroughLayerFB createLayer = nullptr;
    PFN_xrDestroyPassthroughLayerFB destroyLayer = nullptr;
    PFN_xrPassthroughLayerResumeFB resumeLayer = nullptr;
    PFN_xrPassthroughLayerPauseFB pauseLayer = nullptr;
};

struct InputActions {
    XrActionSet set = XR_NULL_HANDLE;
    XrAction leftStick = XR_NULL_HANDLE, rightStick = XR_NULL_HANDLE;
    XrAction leftTrigger = XR_NULL_HANDLE, rightTrigger = XR_NULL_HANDLE;
    XrAction leftGrip = XR_NULL_HANDLE, rightGrip = XR_NULL_HANDLE;
    XrAction a = XR_NULL_HANDLE, b = XR_NULL_HANDLE, x = XR_NULL_HANDLE, y = XR_NULL_HANDLE;
    XrAction menu = XR_NULL_HANDLE;
};

struct CameraNote {
    float eye[3];
    float at[3];
    // The same camera's previous game tick (interpolated presents lie between).
    bool hasPrev = false;
    float prevEye[3] = {};
    float prevAt[3] = {};
    // The camera's viewport in native 320x240 pixels (left, top, right, bottom).
    float viewport[4];
};

// Room view (see RoomEyeMatrix), read live.
constexpr const char* kRoomViewCVar = "gXrRoomView";             // scenes as objects in the room
constexpr const char* kRoomDistanceCVar = "gXrRoomDistance";     // m ahead of the LOCAL origin
constexpr const char* kRoomHeightCVar = "gXrRoomHeight";         // m above the LOCAL origin (eye level)
constexpr const char* kRoomUnitsPerMeterCVar = "gXrRoomUnitsPerMeter"; // game units per meter (model scale)
constexpr float kDefaultRoomDistance = 1.0f;
constexpr float kDefaultRoomHeight = -0.35f;
constexpr float kDefaultRoomUnitsPerMeter = 300.0f;

struct QuestXr {
    Mode mode = Mode::Untried;
    jobject activity = nullptr; // global ref, held for the process lifetime

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    bool refreshRateExt = false;
    bool refreshRateSettled = false; // rates enumerated and a choice made

    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    int32_t width = 0, height = 0;    // one eye's image; the swapchain holds two side by side
    int drawableW = 0, drawableH = 0; // last logged SDL drawable size
    GLuint blitFbo = 0;
    bool srgbWriteControl = false;
    bool hasImage = false;    // a swapchain image has been released and can be re-shown
    bool imageStereo = false; // ...and its halves are a left/right pair, not one flat image
    bool imageCutout = false; // ...and it was drawn without a backdrop (alpha = coverage)
    bool imageHud = false;    // ...and the HUD swapchain holds its UI (HUD panel)

    // Copy from the game framebuffer's texture, keeping its alpha (CopyGameEye).
    GLuint copyProgram = 0, copyVao = 0;
    GLint copyAlphaViewLoc = -1;
    GLint copyOpacityLoc = -1;
    // The scene image's opacity for the pair being drawn: below 1 during a
    // screen transition in the room view (port_xr_note_scene_fade).
    float sceneOpacity = 1.0f;
    // Where the present being drawn lies between the last two game ticks
    // (port_xr_set_present_fraction; 1 = the tick itself).
    float presentFraction = 1.0f;
    GLuint loggedCopyTexture = 0;

    // Passthrough (XR_FB_passthrough): created once when first wanted, then
    // paused/resumed to follow gXrPassthrough. Any error turns it off for
    // the rest of the process.
    bool passthroughExt = false;
    bool passthroughRunning = false;
    bool passthroughFailed = false;
    XrPassthroughFB passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthroughLayer = XR_NULL_HANDLE;

    // The frame being drawn has no backdrop (port_xr_frame_background).
    bool cutoutDraw = false;

    // Room view: the pair being drawn uses the eye poses and fields of view
    // in pairViews (fov already widened, see RoomFov); the released image was
    // drawn with imageViews and is submitted as a projection layer.
    bool pairRoom = false;
    bool loggedRoom = false;
    XrView pairViews[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    bool imageRoom = false;
    XrView imageViews[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    XrTime lastDisplayTime = 0; // predicted display time of the latest XR frame
    int roomCheckCountdown = 0;

    // Stereo: the eye the render loop is drawing (-1 = flat), and the left
    // eye's image, held until the right eye's swap submits both.
    int eye = -1;
    GLuint stashFbo = 0, stashTex = 0;
    bool stashValid = false;
    std::unordered_map<Mtx*, MtxF> eyeReplacements;
    int stereoCheckCountdown = 0;

    // Pacing (see port_xr_paces_frames).
    int presentRate = 30;         // render loop presents per second
    XrDuration displayPeriod = 0; // from the latest xrWaitFrame
    XrTime nextPresentTime = 0;   // display time the next present is due; 0 = unscheduled
    int loggedRefreshes = -1;     // last refreshes-per-present value logged

    // Pacing stats, logged every kStatsPeriod of display time.
    XrTime statsStart = 0;
    int statsPresents = 0, statsRepeats = 0, statsResyncs = 0;

    InputActions input;
    SDL_Joystick* pad = nullptr;
};

QuestXr gXr;

// Camera matrices noted by the game thread (port_xr_note_camera), keyed by
// the Mtx they were written to. A handful of entries: each display context
// has one per camera.
std::mutex gCameraMutex;
std::unordered_map<const void*, CameraNote> gCameras;

// Whether each background display list (one per display context) was built
// without its backdrop, noted by the game thread (port_xr_background_cutout)
// and looked up when the frame is drawn (port_xr_frame_background).
std::mutex gBackgroundMutex;
std::unordered_map<const void*, bool> gBackgroundCutout;

// Read by the game thread while it builds a frame.
std::atomic<bool> gCutoutAvailable{ false };
std::atomic<bool> gRoomViewActive{ false }; // the latest stereo pair was a room view

bool Ok(XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    char name[XR_MAX_RESULT_STRING_SIZE] = {};
    if (gXr.instance != XR_NULL_HANDLE) {
        xrResultToString(gXr.instance, result, name);
    }
    LOGE("%s failed: %s (%d)", what, name[0] ? name : "?", (int) result);
    return false;
}

bool HasInstanceExtension(const std::vector<XrExtensionProperties>& available, const char* name) {
    return std::any_of(available.begin(), available.end(),
                       [&](const XrExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

bool HasGlExtension(const char* name) {
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
        if (ext != nullptr && std::strcmp(ext, name) == 0) {
            return true;
        }
    }
    return false;
}

template <typename Fn> bool InstanceProc(const char* name, Fn& fn) {
    return Ok(xrGetInstanceProcAddr(gXr.instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn)), name);
}

// ---------------------------------------------------------------------------
// Instance / session
// ---------------------------------------------------------------------------

bool CreateInstance() {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    auto localActivity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env == nullptr || localActivity == nullptr) {
        LOGE("SDL Android activity unavailable");
        return false;
    }
    gXr.activity = env->NewGlobalRef(localActivity);
    env->DeleteLocalRef(localActivity);
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);

    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    if (!Ok(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                  reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader)),
            "xrGetInstanceProcAddr(xrInitializeLoaderKHR)")) {
        return false;
    }
    XrLoaderInitInfoAndroidKHR loaderInfo{ XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
    loaderInfo.applicationVM = vm;
    loaderInfo.applicationContext = gXr.activity;
    if (!Ok(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo)),
            "xrInitializeLoaderKHR")) {
        return false;
    }

    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> available(extCount, { XR_TYPE_EXTENSION_PROPERTIES });
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, available.data());

    std::vector<const char*> extensions = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                            XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };
    for (const char* required : extensions) {
        if (!HasInstanceExtension(available, required)) {
            LOGE("Runtime lacks required extension %s", required);
            return false;
        }
    }
    gXr.refreshRateExt = HasInstanceExtension(available, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (gXr.refreshRateExt) {
        extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
    gXr.passthroughExt = HasInstanceExtension(available, XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (gXr.passthroughExt) {
        extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    } else {
        LOGI("Runtime lacks %s; no passthrough", XR_FB_PASSTHROUGH_EXTENSION_NAME);
    }

    XrInstanceCreateInfoAndroidKHR androidInfo{ XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    androidInfo.applicationVM = vm;
    androidInfo.applicationActivity = gXr.activity;
    XrInstanceCreateInfo info{ XR_TYPE_INSTANCE_CREATE_INFO };
    info.next = &androidInfo;
    std::strncpy(info.applicationInfo.applicationName, "PaperBoat", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(info.applicationInfo.engineName, "libultraship", XR_MAX_ENGINE_NAME_SIZE - 1);
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = (uint32_t) extensions.size();
    info.enabledExtensionNames = extensions.data();
    if (!Ok(xrCreateInstance(&info, &gXr.instance), "xrCreateInstance")) {
        return false;
    }

    XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    return Ok(xrGetSystem(gXr.instance, &systemInfo, &gXr.system), "xrGetSystem");
}

bool CreateSession() {
    // The spec requires querying graphics requirements before xrCreateSession.
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
    if (!InstanceProc("xrGetOpenGLESGraphicsRequirementsKHR", getRequirements)) {
        return false;
    }
    XrGraphicsRequirementsOpenGLESKHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
    if (!Ok(getRequirements(gXr.instance, gXr.system, &requirements), "xrGetOpenGLESGraphicsRequirementsKHR")) {
        return false;
    }
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    const XrVersion glVersion = XR_MAKE_VERSION(major, minor, 0);
    if (glVersion < requirements.minApiVersionSupported) {
        LOGE("GLES %d.%d below runtime minimum", major, minor);
        return false;
    }

    // Share SDL's own context with the runtime so swapchain images are
    // directly usable by the GL calls libultraship already makes.
    XrGraphicsBindingOpenGLESAndroidKHR binding{ XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
    binding.display = eglGetCurrentDisplay();
    binding.context = eglGetCurrentContext();
    if (binding.display == EGL_NO_DISPLAY || binding.context == EGL_NO_CONTEXT) {
        LOGE("No current EGL context on the render thread");
        return false;
    }
    EGLint configId = 0;
    eglQueryContext(binding.display, binding.context, EGL_CONFIG_ID, &configId);
    const EGLint configAttribs[] = { EGL_CONFIG_ID, configId, EGL_NONE };
    EGLint configCount = 0;
    if (!eglChooseConfig(binding.display, configAttribs, &binding.config, 1, &configCount) || configCount != 1) {
        LOGE("Cannot resolve SDL's EGL config (id %d)", configId);
        return false;
    }

    XrSessionCreateInfo info{ XR_TYPE_SESSION_CREATE_INFO };
    info.next = &binding;
    info.systemId = gXr.system;
    if (!Ok(xrCreateSession(gXr.instance, &info, &gXr.session), "xrCreateSession")) {
        return false;
    }

    XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    return Ok(xrCreateReferenceSpace(gXr.session, &spaceInfo, &gXr.local), "xrCreateReferenceSpace");
}

// 30 and 60 fps presents divide evenly into 120 Hz, so each one is held for a
// whole number of display refreshes instead of juddering against 72/90 Hz.
// Called on READY and again on FOCUSED until the runtime answers: right after
// xrBeginSession the Quest runtime has been seen to report no rates at all.
void RequestRefreshRate() {
    if (!gXr.refreshRateExt || gXr.refreshRateSettled) {
        return;
    }
    PFN_xrEnumerateDisplayRefreshRatesFB enumerateRates = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestRate = nullptr;
    if (!InstanceProc("xrEnumerateDisplayRefreshRatesFB", enumerateRates) ||
        !InstanceProc("xrRequestDisplayRefreshRateFB", requestRate)) {
        return;
    }
    uint32_t count = 0;
    if (!Ok(enumerateRates(gXr.session, 0, &count, nullptr), "xrEnumerateDisplayRefreshRatesFB")) {
        return;
    }
    if (count == 0) {
        LOGI("No display refresh rates reported yet (session state %d)", (int) gXr.state);
        return;
    }
    std::vector<float> rates(count);
    if (!Ok(enumerateRates(gXr.session, count, &count, rates.data()), "xrEnumerateDisplayRefreshRatesFB")) {
        return;
    }
    gXr.refreshRateSettled = true;
    std::string offered;
    for (float rate : rates) {
        offered += std::to_string(rate) + " ";
    }
    LOGI("Display refresh rates offered: %s", offered.c_str());
    // gXrFps 90 runs the display at 90 Hz (one present per refresh), the other
    // rates at 120 Hz. Runtimes report rates such as 119.99: compare with a tolerance.
    const float wanted = CVarGetInteger("gXrFps", 90) == 90 ? 90.0f : 120.0f;
    for (float rate : rates) {
        if (std::fabs(rate - wanted) < 0.5f) {
            if (Ok(requestRate(gXr.session, rate), "xrRequestDisplayRefreshRateFB")) {
                LOGI("Display refresh rate %.2f Hz requested", rate);
            }
            return;
        }
    }
    LOGI("%.0f Hz unavailable; keeping runtime default refresh rate", wanted);
}

// Ask MainActivity to pin SDL's surface to the render size. The surface change
// reaches SDL as a window resize, and libultraship sizes the game viewport
// from that. JNI is safe here: this runs on the SDL_main thread.
void RequestRenderSize(int width, int height) {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = env->GetObjectClass(gXr.activity);
    jmethodID method = env->GetMethodID(cls, "setXrRenderSize", "(II)V");
    if (method == nullptr) {
        env->ExceptionClear();
        LOGE("MainActivity.setXrRenderSize missing; keeping the surface size");
    } else {
        env->CallVoidMethod(gXr.activity, method, width, height);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGE("MainActivity.setXrRenderSize threw; keeping the surface size");
        }
    }
    env->DeleteLocalRef(cls);
}

bool CreateSwapchain() {
    const int width = kRenderWidth;
    const int height = kRenderHeight;

    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(gXr.session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(gXr.session, formatCount, &formatCount, formats.data());
    auto supported = [&](GLenum f) { return std::find(formats.begin(), formats.end(), (int64_t) f) != formats.end(); };

    // Fast3D's output is already display-encoded. An sRGB swapchain tells the
    // compositor that; we then copy the bytes verbatim by turning off sRGB
    // encoding for the blit. Without write control, use a plain RGBA8 chain.
    gXr.srgbWriteControl = HasGlExtension("GL_EXT_sRGB_write_control");
    int64_t format = 0;
    if (gXr.srgbWriteControl && supported(GL_SRGB8_ALPHA8)) {
        format = GL_SRGB8_ALPHA8;
    } else if (supported(GL_RGBA8)) {
        format = GL_RGBA8;
    } else {
        LOGE("Runtime offers neither SRGB8_ALPHA8 nor RGBA8 swapchains");
        return false;
    }

    XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = (uint32_t) (2 * width);
    info.height = (uint32_t) height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!Ok(xrCreateSwapchain(gXr.session, &info, &gXr.swapchain), "xrCreateSwapchain")) {
        return false;
    }
    gXr.width = width;
    gXr.height = height;

    uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(gXr.swapchain, 0, &imageCount, nullptr);
    gXr.images.assign(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR });
    if (!Ok(xrEnumerateSwapchainImages(gXr.swapchain, imageCount, &imageCount,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(gXr.images.data())),
            "xrEnumerateSwapchainImages")) {
        return false;
    }
    glGenFramebuffers(1, &gXr.blitFbo);

    // Plain RGBA8: the stash is copied byte for byte, like the default framebuffer.
    GLint prevTex = 0, prevDraw = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    glGenTextures(1, &gXr.stashTex);
    glBindTexture(GL_TEXTURE_2D, gXr.stashTex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
    glGenFramebuffers(1, &gXr.stashFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gXr.stashFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gXr.stashTex, 0);
    const bool stashComplete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint) prevDraw);
    glBindTexture(GL_TEXTURE_2D, (GLuint) prevTex);
    if (!stashComplete) {
        LOGE("Stereo stash framebuffer incomplete");
        return false;
    }

    LOGI("Swapchain %dx%d (two %dx%d eyes), %u images, format 0x%llx", 2 * width, height, width, height, imageCount,
         (unsigned long long) format);
    RequestRenderSize(width, height);
    return true;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

XrPath Path(const char* str) {
    XrPath path = XR_NULL_PATH;
    xrStringToPath(gXr.instance, str, &path);
    return path;
}

bool CreateAction(XrActionType type, const char* name, const char* localized, XrAction* out) {
    XrActionCreateInfo info{ XR_TYPE_ACTION_CREATE_INFO };
    info.actionType = type;
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    return Ok(xrCreateAction(gXr.input.set, &info, out), name);
}

bool CreateInput() {
    InputActions& in = gXr.input;
    XrActionSetCreateInfo setInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
    std::strncpy(setInfo.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(setInfo.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (!Ok(xrCreateActionSet(gXr.instance, &setInfo, &in.set), "xrCreateActionSet")) {
        return false;
    }

    const bool created =
        CreateAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "left_stick", "Control stick", &in.leftStick) &&
        CreateAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "right_stick", "C buttons", &in.rightStick) &&
        CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "left_trigger", "Z (trigger)", &in.leftTrigger) &&
        CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "right_trigger", "R (trigger)", &in.rightTrigger) &&
        CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "left_grip", "Z (grip)", &in.leftGrip) &&
        CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "right_grip", "R (grip)", &in.rightGrip) &&
        CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "A", &in.a) &&
        CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "B", &in.b) &&
        CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", "Partner ability (C-down)", &in.x) &&
        CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", "L", &in.y) &&
        CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Start", &in.menu);
    if (!created) {
        return false;
    }

    const XrActionSuggestedBinding bindings[] = {
        { in.leftStick, Path("/user/hand/left/input/thumbstick") },
        { in.rightStick, Path("/user/hand/right/input/thumbstick") },
        { in.leftTrigger, Path("/user/hand/left/input/trigger/value") },
        { in.rightTrigger, Path("/user/hand/right/input/trigger/value") },
        { in.leftGrip, Path("/user/hand/left/input/squeeze/value") },
        { in.rightGrip, Path("/user/hand/right/input/squeeze/value") },
        { in.a, Path("/user/hand/right/input/a/click") },
        { in.b, Path("/user/hand/right/input/b/click") },
        { in.x, Path("/user/hand/left/input/x/click") },
        { in.y, Path("/user/hand/left/input/y/click") },
        { in.menu, Path("/user/hand/left/input/menu/click") },
    };
    XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggested.interactionProfile = Path("/interaction_profiles/oculus/touch_controller");
    suggested.suggestedBindings = bindings;
    suggested.countSuggestedBindings = (uint32_t) (sizeof(bindings) / sizeof(bindings[0]));
    if (!Ok(xrSuggestInteractionProfileBindings(gXr.instance, &suggested), "xrSuggestInteractionProfileBindings")) {
        return false;
    }

    XrSessionActionSetsAttachInfo attach{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &in.set;
    return Ok(xrAttachSessionActionSets(gXr.session, &attach), "xrAttachSessionActionSets");
}

// Xbox 360 VID/PID so SDL promotes the device to an SDL_GameController and
// libultraship's default SDL mappings apply. This libultraship feeds every
// recognized gamepad to port 1, so Horizon OS's own Touch gamepads (vendor
// 0x2833, which SDL doesn't recognize as gamepads anyway) can't take the port
// the way they did in BattleShip.
bool AttachPad() {
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.vendor_id = 0x045E;
    desc.product_id = 0x028E;
    desc.name = "Quest Touch Controllers";

    const int index = SDL_JoystickAttachVirtualEx(&desc);
    if (index < 0) {
        LOGE("SDL_JoystickAttachVirtualEx failed: %s", SDL_GetError());
        return false;
    }
    gXr.pad = SDL_JoystickOpen(index);
    if (gXr.pad == nullptr) {
        LOGE("SDL_JoystickOpen(%d) failed: %s", index, SDL_GetError());
        SDL_JoystickDetachVirtual(index);
        return false;
    }
    return true;
}

Sint16 StickAxis(float v) {
    return (Sint16) std::clamp((int) (v * 32767.0f), -32768, 32767);
}

// SDL's Xbox mapping reads raw -32768 as a released trigger, +32767 as full.
Sint16 TriggerAxis(float v) {
    return (Sint16) std::clamp((int) (v * 65535.0f) - 32768, -32768, 32767);
}

void WritePad(float lx, float ly, float rx, float ry, float lt, float rt, bool a, bool b, bool l, bool start) {
    if (gXr.pad == nullptr) {
        return;
    }
    // OpenXR sticks are +Y up; SDL gamepad axes are +Y down.
    SDL_JoystickSetVirtualAxis(gXr.pad, SDL_CONTROLLER_AXIS_LEFTX, StickAxis(lx));
    SDL_JoystickSetVirtualAxis(gXr.pad, SDL_CONTROLLER_AXIS_LEFTY, StickAxis(-ly));
    SDL_JoystickSetVirtualAxis(gXr.pad, SDL_CONTROLLER_AXIS_RIGHTX, StickAxis(rx));
    SDL_JoystickSetVirtualAxis(gXr.pad, SDL_CONTROLLER_AXIS_RIGHTY, StickAxis(-ry));
    SDL_JoystickSetVirtualAxis(gXr.pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, TriggerAxis(lt));
    SDL_JoystickSetVirtualAxis(gXr.pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, TriggerAxis(rt));
    SDL_JoystickSetVirtualButton(gXr.pad, SDL_CONTROLLER_BUTTON_A, a);
    SDL_JoystickSetVirtualButton(gXr.pad, SDL_CONTROLLER_BUTTON_B, b);
    SDL_JoystickSetVirtualButton(gXr.pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, l);
    SDL_JoystickSetVirtualButton(gXr.pad, SDL_CONTROLLER_BUTTON_START, start);
}

void ReleasePad() {
    WritePad(0, 0, 0, 0, 0, 0, false, false, false, false);
}

bool GetBool(XrAction action) {
    XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
    info.action = action;
    XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(xrGetActionStateBoolean(gXr.session, &info, &state)) && state.isActive && state.currentState;
}

float GetFloat(XrAction action) {
    XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
    info.action = action;
    XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
    return XR_SUCCEEDED(xrGetActionStateFloat(gXr.session, &info, &state)) && state.isActive ? state.currentState
                                                                                             : 0.0f;
}

XrVector2f GetVector(XrAction action) {
    XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
    info.action = action;
    XrActionStateVector2f state{ XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_SUCCEEDED(xrGetActionStateVector2f(gXr.session, &info, &state)) && state.isActive) {
        return state.currentState;
    }
    return { 0.0f, 0.0f };
}

void UpdateInput() {
    // Only a FOCUSED session receives input; otherwise (system menu open,
    // headset off) release everything so no button sticks down.
    if (gXr.state != XR_SESSION_STATE_FOCUSED) {
        ReleasePad();
        return;
    }
    const XrActiveActionSet active{ gXr.input.set, XR_NULL_PATH };
    XrActionsSyncInfo sync{ XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (!XR_SUCCEEDED(xrSyncActions(gXr.session, &sync))) {
        ReleasePad();
        return;
    }
    const InputActions& in = gXr.input;
    const XrVector2f left = GetVector(in.leftStick);
    XrVector2f right = GetVector(in.rightStick);
    if (GetBool(in.x)) {
        right.y = -1.0f; // C-down: partner ability
    }
    const float z = std::max(GetFloat(in.leftTrigger), GetFloat(in.leftGrip));
    const float r = std::max(GetFloat(in.rightTrigger), GetFloat(in.rightGrip));
    WritePad(left.x, left.y, right.x, right.y, z, r, GetBool(in.a), GetBool(in.b), GetBool(in.y), GetBool(in.menu));
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

void PollEvents() {
    XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(gXr.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                gXr.state = changed.state;
                LOGI("Session state -> %d", (int) changed.state);
                if (changed.state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{ XR_TYPE_SESSION_BEGIN_INFO };
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (Ok(xrBeginSession(gXr.session, &begin), "xrBeginSession")) {
                        gXr.running = true;
                        RequestRefreshRate();
                    }
                } else if (changed.state == XR_SESSION_STATE_FOCUSED) {
                    RequestRefreshRate();
                } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                    Ok(xrEndSession(gXr.session), "xrEndSession");
                    gXr.running = false;
                    ReleasePad();
                } else if (changed.state == XR_SESSION_STATE_EXITING ||
                           changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                    // User quit from the system menu: shut the game down cleanly.
                    gXr.running = false;
                    ReleasePad();
                    SDL_Event quit;
                    SDL_zero(quit);
                    quit.type = SDL_QUIT;
                    SDL_PushEvent(&quit);
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                LOGE("OpenXR instance loss pending");
                gXr.running = false;
                break;
            default:
                break;
        }
        event = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// Saves the GL state the copies below change and restores it on scope exit,
// so Fast3D's own state tracking stays valid.
class BlitState {
  public:
    BlitState() {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &mRead);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &mDraw);
        mScissor = glIsEnabled(GL_SCISSOR_TEST);
        mSrgbWrite = gXr.srgbWriteControl && glIsEnabled(kFramebufferSrgbExt);
        glDisable(GL_SCISSOR_TEST); // glBlitFramebuffer honours the scissor box
        if (mSrgbWrite) {
            glDisable(kFramebufferSrgbExt);
        }
    }
    ~BlitState() {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) mRead);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint) mDraw);
        if (mScissor) {
            glEnable(GL_SCISSOR_TEST);
        }
        if (mSrgbWrite) {
            glEnable(kFramebufferSrgbExt);
        }
    }

  private:
    GLint mRead = 0, mDraw = 0;
    GLboolean mScissor = GL_FALSE;
    bool mSrgbWrite = false;
};

// Size of the frame SDL just finished in the default framebuffer. Until the
// surface resize from RequestRenderSize lands, the drawable is still the
// full-panel size; the scaling blits cover those first frames.
void GetDrawableSize(SDL_Window* window, int* w, int* h) {
    SDL_GL_GetDrawableSize(window, w, h);
    if (*w != gXr.drawableW || *h != gXr.drawableH) {
        gXr.drawableW = *w;
        gXr.drawableH = *h;
        LOGI("Game drawable %dx%d", *w, *h);
    }
}

// Copies a srcW x srcH image into one eye-sized slot at dstX. The default
// framebuffer, the stash and GL swapchain images all have a bottom-left
// origin, so no vertical flip is needed.
void BlitEye(GLuint readFbo, int srcW, int srcH, GLuint drawFbo, int dstX) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
    glBlitFramebuffer(0, 0, srcW, srcH, dstX, 0, dstX + gXr.width, gXr.height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

// Saves the GL state a shader draw changes and restores it on scope exit
// (used inside a BlitState, which covers framebuffers, scissor and sRGB).
class DrawState {
  public:
    DrawState() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &mProgram);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &mVao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &mActiveTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &mTexture0);
        glGetIntegerv(GL_SAMPLER_BINDING, &mSampler0);
        glGetIntegerv(GL_VIEWPORT, mViewport);
        glGetBooleanv(GL_COLOR_WRITEMASK, mColorMask);
        for (int i = 0; i < kCapCount; ++i) {
            mCapsOn[i] = glIsEnabled(kCaps[i]);
            glDisable(kCaps[i]);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    ~DrawState() {
        glBindSampler(0, (GLuint) mSampler0);
        glBindTexture(GL_TEXTURE_2D, (GLuint) mTexture0);
        glActiveTexture((GLenum) mActiveTexture);
        glBindVertexArray((GLuint) mVao);
        glUseProgram((GLuint) mProgram);
        glViewport(mViewport[0], mViewport[1], mViewport[2], mViewport[3]);
        glColorMask(mColorMask[0], mColorMask[1], mColorMask[2], mColorMask[3]);
        for (int i = 0; i < kCapCount; ++i) {
            if (mCapsOn[i]) {
                glEnable(kCaps[i]);
            } else {
                glDisable(kCaps[i]);
            }
        }
    }

  private:
    static constexpr int kCapCount = 4;
    static constexpr GLenum kCaps[kCapCount] = { GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_STENCIL_TEST };
    GLint mProgram = 0, mVao = 0, mActiveTexture = 0, mTexture0 = 0, mSampler0 = 0, mViewport[4] = {};
    GLboolean mColorMask[4] = {};
    GLboolean mCapsOn[kCapCount] = {};
};

// The game texture copy (see CopyGameEye): a full-screen triangle that samples
// the game framebuffer's texture. In this libultraship the texture has the GL
// bottom-left origin like the swapchain image, so no flip (BattleShip's
// libultraship needed one; on device 2026-11-01 the flipped copy was upside down).
constexpr const char* kGameCopyVs = R"(#version 300 es
out vec2 vPos;
void main() {
    vPos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(vPos * 2.0 - 1.0, 0.0, 1.0);
}
)";
constexpr const char* kGameCopyFs = R"(#version 300 es
precision highp float;
uniform sampler2D uGame;
uniform bool uAlphaView;
uniform float uOpacity;
in vec2 vPos;
out vec4 oColor;
void main() {
    vec4 c = texture(uGame, vPos) * uOpacity; // premultiplied: fades into what's behind
    oColor = uAlphaView ? vec4(c.aaa, 1.0) : c;
}
)";

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Shader: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Failure is not fatal: CopyGameEye then blits from the default framebuffer.
void CreateGameCopy() {
    const GLuint vs = CompileShader(GL_VERTEX_SHADER, kGameCopyVs);
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kGameCopyFs);
    if (vs != 0 && fs != 0) {
        GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        GLint ok = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok) {
            gXr.copyProgram = program;
            gXr.copyAlphaViewLoc = glGetUniformLocation(program, "uAlphaView");
            gXr.copyOpacityLoc = glGetUniformLocation(program, "uOpacity");
            GLint prevProgram = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
            glUseProgram(program);
            glUniform1i(glGetUniformLocation(program, "uGame"), 0);
            glUseProgram((GLuint) prevProgram);
            glGenVertexArrays(1, &gXr.copyVao);
        } else {
            LOGE("Game copy program failed to link");
            glDeleteProgram(program);
        }
    }
    glDeleteShader(vs); // 0 is ignored
    glDeleteShader(fs);
}

// The game framebuffer's color texture (0 until the game has drawn a frame).
GLuint GameTexture() {
    Ship::Context* context = Ship::Context::GetRawInstance();
    if (context == nullptr) {
        return 0;
    }
    const auto window = context->GetWindow();
    return window ? (GLuint) window->GetGfxFrameBuffer() : 0;
}

// Copies the frame just drawn into one eye-sized slot at dstX, with its alpha.
// The source is the game framebuffer's texture, not the default framebuffer:
// the Quest build keeps the game's alpha (LUS_GL_GAME_FB_ALPHA), and the GUI's
// blended presentation into the default framebuffer would darken pixels with
// alpha below 1. Until the game has a texture, blit the default framebuffer.
// Runs inside a BlitState (scissor off, sRGB writes off).
void CopyGameEye(int srcW, int srcH, GLuint drawFbo, int dstX) {
    const GLuint texture = GameTexture();
    if (texture == 0 || gXr.copyProgram == 0) {
        BlitEye(0, srcW, srcH, drawFbo, dstX);
        return;
    }
    if (texture != gXr.loggedCopyTexture) {
        gXr.loggedCopyTexture = texture;
        LOGI("Presenting from the game texture (GL %u)", texture);
    }
    DrawState state;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
    glViewport(dstX, 0, gXr.width, gXr.height);
    glUseProgram(gXr.copyProgram);
    glUniform1i(gXr.copyAlphaViewLoc, CVarGetInteger(kAlphaViewCVar, 0) != 0);
    glUniform1f(gXr.copyOpacityLoc, gXr.sceneOpacity);
    glBindVertexArray(gXr.copyVao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindSampler(0, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// The left eye's swap: hold its image until the right eye's swap, so both
// halves of a swapchain image always come from the same frame (repeat XR
// frames re-show the last released image).
void StashLeftEye(SDL_Window* window) {
    int srcW = 0, srcH = 0;
    GetDrawableSize(window, &srcW, &srcH);
    BlitState state;
    CopyGameEye(srcW, srcH, gXr.stashFbo, 0);
    gXr.stashValid = true;
}

bool CopyHudImage();

// Copy the finished frame into the next swapchain image: on the right eye's
// swap, the stashed left eye and this frame side by side; on a flat present,
// this frame into the left half alone.
bool CopyGameFrame(SDL_Window* window) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!Ok(xrAcquireSwapchainImage(gXr.swapchain, &acquire, &index), "xrAcquireSwapchainImage")) {
        return false;
    }
    XrSwapchainImageWaitInfo wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    if (!Ok(xrWaitSwapchainImage(gXr.swapchain, &wait), "xrWaitSwapchainImage")) {
        return false;
    }

    int srcW = 0, srcH = 0;
    GetDrawableSize(window, &srcW, &srcH);
    const bool stereo = gXr.eye == 1 && gXr.stashValid;
    {
        BlitState state;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gXr.blitFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gXr.images[index].image, 0);
        if (stereo) {
            BlitEye(gXr.stashFbo, gXr.width, gXr.height, gXr.blitFbo, 0);
            CopyGameEye(srcW, srcH, gXr.blitFbo, gXr.width);
        } else {
            CopyGameEye(srcW, srcH, gXr.blitFbo, 0);
        }
    }
    gXr.stashValid = false;

    XrSwapchainImageReleaseInfo release{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (!Ok(xrReleaseSwapchainImage(gXr.swapchain, &release), "xrReleaseSwapchainImage")) {
        return false;
    }
    gXr.imageStereo = stereo;
    gXr.imageCutout = gXr.cutoutDraw && GameTexture() != 0 && gXr.copyProgram != 0;
    gXr.imageRoom = stereo && gXr.pairRoom;
    if (gXr.imageRoom) {
        gXr.imageViews[0] = gXr.pairViews[0];
        gXr.imageViews[1] = gXr.pairViews[1];
    }
    gXr.imageHud = gXr.imageRoom && CopyHudImage();
    return true;
}

// ---------------------------------------------------------------------------
// Passthrough
// ---------------------------------------------------------------------------

PassthroughFns gPassthroughFns;

void DestroyPassthrough() {
    if (gXr.passthroughLayer != XR_NULL_HANDLE && gPassthroughFns.destroyLayer != nullptr) {
        gPassthroughFns.destroyLayer(gXr.passthroughLayer);
    }
    if (gXr.passthrough != XR_NULL_HANDLE && gPassthroughFns.destroy != nullptr) {
        gPassthroughFns.destroy(gXr.passthrough);
    }
    gXr.passthroughLayer = XR_NULL_HANDLE;
    gXr.passthrough = XR_NULL_HANDLE;
    gXr.passthroughRunning = false;
}

bool CreatePassthrough() {
    PassthroughFns& fn = gPassthroughFns;
    if (!InstanceProc("xrCreatePassthroughFB", fn.create) || !InstanceProc("xrDestroyPassthroughFB", fn.destroy) ||
        !InstanceProc("xrPassthroughStartFB", fn.start) || !InstanceProc("xrPassthroughPauseFB", fn.pause) ||
        !InstanceProc("xrCreatePassthroughLayerFB", fn.createLayer) ||
        !InstanceProc("xrDestroyPassthroughLayerFB", fn.destroyLayer) ||
        !InstanceProc("xrPassthroughLayerResumeFB", fn.resumeLayer) ||
        !InstanceProc("xrPassthroughLayerPauseFB", fn.pauseLayer)) {
        return false;
    }
    XrPassthroughCreateInfoFB create{ XR_TYPE_PASSTHROUGH_CREATE_INFO_FB };
    create.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    if (!Ok(fn.create(gXr.session, &create, &gXr.passthrough), "xrCreatePassthroughFB")) {
        return false;
    }
    XrPassthroughLayerCreateInfoFB layer{ XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB };
    layer.passthrough = gXr.passthrough;
    layer.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    layer.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    return Ok(fn.createLayer(gXr.session, &layer, &gXr.passthroughLayer), "xrCreatePassthroughLayerFB");
}

// Follows gXrPassthrough (read live): creates the passthrough object and
// layer the first time it's wanted, pauses and resumes them after that.
// Any failure leaves opaque VR for the rest of the process.
void UpdatePassthrough() {
    if (!gXr.passthroughExt || gXr.passthroughFailed) {
        gCutoutAvailable = false;
        return;
    }
    const bool wanted = CVarGetInteger(kPassthroughCVar, 1) != 0;
    if (wanted != gXr.passthroughRunning) {
        PassthroughFns& fn = gPassthroughFns;
        bool ok;
        if (!wanted) {
            ok = Ok(fn.pauseLayer(gXr.passthroughLayer), "xrPassthroughLayerPauseFB") &&
                 Ok(fn.pause(gXr.passthrough), "xrPassthroughPauseFB");
        } else if (gXr.passthrough == XR_NULL_HANDLE) {
            ok = CreatePassthrough();
        } else {
            ok = Ok(fn.start(gXr.passthrough), "xrPassthroughStartFB") &&
                 Ok(fn.resumeLayer(gXr.passthroughLayer), "xrPassthroughLayerResumeFB");
        }
        if (!ok) {
            LOGE("Passthrough disabled for this run; staying in opaque VR");
            gXr.passthroughFailed = true;
            DestroyPassthrough();
            gCutoutAvailable = false;
            return;
        }
        gXr.passthroughRunning = wanted;
        LOGI("Passthrough %s", wanted ? "active" : "paused");
    }
    gCutoutAvailable = gXr.passthroughRunning && gXr.copyProgram != 0;
}

// Display refreshes each present covers when the refresh rate is a whole
// multiple of the present rate (120 Hz / 30 fps = 4), else 0.
int RefreshesPerPresent() {
    if (gXr.displayPeriod <= 0 || gXr.presentRate <= 0) {
        return 0;
    }
    const double refreshes = 1e9 / ((double) gXr.displayPeriod * gXr.presentRate);
    const double whole = std::round(refreshes);
    return (whole >= 1.0 && std::fabs(refreshes - whole) < 0.02) ? (int) whole : 0;
}

// A UI quad in LOCAL space (UI in the room view).
struct HudPlane {
    XrPosef pose;
    float width, height; // m
};
// A world sprite's quad: its rectangle of the HUD swapchain's sprite third.
struct SpriteQuad {
    HudPlane plane;
    XrRect2Di rect;
};
const std::vector<SpriteQuad>& HudImageSprites();
void Rotate(const XrQuaternionf& q, const float v[3], float out[3]);
HudPlane HudPanelPose();
const HudPlane* HudImagePlane();
const HudPlane* HudImageParty();
const HudPlane* HudImageStage();
XrSwapchain HudSwapchain();

void EndFrame(const XrFrameState& frame, bool showScreen) {
    XrCompositionLayerPassthroughFB passthrough{ XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB };
    XrCompositionLayerProjection projection{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView projectionViews[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
                                                            { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    XrCompositionLayerQuad quads[2] = { { XR_TYPE_COMPOSITION_LAYER_QUAD }, { XR_TYPE_COMPOSITION_LAYER_QUAD } };
    XrCompositionLayerQuad hudQuads[4] = { { XR_TYPE_COMPOSITION_LAYER_QUAD }, { XR_TYPE_COMPOSITION_LAYER_QUAD },
                                           { XR_TYPE_COMPOSITION_LAYER_QUAD },
                                           { XR_TYPE_COMPOSITION_LAYER_QUAD } };
    XrCompositionLayerQuad spriteQuads[kMaxSpriteQuads];
    const XrCompositionLayerBaseHeader* layers[6 + kMaxSpriteQuads] = {};
    uint32_t layerCount = 0;
    XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = frame.predictedDisplayTime;
    // FB passthrough composites as a layer; the blend mode stays opaque.
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (frame.shouldRender && gXr.passthroughRunning) {
        // Underlay: submitted first, the screen draws over it.
        passthrough.layerHandle = gXr.passthroughLayer;
        layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&passthrough);
    }
    if (showScreen && gXr.imageRoom) {
        // Room view: each half was rendered from that eye's pose and (widened)
        // field of view; the compositor reprojects it to the eye's current pose.
        const bool blend = gXr.imageCutout && gXr.passthroughRunning;
        projection.layerFlags = blend ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        projection.space = gXr.local;
        projection.viewCount = 2;
        projection.views = projectionViews;
        for (int i = 0; i < 2; ++i) {
            projectionViews[i].pose = gXr.imageViews[i].pose;
            projectionViews[i].fov = gXr.imageViews[i].fov;
            projectionViews[i].subImage.swapchain = gXr.swapchain;
            projectionViews[i].subImage.imageRect = { { i * gXr.width, 0 }, { gXr.width, gXr.height } };
        }
        layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection);
        // The UI layers over the scene: the steady panel, the messages on
        // the picture plane, the battle menus by the party (drawn in that
        // order). Premultiplied like the game image (cleared to 0,0,0,0,
        // "over" alpha).
        if (gXr.imageHud) {
            const HudPlane* plane = HudImagePlane();
            const HudPlane* party = HudImageParty();
            const HudPlane* stage = HudImageStage();
            const HudPlane placed[4] = { HudPanelPose(), plane != nullptr ? *plane : HudPlane{},
                                         party != nullptr ? *party : HudPlane{}, stage != nullptr ? *stage : HudPlane{} };
            auto addLayer = [&](int k) {
                if ((k == PORT_XR_HUD_PLANE && plane == nullptr) || (k == PORT_XR_HUD_PARTY && party == nullptr) ||
                    (k == PORT_XR_HUD_STAGE && stage == nullptr)) {
                    return;
                }
                XrCompositionLayerQuad& quad = hudQuads[k];
                quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                quad.space = gXr.local;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = HudSwapchain();
                quad.subImage.imageRect = { { 0, k * kHudImageH }, { kHudImageW, kHudImageH } };
                quad.pose = placed[k].pose;
                quad.size = { placed[k].width, placed[k].height };
                layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
            };
            // World sprites: each its own small upright quad. Quad layers don't
            // depth-test each other, they stack in submission order, so a sprite
            // behind the stage-front screens (battle menus, star points: both
            // stand at the stage's front edge) as seen from the head goes under
            // them, the others over everything.
            const std::vector<SpriteQuad>& sprites = HudImageSprites();
            const size_t spriteCount = std::min(sprites.size(), (size_t) kMaxSpriteQuads);
            const HudPlane* front = party != nullptr ? party : stage;
            const float head[3] = { 0.5f * (gXr.imageViews[0].pose.position.x + gXr.imageViews[1].pose.position.x),
                                    0.5f * (gXr.imageViews[0].pose.position.y + gXr.imageViews[1].pose.position.y),
                                    0.5f * (gXr.imageViews[0].pose.position.z + gXr.imageViews[1].pose.position.z) };
            auto behindFront = [&](const SpriteQuad& sprite) {
                if (front == nullptr) {
                    return false;
                }
                const float z[3] = { 0.0f, 0.0f, 1.0f };
                float n[3];
                Rotate(front->pose.orientation, z, n);
                const XrVector3f& p0 = front->pose.position;
                const XrVector3f& c = sprite.plane.pose.position;
                const float sideHead = (head[0] - p0.x) * n[0] + (head[1] - p0.y) * n[1] + (head[2] - p0.z) * n[2];
                const float sideSprite = (c.x - p0.x) * n[0] + (c.y - p0.y) * n[1] + (c.z - p0.z) * n[2];
                return (sideHead >= 0.0f) != (sideSprite >= 0.0f);
            };
            auto addSprite = [&](size_t i) {
                XrCompositionLayerQuad& quad = spriteQuads[i];
                quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
                quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                quad.space = gXr.local;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = HudSwapchain();
                quad.subImage.imageRect = sprites[i].rect;
                quad.pose = sprites[i].plane.pose;
                quad.size = { sprites[i].plane.width, sprites[i].plane.height };
                layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
            };
            // Back to front: panel, picture plane, sprites behind the stage
            // front, battle menus, star points, sprites in front.
            addLayer(PORT_XR_HUD_PANEL);
            addLayer(PORT_XR_HUD_PLANE);
            for (size_t i = 0; i < spriteCount; i++) {
                if (behindFront(sprites[i])) {
                    addSprite(i);
                }
            }
            addLayer(PORT_XR_HUD_PARTY);
            addLayer(PORT_XR_HUD_STAGE);
            for (size_t i = 0; i < spriteCount; i++) {
                if (!behindFront(sprites[i])) {
                    addSprite(i);
                }
            }
        }
    } else if (showScreen) {
        // The quads show the most recently released swapchain image: one per
        // eye at the same pose for a stereo pair, else the left half to both.
        // A cut-out image blends over the room. Its color is premultiplied:
        // the game blends over a black, alpha-0 background with "over" alpha.
        const bool blend = gXr.imageCutout && gXr.passthroughRunning;
        const int count = gXr.imageStereo ? 2 : 1;
        for (int i = 0; i < count; ++i) {
            XrCompositionLayerQuad& quad = quads[i];
            quad.layerFlags = blend ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
            quad.space = gXr.local;
            quad.eyeVisibility = !gXr.imageStereo ? XR_EYE_VISIBILITY_BOTH
                                                  : (i == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT);
            quad.subImage.swapchain = gXr.swapchain;
            quad.subImage.imageRect = { { i * gXr.width, 0 }, { gXr.width, gXr.height } };
            quad.pose.orientation.w = 1.0f;
            quad.pose.position = { 0.0f, 0.0f, -kScreenDistance };
            quad.size = { kScreenHeight * (float) gXr.width / (float) gXr.height, kScreenHeight };
            layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
        }
    }
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount > 0 ? layers : nullptr;
    Ok(xrEndFrame(gXr.session, &endInfo), "xrEndFrame");
}

// Presents per second equal the interpolation rate when the game keeps up,
// so this is the check that the game runs at full speed.
void LogPacingStats(XrTime displayTime) {
    ++gXr.statsPresents;
    if (gXr.statsStart == 0) {
        gXr.statsStart = displayTime;
        return;
    }
    const XrDuration elapsed = displayTime - gXr.statsStart;
    if (elapsed < kStatsPeriod) {
        return;
    }
    LOGI("Pacing: %.2f presents/s (target %d), %d repeat frames, %d resyncs",
         gXr.statsPresents * 1e9 / (double) elapsed, gXr.presentRate, gXr.statsRepeats, gXr.statsResyncs);
    gXr.statsStart = displayTime;
    gXr.statsPresents = gXr.statsRepeats = gXr.statsResyncs = 0;
}

// Present the game frame. When the display clock paces (port_xr_paces_frames),
// keep running XR frames that re-show the previous image until the refresh
// this present is due on, so each present is held for exactly N refreshes.
// The schedule is absolute: presents after a short stall are not held, so the
// game catches up; only a stall longer than kMaxCatchUpPresents resyncs.
void SubmitFrame(SDL_Window* window) {
    UpdatePassthrough();
    const int refreshes = port_xr_paces_frames() ? RefreshesPerPresent() : 0;
    if (refreshes != gXr.loggedRefreshes) {
        gXr.loggedRefreshes = refreshes;
        if (refreshes > 0) {
            LOGI("Display clock paces presents: %d fps, %d refresh(es) each", gXr.presentRate, refreshes);
        } else {
            LOGI("Frame limiter paces presents (%d fps)", gXr.presentRate);
        }
    }
    if (refreshes == 0) {
        gXr.nextPresentTime = 0;
    }
    for (;;) {
        XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState frame{ XR_TYPE_FRAME_STATE };
        if (!Ok(xrWaitFrame(gXr.session, &waitInfo, &frame), "xrWaitFrame")) {
            return;
        }
        XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
        if (!Ok(xrBeginFrame(gXr.session, &beginInfo), "xrBeginFrame")) {
            return;
        }
        gXr.displayPeriod = frame.predictedDisplayPeriod;
        gXr.lastDisplayTime = frame.predictedDisplayTime;
        UpdateInput();

        const XrTime displayTime = frame.predictedDisplayTime;
        const bool due = refreshes == 0 || gXr.nextPresentTime == 0 ||
                         displayTime + frame.predictedDisplayPeriod / 2 >= gXr.nextPresentTime;
        if (due && frame.shouldRender && CopyGameFrame(window)) {
            gXr.hasImage = true;
        }
        EndFrame(frame, frame.shouldRender && gXr.hasImage);
        if (!due) {
            ++gXr.statsRepeats;
            continue;
        }
        if (refreshes > 0) {
            const XrDuration interval = refreshes * frame.predictedDisplayPeriod;
            const bool resync =
                gXr.nextPresentTime == 0 || displayTime > gXr.nextPresentTime + kMaxCatchUpPresents * interval;
            gXr.nextPresentTime = (resync ? displayTime : gXr.nextPresentTime) + interval;
            gXr.statsResyncs += resync;
        }
        LogPacingStats(displayTime);
        return;
    }
}

// ---------------------------------------------------------------------------
// Stereo camera
// ---------------------------------------------------------------------------
//
// Paper Mario loads each 3D camera as one projection-slot matrix, the view
// combined with the perspective (cam_main.c: mtxPerspective = view [* lead
// * shake] * persp). Fast3D decodes it in row-vector form: clip = [x y z 1]*M.
// With a rigid view V and a symmetric perspective P scaled by s, M = V*(s*P),
// and the matrix alone yields both factors:
//   |column 0, rows 0-2| = s*P00     |column 3, rows 0-2| = s  (P23 = -1)
// An eye at camera-space x offset e sees x_c - e. Adding a shear that cancels
// the shift at depth D gives zero parallax there:
//   x_clip' = x_clip + (P00*e/D)*w_clip - s*P00*e
// i.e. M'[i][0] = M[i][0] + (P00*e/D)*M[i][3], then M'[3][0] -= s*P00*e.

constexpr float kRenderAspect = (float) kRenderWidth / (float) kRenderHeight;

float Norm3(float a, float b, float c) {
    return std::sqrt(a * a + b * b + c * c);
}

float Distance(const float a[3], const float b[3]) {
    return Norm3(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
}

// Views and ortho projections: w_clip doesn't depend on depth.
bool IsAffine(const float m[4][4]) {
    return std::fabs(m[0][3]) < 1e-6f && std::fabs(m[1][3]) < 1e-6f && std::fabs(m[2][3]) < 1e-6f &&
           std::fabs(m[3][3] - 1.0f) < 1e-6f;
}

// The matrix as the interpreter reads it from game memory (GfxSpMatrix).
void DecodeMtx(const Mtx* mtx, float out[4][4]) {
#ifndef GBI_FLOATS
    const int32_t* addr = reinterpret_cast<const int32_t*>(mtx);
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            const int32_t intPart = addr[i * 2 + j / 2];
            const uint32_t fracPart = (uint32_t) addr[8 + i * 2 + j / 2];
            out[i][j] = (int32_t) ((intPart & 0xffff0000) | (fracPart >> 16)) / 65536.0f;
            out[i][j + 1] = (int32_t) ((intPart << 16) | (fracPart & 0xffff)) / 65536.0f;
        }
    }
#else
    std::memcpy(out, mtx, sizeof(float) * 16);
#endif
}

struct StereoParams {
    float separation;
    float convergence;
    bool swapEyes;
};

StereoParams ReadStereoParams() {
    return { std::clamp(CVarGetFloat(kStereoSeparationCVar, kDefaultSeparation), 0.0f, 0.2f),
             std::clamp(CVarGetFloat(kStereoConvergenceCVar, kDefaultConvergence), 0.1f, 10.0f),
             CVarGetInteger(kStereoSwapEyesCVar, 0) != 0 };
}

// Writes the eye's copy of perspective matrix `in` (see above).
// `effectiveSeparation`, if given, receives the separation after the
// infinity-disparity cap.
bool ShiftForEye(const float in[4][4], float focus, int eye, const StereoParams& params, float out[4][4],
                 float* effectiveSeparation = nullptr) {
    if (IsAffine(in) || !(focus > 0.0f)) {
        return false;
    }
    const float sP00 = Norm3(in[0][0], in[1][0], in[2][0]);
    const float s = Norm3(in[0][3], in[1][3], in[2][3]);
    if (s < 1e-9f || sP00 < 1e-9f) {
        return false;
    }
    const float p00 = sP00 / s;
    // A point at infinity lands P00*separation/convergence apart in NDC
    // (2 NDC units = the screen width).
    const float maxNdc = 2.0f * kMaxInfinityDisparity / (kScreenHeight * kRenderAspect);
    const float separation = std::min(params.separation, maxNdc * params.convergence / p00);
    if (effectiveSeparation != nullptr) {
        *effectiveSeparation = separation;
    }
    const bool right = (eye == 1) != params.swapEyes;
    const float e = (right ? 0.5f : -0.5f) * separation * focus;
    const float D = params.convergence * focus;
    const float shear = p00 * e / D;
    std::memcpy(out, in, sizeof(float) * 16);
    for (int i = 0; i < 4; i++) {
        out[i][0] += shear * in[i][3];
    }
    out[3][0] -= sP00 * e;
    return true;
}

void Project(const float m[4][4], const float p[3], float clip[4]) {
    for (int j = 0; j < 4; j++) {
        clip[j] = p[0] * m[0][j] + p[1] * m[1][j] + p[2] * m[2][j] + m[3][j];
    }
}

float NdcX(const float m[4][4], const float p[3]) {
    float clip[4];
    Project(m, p, clip);
    return clip[0] / clip[3];
}

// Logs a check of ShiftForEye against one camera matrix: with convergence at
// the focus distance, the look-at point must project to the same spot for
// both eyes, and a point twice as far must sit behind the screen (left-eye
// image left of the right-eye image).
void StereoCheck(const float m[4][4], const CameraNote& cam) {
    if (IsAffine(m)) {
        return; // ortho cameras are not shifted
    }
    const float focus = Distance(cam.eye, cam.at);
    const float s = Norm3(m[0][3], m[1][3], m[2][3]);
    const float p00 = Norm3(m[0][0], m[1][0], m[2][0]) / s;

    const float* at = cam.at;
    const float* eye = cam.eye;
    float clip[4];
    Project(m, at, clip);
    if (std::fabs(clip[3] - s * focus) > 0.01f * s * focus) {
        LOGI("Stereo check: unrecognized camera matrix (look-at depth %.1f, expected %.1f)", clip[3] / s, focus);
        return;
    }
    const float farPoint[3] = { 2.0f * at[0] - eye[0], 2.0f * at[1] - eye[1], 2.0f * at[2] - eye[2] };

    StereoParams params = ReadStereoParams();
    const float convergence = params.convergence;
    params.convergence = 1.0f;
    float left[4][4], right[4][4], separation = 0.0f;
    ShiftForEye(m, focus, 0, params, left, &separation);
    ShiftForEye(m, focus, 1, params, right);
    const float atNdc = NdcX(m, at);
    const float atError = std::max(std::fabs(NdcX(left, at) - atNdc), std::fabs(NdcX(right, at) - atNdc));
    // A point at twice the focus distance: L - R = -P00*sep/2 (left image left).
    const float farDisparity = NdcX(left, farPoint) - NdcX(right, farPoint);
    const float farExpected = (params.swapEyes ? 1.0f : -1.0f) * p00 * separation / 2.0f;

    // Settings in use: where infinity lands, in cm on the screen.
    float inUse = 0.0f;
    params.convergence = convergence;
    ShiftForEye(m, focus, 0, params, left, &inUse);
    const float infinityCm = 100.0f * p00 * inUse / convergence * kScreenHeight * kRenderAspect / 2.0f;

    LOGI("Stereo check: focus %.0f, P00 %.3f, scale %.4g | at-plane error %.1e (want ~0) | "
         "2x-focus L-R %.5f (want %.5f; <0 = behind screen) | sep %.4f conv %.2f -> infinity %.1f cm%s",
         focus, p00, s, atError, farDisparity, farExpected, inUse, convergence, infinityCm,
         params.swapEyes ? " [eyes swapped]" : "");
}

// ---------------------------------------------------------------------------
// Room view
// ---------------------------------------------------------------------------
//
// The scene becomes an object in the room: each eye renders the game world
// from its tracked pose (LOCAL space) and field of view, and the pair is
// submitted as a projection layer. The game world maps into the room level
// (world +y = room up) and at a fixed scale (gXrRoomUnitsPerMeter): the
// camera's look-at point sits at (0, gXrRoomHeight, -gXrRoomDistance) and the
// camera's horizontal heading points along room -z, so from the default
// position you look at the scene the way the game camera does. The camera
// keeps following Mario, so the world slides through that spot as he walks.
//
// Each noted perspective matrix M (clip = [world 1]*M) is replaced by one that
// maps world -> room -> eye -> the eye's clip space. Depth keeps the game's
// own mapping (z/w as a function of distance in game units), so fog and the
// depth range behave as in the game. x is pre-divided by Fast3D's widescreen
// squeeze and both axes are mapped through the camera's viewport, so the
// eye's field of view lands exactly on the whole framebuffer.

constexpr int kRoomCheckPresents = 300;

struct RoomParams {
    float distance, height, unitsPerMeter;
};

RoomParams ReadRoomParams() {
    return { std::clamp(CVarGetFloat(kRoomDistanceCVar, kDefaultRoomDistance), 0.2f, 5.0f),
             std::clamp(CVarGetFloat(kRoomHeightCVar, kDefaultRoomHeight), -2.0f, 1.0f),
             std::clamp(CVarGetFloat(kRoomUnitsPerMeterCVar, kDefaultRoomUnitsPerMeter), 20.0f, 20000.0f) };
}

// Fast3D's clip x scale for 3D vertices on the game framebuffer
// (Interpreter::AdjXForAspectRatio): 4:3 over the framebuffer's aspect.
float WidescreenSqueeze() {
    return (4.0f / 3.0f) / kRenderAspect;
}

// Rotates v by unit quaternion q.
void Rotate(const XrQuaternionf& q, const float v[3], float out[3]) {
    const float qv[3] = { q.x, q.y, q.z };
    // t = 2 * cross(q.xyz, v); out = v + w*t + cross(q.xyz, t)
    const float t[3] = { 2.0f * (qv[1] * v[2] - qv[2] * v[1]), 2.0f * (qv[2] * v[0] - qv[0] * v[2]),
                         2.0f * (qv[0] * v[1] - qv[1] * v[0]) };
    out[0] = v[0] + q.w * t[0] + (qv[1] * t[2] - qv[2] * t[1]);
    out[1] = v[1] + q.w * t[1] + (qv[2] * t[0] - qv[0] * t[2]);
    out[2] = v[2] + q.w * t[2] + (qv[0] * t[1] - qv[1] * t[0]);
}

// Widens a view's field of view across so a framebuffer pixel covers the same
// angle across and up (the image is 16:9, an eye's field of view about 1:1).
// Otherwise screen-space sprites and the HUD would come out stretched.
void RoomFov(XrFovf& fov) {
    const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
    const float u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
    const float cx = 0.5f * (r + l);
    const float hx = std::max(0.5f * (r - l), 0.5f * (u - d) * kRenderAspect);
    fov.angleLeft = std::atan(cx - hx);
    fov.angleRight = std::atan(cx + hx);
}

// The eye poses and fields of view for the pair displayed at `time`.
bool SampleRoomViews(XrTime time) {
    XrViewLocateInfo info{ XR_TYPE_VIEW_LOCATE_INFO };
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = time;
    info.space = gXr.local;
    XrViewState state{ XR_TYPE_VIEW_STATE };
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t count = 0;
    if (!XR_SUCCEEDED(xrLocateViews(gXr.session, &info, &state, 2, &count, views)) || count != 2) {
        return false;
    }
    const XrViewStateFlags needed = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if ((state.viewStateFlags & needed) != needed) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        gXr.pairViews[i] = views[i];
        RoomFov(gXr.pairViews[i].fov);
    }
    return true;
}

// The parts of a perspective camera matrix the room view keeps: its scale s
// and the depth mapping z_clip = -P22*w_clip + s*P32 (Paper Mario's
// guPerspective: w_clip = -s*z_camera).
struct PerspDepth {
    float s, p22, sP32;
};

bool ReadPerspDepth(const float m[4][4], PerspDepth* out) {
    if (IsAffine(m)) {
        return false;
    }
    const float s = Norm3(m[0][3], m[1][3], m[2][3]);
    if (s < 1e-9f) {
        return false;
    }
    int row = 0;
    for (int i = 1; i < 3; i++) {
        if (std::fabs(m[i][3]) > std::fabs(m[row][3])) {
            row = i;
        }
    }
    out->s = s;
    out->p22 = -m[row][2] / m[row][3];
    out->sP32 = m[3][2] + out->p22 * m[3][3];
    return true;
}

// World -> room (LOCAL space, m): level, the camera's heading along -z, its
// look-at point at the anchor. The camera must not look straight down/up.
void WorldToRoom(const double p[3], const CameraNote& cam, const RoomParams& room, double r[3]) {
    double fx = (double) cam.at[0] - cam.eye[0], fz = (double) cam.at[2] - cam.eye[2];
    const double fl = std::sqrt(fx * fx + fz * fz);
    fx /= fl;
    fz /= fl;
    const double k = room.unitsPerMeter;
    const double d[3] = { p[0] - cam.at[0], p[1] - cam.at[1], p[2] - cam.at[2] };
    r[0] = (-fz * d[0] + fx * d[2]) / k;
    r[1] = d[1] / k + room.height;
    r[2] = -(fx * d[0] + fz * d[2]) / k - room.distance;
}

// Maps one world point to the eye's clip space (see the section comment).
// In double: RoomEyeMatrix differences nearby results.
void RoomClip(const double p[3], const CameraNote& cam, const XrView& view, const PerspDepth& depth,
              const RoomParams& room, double clip[4]) {
    double r[3];
    WorldToRoom(p, cam, room, r);
    // Room -> eye: rotate by the inverse of the view pose's orientation.
    const double qx = -view.pose.orientation.x, qy = -view.pose.orientation.y, qz = -view.pose.orientation.z;
    const double qw = view.pose.orientation.w;
    const double v[3] = { r[0] - view.pose.position.x, r[1] - view.pose.position.y, r[2] - view.pose.position.z };
    const double t[3] = { 2.0 * (qy * v[2] - qz * v[1]), 2.0 * (qz * v[0] - qx * v[2]), 2.0 * (qx * v[1] - qy * v[0]) };
    const double e[3] = { v[0] + qw * t[0] + (qy * t[2] - qz * t[1]), v[1] + qw * t[1] + (qz * t[0] - qx * t[2]),
                          v[2] + qw * t[2] + (qx * t[1] - qy * t[0]) };
    // Eye -> clip, through the field of view, the viewport and the squeeze.
    const double l = std::tan((double) view.fov.angleLeft), rt = std::tan((double) view.fov.angleRight);
    const double u = std::tan((double) view.fov.angleUp), dn = std::tan((double) view.fov.angleDown);
    const double cx = 0.5 * (rt + l), hx = 0.5 * (rt - l);
    const double cy = 0.5 * (u + dn), hy = 0.5 * (u - dn);
    const double vx0 = ((double) cam.viewport[0] + cam.viewport[2]) / 320.0 - 1.0;
    const double vxs = ((double) cam.viewport[2] - cam.viewport[0]) / 320.0;
    const double vy0 = 1.0 - ((double) cam.viewport[1] + cam.viewport[3]) / 240.0;
    const double vys = ((double) cam.viewport[3] - cam.viewport[1]) / 240.0;
    const double W = -e[2]; // distance ahead, m
    const double sk = depth.s * room.unitsPerMeter;
    clip[0] = sk * ((e[0] - cx * W) / hx - vx0 * W) / (vxs * WidescreenSqueeze());
    clip[1] = sk * ((e[1] - cy * W) / hy - vy0 * W) / vys;
    clip[3] = sk * W;
    clip[2] = -depth.p22 * clip[3] + depth.sP32;
}

// The eye's replacement for perspective camera matrix `in`. RoomClip is affine
// in the world point, so the matrix is read off the images of the origin and
// the unit axes.
bool RoomEyeMatrix(const float in[4][4], const CameraNote& cam, int eye, float out[4][4]) {
    PerspDepth depth;
    if (!ReadPerspDepth(in, &depth)) {
        return false;
    }
    const float fx = cam.at[0] - cam.eye[0], fz = cam.at[2] - cam.eye[2];
    if (fx * fx + fz * fz < 1e-6f || cam.viewport[2] <= cam.viewport[0] || cam.viewport[3] <= cam.viewport[1]) {
        return false; // looking straight down/up, or no viewport
    }
    const RoomParams room = ReadRoomParams();
    const XrView& view = gXr.pairViews[eye == 1 ? 1 : 0];
    const double origin[3] = { 0.0, 0.0, 0.0 };
    double base[4];
    RoomClip(origin, cam, view, depth, room, base);
    for (int i = 0; i < 3; i++) {
        double axis[3] = { 0.0, 0.0, 0.0 }, clip[4];
        axis[i] = 1.0;
        RoomClip(axis, cam, view, depth, room, clip);
        for (int j = 0; j < 4; j++) {
            out[i][j] = (float) (clip[j] - base[j]);
        }
    }
    for (int j = 0; j < 4; j++) {
        out[3][j] = (float) base[j];
    }
    return true;
}

// Logs a check of RoomEyeMatrix: the look-at point must land, through the
// replacement matrix, the viewport and Fast3D's squeeze, where the eye sees
// the room anchor; 10 cm of world up above it must land 10 cm above the anchor.
void RoomCheck(const float m[4][4], const CameraNote& cam) {
    float left[4][4];
    if (!RoomEyeMatrix(m, cam, 0, left)) {
        return;
    }
    const RoomParams room = ReadRoomParams();
    const XrView& view = gXr.pairViews[0];
    // Window NDC of a world point through the matrix (what Fast3D does).
    auto windowNdc = [&](const float p[3], float ndc[2]) {
        float clip[4];
        Project(left, p, clip);
        const float x = WidescreenSqueeze() * clip[0] / clip[3], y = clip[1] / clip[3];
        const float vx0 = (cam.viewport[0] + cam.viewport[2]) / 320.0f - 1.0f;
        const float vxs = (cam.viewport[2] - cam.viewport[0]) / 320.0f;
        const float vy0 = 1.0f - (cam.viewport[1] + cam.viewport[3]) / 240.0f;
        const float vys = (cam.viewport[3] - cam.viewport[1]) / 240.0f;
        ndc[0] = vx0 + vxs * x;
        ndc[1] = vy0 + vys * y;
    };
    // Window NDC of a room point seen by the eye (independent of the matrix).
    auto eyeNdc = [&](const float r[3], float ndc[2]) {
        const XrQuaternionf inv = { -view.pose.orientation.x, -view.pose.orientation.y, -view.pose.orientation.z,
                                    view.pose.orientation.w };
        const float rel[3] = { r[0] - view.pose.position.x, r[1] - view.pose.position.y,
                               r[2] - view.pose.position.z };
        float e[3];
        Rotate(inv, rel, e);
        const float l = std::tan(view.fov.angleLeft), rt = std::tan(view.fov.angleRight);
        const float u = std::tan(view.fov.angleUp), dn = std::tan(view.fov.angleDown);
        ndc[0] = (e[0] / -e[2] - 0.5f * (rt + l)) / (0.5f * (rt - l));
        ndc[1] = (e[1] / -e[2] - 0.5f * (u + dn)) / (0.5f * (u - dn));
    };
    const float anchor[3] = { 0.0f, room.height, -room.distance };
    const float above[3] = { 0.0f, room.height + 0.1f, -room.distance };
    const float upWorld[3] = { cam.at[0], cam.at[1] + 0.1f * room.unitsPerMeter, cam.at[2] };
    float a[2], b[2], c[2], d2[2];
    windowNdc(cam.at, a);
    eyeNdc(anchor, b);
    windowNdc(upWorld, c);
    eyeNdc(above, d2);
    const float error = std::max(std::max(std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1])),
                                 std::max(std::fabs(c[0] - d2[0]), std::fabs(c[1] - d2[1])));
    LOGI("Room check: focus %.0f, %.0f units/m, anchor %+.2f %+.2f m | NDC error %.1e (want ~0) | "
         "viewport %.0f,%.0f-%.0f,%.0f | L eye at %+.2f %+.2f %+.2f m, fov x %.2f..%.2f y %.2f..%.2f (tan)",
         Distance(cam.eye, cam.at), room.unitsPerMeter, room.height, -room.distance, error, cam.viewport[0],
         cam.viewport[1], cam.viewport[2], cam.viewport[3], view.pose.position.x, view.pose.position.y,
         view.pose.position.z, std::tan(view.fov.angleLeft), std::tan(view.fov.angleRight),
         std::tan(view.fov.angleDown), std::tan(view.fov.angleUp));
}

// ---------------------------------------------------------------------------
// UI in the room view
// ---------------------------------------------------------------------------
//
// gfx_draw_frame brackets the screen-space UI passes with two no-ops each and
// notes them per main display list (port_xr_note_hud), with a kind and the
// frame camera's matrix. For a room-view pair BeginStereoPair turns them into
// G_SETFB (that kind's framebuffer) / G_RESETFB, so the UI draws into our
// own Fast3D framebuffers instead of the eye images; after the pair they go
// back to no-ops, so flat presents and later frames draw the UI as before.
// The interact prompts and UI effects (damage numbers) are 3D objects at
// world positions and stay unbracketed, in the scene.
//
//   PORT_XR_HUD_PANEL - status bar, menus, windows: a steady panel above
//     the scene (HudPanelPose), facing the LOCAL origin; it doesn't follow
//     the game camera's zoom.
//   PORT_XR_HUD_PLANE - messages (speech bubbles): the frame camera's
//     picture plane, i.e. the plane through its look-at point facing it,
//     spanning exactly what the camera shows at that depth
//     (HudPlaneFromCamera), so a bubble the game placed over its speaker
//     floats over the speaker (exactly at the look-at depth).
//   PORT_XR_HUD_STAGE - battle star points: the party layer's mirror image,
//     its right edge at the stage's front right (same scale and offsets,
//     the offset across mirrored).
//   PORT_XR_HUD_PARTY - battle menus: a constant-scale projector screen
//     standing at the stage's front left, facing the room's +z (the way
//     the stage faces the player): its anchor's screen point (the screen's
//     left edge at the wheel's foot, port_xr_note_hud_anchor) sits at its anchor's
//     world point (a fixed point of the battle stage) mapped into the room
//     like the scene (HudAnchoredLayer), so it moves with the stage and
//     doesn't zoom with the battle camera.
// Ranges may nest: an inner range's closing bracket switches back to the
// outer range's framebuffer.
//
// The framebuffers are native 320x240 with resize on: Fast3D keeps their
// storage at the game framebuffer's size (1920x1080) and lays them out like
// the game screen; the whole 16:9 width is used (this port stretches the
// status bar to the widescreen edges). They are kept non-inverted
// (KeepHudUpright): Fast3D computes an off-screen framebuffer's scissor as
// for a non-inverted one, so with its default invertY every scissor landed
// mirrored and clipped away the battle menus' option lists. Both eyes'
// passes draw them, so they are cleared to (0,0,0,0) before each. The
// released room image copies them into thirds of the HUD swapchain
// (CopyHudImage), each shown on a quad.
//
// World-anchored elements (the battle target hand, enemy HP bars) are
// bracketed separately (port_xr_note_world_sprite): their brackets become
// G_SETFB to the sprite framebuffer (kHudSpriteKind) and back, and each is shown
// on its own quad (SpriteQuadFor): its box of that framebuffer, at the game's
// size against the scene, with its anchor's screen point on the anchor mapped
// into the room; upright, facing the head, or the stage's front for stage
// sprites (HP bars). Without the UI layers (gXrHud 0) they draw into the eye
// images, moved and scaled per eye to the anchor (ShiftSprite).

constexpr int kHudKinds = 5; // PORT_XR_HUD_PANEL, _PLANE, _PARTY, _STAGE, kHudSpriteKind
constexpr const char* kHudCVar = "gXrHud"; // 1 = UI off the eye images in the room view (default)
constexpr const char* kHudBackingCVar = "gXrHudBacking"; // debug: alpha of a black card behind the UI
constexpr float kDefaultHudBacking = 0.0f;
constexpr int kHudPlaneLogPresents = 300; // ~10 s
// The picture plane shrunk by this factor about the camera's look-at point
// (the room anchor), read live: 1 = exact alignment with the scene.
constexpr const char* kHudScaleCVar = "gXrHudScale";
// The steady panel (read live): its width (the 16:9 screen), and its center
// this far above / behind the room anchor (gXrRoomHeight / gXrRoomDistance).
constexpr const char* kHudPanelWidthCVar = "gXrHudPanelWidth";
constexpr const char* kHudPanelUpCVar = "gXrHudPanelUp";
constexpr const char* kHudPanelBackCVar = "gXrHudPanelBack";
constexpr float kDefaultHudPanelWidth = 0.9f;
constexpr float kDefaultHudPanelUp = 0.15f;
constexpr float kDefaultHudPanelBack = 0.4f;
// The party layer (read live): the width the 16:9 screen has on it; its
// anchor moved this far right / up / back (m, room axes); and its lean back
// from vertical (degrees).
constexpr const char* kHudPartyWidthCVar = "gXrHudPartyWidth";
constexpr const char* kHudPartyRightCVar = "gXrHudPartyRight";
constexpr const char* kHudPartyUpCVar = "gXrHudPartyUp";
constexpr const char* kHudPartyBackCVar = "gXrHudPartyBack";
constexpr const char* kHudPartyTiltCVar = "gXrHudPartyTilt";
constexpr float kDefaultHudPartyWidth = 1.28f;
constexpr float kDefaultHudPartyRight = 0.15f;
// The stage layer (star points, the party layer's mirror) moved this far right
// of the stage's front right (m, room x; negative = toward the stage center).
constexpr const char* kHudStageRightCVar = "gXrHudStageRight";
constexpr float kDefaultHudStageRight = -0.23f;
constexpr float kDefaultHudPartyUp = -0.03f;
constexpr float kDefaultHudPartyBack = 0.0f;
constexpr float kDefaultHudPartyTilt = 0.0f;
// World sprites' size against the scene, times this (read live).
constexpr const char* kHudSpriteScaleCVar = "gXrHudSpriteScale";

struct HudRange {
    Fast::F3DGfx* begin; // the two placeholder commands
    Fast::F3DGfx* end;
    int kind;
    int parent = -1; // kind of the range it sits in, -1 = none
};

// A screen transition's commands (port_xr_note_scene_fade).
struct FadeRange {
    Fast::F3DGfx* begin; // the two placeholder commands
    Fast::F3DGfx* end;
    float coverage; // 0..1 of the screen
};

// A layer's anchor (port_xr_note_hud_anchor).
struct HudLayerAnchor {
    bool valid = false;
    float world[3] = {};
    int screen[2] = {};
};

struct WorldSprite {
    Fast::F3DGfx* begin; // the two placeholder commands
    Fast::F3DGfx* end;
    const void* mtx;  // the anchor camera's matrix (a noted camera)
    float world[3];   // anchor point
    int screen[2];    // where the game put it on screen (native px)
    int kind = -1;    // kind of the UI range it sits in, -1 = none
    bool stageFacing = false; // its quad faces the stage's front (room +z), not the head
    std::vector<std::pair<uintptr_t, uintptr_t>> saved; // the commands between the brackets, as built
};

struct Hud {
    std::vector<HudRange> ranges;     // of the frame being drawn (port_xr_frame_main)
    std::vector<WorldSprite> sprites; // ditto
    std::vector<FadeRange> fades;     // ditto
    std::vector<HudRange> sceneRanges; // ditto: 3D back into the eye images (kind = enclosing UI range's, or -1)
    int fb[kHudKinds] = { -1, -1, -1, -1, -1 }; // Fast3D framebuffer ids
    bool fbFailed = false;
    bool redirect = false; // the current pair draws the UI into fb
    bool loggedRedirect = false;
    XrSwapchain swapchain = XR_NULL_HANDLE; // kind k in rows [k*kHudImageH, (k+1)*kHudImageH)
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    bool swapchainFailed = false;
    GLuint drawFbo = 0, clearFbo = 0;
    GLuint program = 0, vao = 0, sampler = 0;
    GLint rectLoc = -1, backingLoc = -1;
    const void* camMtx = nullptr; // the frame camera's matrix (port_xr_frame_main)
    bool planeValid = false;      // plane: for the pair being drawn, from that camera
    HudPlane plane = {};
    bool imagePlaneValid = false; // ...as of the released image
    HudPlane imagePlane = {};
    HudLayerAnchor anchors[kHudKinds]; // of the frame being drawn
    bool partyValid = false;           // party: for the pair being drawn
    HudPlane party = {};
    bool imagePartyValid = false; // ...as of the released image
    HudPlane imageParty = {};
    bool stageValid = false; // stage (star points): for the pair being drawn
    HudPlane stage = {};
    bool imageStageValid = false; // ...as of the released image
    HudPlane imageStage = {};
    std::vector<SpriteQuad> spriteQuads;      // for the pair being drawn
    std::vector<SpriteQuad> imageSpriteQuads; // ...as of the released image
    int logCountdown = 0;
    int spriteLogCountdown = 0;
};
Hud gHud;

// Noted by the game thread, per main display list: brackets, the frame
// camera's matrix, world sprites; and the anchors set for UI elements.
struct HudAnchor {
    int cam;
    float world[3];
    int screen[2];
    unsigned stamp; // gHudBuild when set
    bool stageFacing; // port_xr_anchor_stage_sprite
};
std::mutex gHudMutex;
std::unordered_map<const void*, std::vector<HudRange>> gHudNotes;
std::unordered_map<const void*, const void*> gHudCameras;
std::unordered_map<const void*, std::vector<WorldSprite>> gSpriteNotes;
std::unordered_map<const void*, std::vector<FadeRange>> gFadeNotes;
std::unordered_map<const void*, std::vector<HudRange>> gSceneNotes;
std::unordered_map<const void*, HudAnchor> gHudAnchors;
std::unordered_map<const void*, std::vector<std::pair<int, HudLayerAnchor>>> gLayerAnchorNotes;
unsigned gHudBuild = 0; // frames built (port_xr_hud_frame_start)

std::shared_ptr<Fast::Interpreter> HudInterpreter() {
    Ship::Context* context = Ship::Context::GetRawInstance();
    if (context == nullptr) {
        return nullptr;
    }
    auto window = std::dynamic_pointer_cast<Fast::Fast3dWindow>(context->GetWindow());
    return window ? window->GetInterpreterWeak().lock() : nullptr;
}

// Created on first use (render thread, between draws).
bool CreateHudFramebuffers() {
    static_assert(sizeof(Hud::fb) / sizeof(int) == kHudKinds, "one framebuffer per kind");
    if (gHud.fbFailed) {
        return false;
    }
    if (std::all_of(std::begin(gHud.fb), std::end(gHud.fb), [](int fb) { return fb > 0; })) {
        return true;
    }
    gHud.fbFailed = true;
    const auto interp = HudInterpreter();
    if (!interp || interp->mRapi == nullptr) {
        LOGE("HUD: no Fast3D interpreter");
        return false;
    }
    for (int k = 0; k < kHudKinds; k++) {
        const int fb = interp->CreateFrameBuffer(320, 240, 320, 240, 1);
        auto it = interp->mFrameBuffers.find(fb);
        if (fb <= 0 || it == interp->mFrameBuffers.end()) {
            LOGE("HUD: CreateFrameBuffer failed (%d)", fb);
            return false;
        }
        gHud.fb[k] = fb;
        LOGI("HUD framebuffer %d for the %s (%ux%u for 320x240)", fb,
             k == PORT_XR_HUD_PANEL ? "panel"
             : k == PORT_XR_HUD_PLANE ? "plane"
             : k == PORT_XR_HUD_PARTY ? "party layer"
             : k == PORT_XR_HUD_STAGE ? "stage layer"
                                      : "world sprites",
             it->second.applied_width, it->second.applied_height);
    }
    gHud.fbFailed = false;
    return true;
}

// Keeps the UI framebuffers non-inverted (see the section comment). Fast3D
// creates them, and re-creates them on a resize, with invertY on; setting
// the same size again only rebinds. Between draws.
void KeepHudUpright() {
    const auto interp = HudInterpreter();
    if (!interp || interp->mRapi == nullptr) {
        return;
    }
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    for (int k = 0; k < kHudKinds; k++) {
        auto it = interp->mFrameBuffers.find(gHud.fb[k]);
        if (gHud.fb[k] >= 0 && it != interp->mFrameBuffers.end()) {
            interp->mRapi->UpdateFramebufferParameters(gHud.fb[k], it->second.applied_width,
                                                       it->second.applied_height, 1, false, true, true, true);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prev);
}

// A UI framebuffer's color texture and its current storage size, or 0.
GLuint HudTexture(int kind, int* w = nullptr, int* h = nullptr) {
    const auto interp = HudInterpreter();
    if (gHud.fb[kind] < 0 || !interp || interp->mRapi == nullptr) {
        return 0;
    }
    auto it = interp->mFrameBuffers.find(gHud.fb[kind]);
    if (it == interp->mFrameBuffers.end()) {
        return 0;
    }
    if (w != nullptr && h != nullptr) {
        *w = (int) it->second.applied_width;
        *h = (int) it->second.applied_height;
    }
    return (GLuint) (uintptr_t) interp->mRapi->GetFramebufferTextureId(gHud.fb[kind]);
}

void SetFb(Fast::F3DGfx* cmd, int fb) {
    cmd->words.w0 = (uintptr_t) 0x21 << 24; // G_SETFB
    cmd->words.w1 = (uintptr_t) fb;
}

void ResetFb(Fast::F3DGfx* cmd) {
    cmd->words.w0 = (uintptr_t) 0x22 << 24; // G_RESETFB
    cmd->words.w1 = 0;
}

void NoOp(Fast::F3DGfx* cmd) {
    cmd->words.w0 = cmd->words.w1 = 0; // G_NOOP
}

// Jumps to `to` in the same list (gSPBranchList: G_DL, no push).
void BranchTo(Fast::F3DGfx* cmd, Fast::F3DGfx* to) {
    cmd->words.w0 = ((uintptr_t) 0xDE << 24) | ((uintptr_t) 1 << 16);
    cmd->words.w1 = (uintptr_t) to;
}

// The brackets become switches to the UI framebuffers and back (redirect),
// or no-ops.
void WriteHudBrackets(bool redirect) {
    for (const HudRange& range : gHud.ranges) {
        if (redirect) {
            SetFb(range.begin, gHud.fb[range.kind]);
            if (range.parent >= 0) {
                SetFb(range.end, gHud.fb[range.parent]); // back to the outer range's layer
            } else {
                ResetFb(range.end);
            }
        } else {
            NoOp(range.begin);
            NoOp(range.end);
        }
    }
    // 3D inside a UI range: into the eye image and back to the range's framebuffer.
    for (const HudRange& scene : gHud.sceneRanges) {
        if (redirect && scene.kind >= 0) {
            ResetFb(scene.begin);
            SetFb(scene.end, gHud.fb[scene.kind]);
        } else {
            NoOp(scene.begin);
            NoOp(scene.end);
        }
    }
    // Screen transitions: skipped (the scene image fades instead, PatchHud).
    for (const FadeRange& fade : gHud.fades) {
        if (redirect) {
            BranchTo(fade.begin, fade.end);
        } else {
            NoOp(fade.begin);
        }
        NoOp(fade.end);
    }
    for (const WorldSprite& sprite : gHud.sprites) {
        if (redirect) {
            SetFb(sprite.begin, gHud.fb[kHudSpriteKind]); // into the sprite framebuffer...
            if (sprite.kind >= 0) {
                SetFb(sprite.end, gHud.fb[sprite.kind]); // ...and back to the range's framebuffer
            } else {
                ResetFb(sprite.end);
            }
        } else {
            NoOp(sprite.begin);
            NoOp(sprite.end);
        }
    }
}

// The UI layers come out the same for both eyes, so only the left eye's pass
// draws them: for the right eye every outermost UI range, and every world
// sprite outside one, branches past its commands (each framebuffer switch
// costs a full tile store/load on the Quest's GPU). In a range that holds 3D
// drawn back into the eye images (port_xr_note_scene_range) the right eye
// jumps from one such piece to the next: begin -> piece, piece end -> next
// piece (or the range's end).
void SkipHudForSecondEye() {
    for (const HudRange& range : gHud.ranges) {
        if (range.parent >= 0) {
            continue;
        }
        std::vector<const HudRange*> scenes;
        for (const HudRange& scene : gHud.sceneRanges) {
            if (scene.begin > range.begin && scene.end < range.end) {
                scenes.push_back(&scene);
            }
        }
        std::sort(scenes.begin(), scenes.end(), [](const HudRange* a, const HudRange* b) { return a->begin < b->begin; });
        Fast::F3DGfx* from = range.begin;
        for (const HudRange* scene : scenes) {
            BranchTo(from, scene->begin); // scene->begin stays G_RESETFB: into the eye image
            from = scene->end;
        }
        BranchTo(from, range.end);
        NoOp(range.end);
    }
    for (const WorldSprite& sprite : gHud.sprites) {
        if (sprite.kind < 0) {
            BranchTo(sprite.begin, sprite.end);
            NoOp(sprite.end);
        }
    }
}

// Puts the world sprites' commands back as the game built them.
void RestoreSprites() {
    for (const WorldSprite& sprite : gHud.sprites) {
        Fast::F3DGfx* cmd = sprite.begin + 1;
        for (const auto& words : sprite.saved) {
            cmd->words.w0 = words.first;
            cmd->words.w1 = words.second;
            ++cmd;
        }
    }
}

int SignExtend24(uintptr_t v) {
    return (int) ((uint32_t) (v & 0xFFFFFF) << 8) >> 8;
}

// Transforms a world sprite's texture and fill rectangles (as built): native x
// becomes px + f * (x - ax), likewise y, and the texture steps are divided
// by f. G_TEXRECT_WIDE (24-bit signed 10.2, then s/t and steps) is scaled;
// G_TEXRECT/FLIP (12-bit 10.2) is only moved; G_FILLRECT is moved and scaled.
void TransformSpriteRects(const WorldSprite& sprite, float ax, float ay, float px, float py, float f) {
    auto to4 = [&](int v4, float a, float p) { return (int) std::lround(4.0f * (p + f * (v4 / 4.0f - a))); };
    for (Fast::F3DGfx* cmd = sprite.begin + 1; cmd < sprite.end; ++cmd) {
        const uint32_t op = (uint32_t) ((cmd->words.w0 >> 24) & 0xFF);
        if (op == 0x37 && cmd + 2 < sprite.end) { // G_TEXRECT_WIDE: lrx, lry; ulx, uly; s, t, dsdx, dtdy
            Fast::F3DGfx* ul = cmd + 1;
            Fast::F3DGfx* tex = cmd + 2;
            auto put = [](uintptr_t& word, int v) { word = (word & ~(uintptr_t) 0xFFFFFF) | ((uint32_t) v & 0xFFFFFF); };
            put(cmd->words.w0, to4(SignExtend24(cmd->words.w0), ax, px));
            put(cmd->words.w1, to4(SignExtend24(cmd->words.w1), ay, py));
            put(ul->words.w0, to4(SignExtend24(ul->words.w0), ax, px));
            put(ul->words.w1, to4(SignExtend24(ul->words.w1), ay, py));
            const int dsdx = (int16_t) ((tex->words.w1 >> 16) & 0xFFFF);
            const int dtdy = (int16_t) (tex->words.w1 & 0xFFFF);
            const int sdx = std::clamp((int) std::lround(dsdx / f), -32768, 32767);
            const int tdy = std::clamp((int) std::lround(dtdy / f), -32768, 32767);
            tex->words.w1 = (tex->words.w1 & ~(uintptr_t) 0xFFFFFFFF) | ((uint32_t) (uint16_t) sdx << 16) |
                            (uint32_t) (uint16_t) tdy;
            cmd += 2;
        } else if (op == 0xE4 || op == 0xE5) { // G_TEXRECT(FLIP): w0 lrx, lry; w1 ulx, uly
            const int dx4 = (int) std::lround(4.0f * (px - ax)), dy4 = (int) std::lround(4.0f * (py - ay));
            auto move = [&](uintptr_t& word) {
                const int x = std::clamp((int) ((word >> 12) & 0xFFF) + dx4, 0, 0xFFF);
                const int y = std::clamp((int) (word & 0xFFF) + dy4, 0, 0xFFF);
                word = (word & ~(uintptr_t) 0xFFFFFF) | ((uintptr_t) x << 12) | (uintptr_t) y;
            };
            move(cmd->words.w0);
            move(cmd->words.w1);
            cmd += 2; // the two RDPHALF words
        } else if (op == 0xF6) { // G_FILLRECT: w0 lrx, lry; w1 ulx, uly (12-bit 10.2), e.g. HP bar strips
            auto place = [&](uintptr_t& word) {
                const int x = std::clamp(to4((int) ((word >> 12) & 0xFFF), ax, px), 0, 0xFFF);
                const int y = std::clamp(to4((int) (word & 0xFFF), ay, py), 0, 0xFFF);
                word = (word & ~(uintptr_t) 0xFFFFFF) | ((uintptr_t) x << 12) | (uintptr_t) y;
            };
            place(cmd->words.w0);
            place(cmd->words.w1);
        }
    }
}

// Where matrix m (clip = [world 1]*m) shows world point p, in native screen
// px (the coordinates texture rectangles use: Fast3D squeezes both alike).
bool NativeScreenPoint(const float m[4][4], const CameraNote& cam, const float p[3], float* px, float* py) {
    float clip[4];
    Project(m, p, clip);
    if (clip[3] <= 1e-6f) {
        return false; // behind the eye
    }
    const float squeeze = WidescreenSqueeze();
    const float vx0 = (cam.viewport[0] + cam.viewport[2]) / 320.0f - 1.0f;
    const float vxs = (cam.viewport[2] - cam.viewport[0]) / 320.0f;
    const float vy0 = 1.0f - (cam.viewport[1] + cam.viewport[3]) / 240.0f;
    const float vys = (cam.viewport[3] - cam.viewport[1]) / 240.0f;
    const float ndcX = vx0 + vxs * squeeze * clip[0] / clip[3];
    const float ndcY = vy0 + vys * clip[1] / clip[3];
    *px = 160.0f * (ndcX / squeeze + 1.0f);
    *py = 120.0f * (1.0f - ndcY);
    return true;
}

// Per eye: moves a world sprite to where eye matrix m (the anchor camera's
// replacement) shows its anchor, keeping the game's offset from it, scaled
// by how much bigger the eye sees the anchor's surroundings than the game
// camera (matrix game) does: 10 units of world up at the anchor, compared.
void ShiftSprite(const WorldSprite& sprite, const float game[4][4], const float m[4][4], const CameraNote& cam) {
    const float up[3] = { sprite.world[0], sprite.world[1] + 10.0f, sprite.world[2] };
    float px, py, ux, uy, gx, gy, gux, guy;
    if (!NativeScreenPoint(m, cam, sprite.world, &px, &py) || !NativeScreenPoint(m, cam, up, &ux, &uy) ||
        !NativeScreenPoint(game, cam, sprite.world, &gx, &gy) || !NativeScreenPoint(game, cam, up, &gux, &guy)) {
        return;
    }
    const float gameLen = std::hypot(gux - gx, guy - gy), eyeLen = std::hypot(ux - px, uy - py);
    const float extra = std::clamp(CVarGetFloat(kHudSpriteScaleCVar, 1.0f), 0.1f, 4.0f);
    const float f = std::clamp(gameLen > 1e-3f ? extra * eyeLen / gameLen : extra, 0.05f, 4.0f);
    TransformSpriteRects(sprite, (float) sprite.screen[0], (float) sprite.screen[1], px, py, f);
    if (--gHud.spriteLogCountdown <= 0) {
        gHud.spriteLogCountdown = kHudPlaneLogPresents;
        LOGI("World sprite: anchor %.0f %.0f %.0f, game screen %d,%d -> eye %.1f,%.1f, scale %.2f", sprite.world[0],
             sprite.world[1], sprite.world[2], sprite.screen[0], sprite.screen[1], px, py, f);
    }
}

// Clears the UI framebuffers to (0,0,0,0), keeping the GL state.
void ClearHudTextures() {
    if (gHud.clearFbo == 0) {
        glGenFramebuffers(1, &gHud.clearFbo);
    }
    GLint draw = 0;
    GLfloat clearColor[4] = {};
    GLboolean colorMask[4] = {};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    for (int k = 0; k < kHudKinds; k++) {
        const GLuint texture = HudTexture(k);
        if (texture == 0) {
            continue;
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gHud.clearFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    if (scissor) {
        glEnable(GL_SCISSOR_TEST);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint) draw);
}

// At the start of each stereo pair: room-view pairs draw the UI into the UI
// framebuffers (gXrHud on, brackets noted for this frame).
void PatchHud(bool room) {
    const bool redirect = room && CVarGetInteger(kHudCVar, 1) != 0 && !gHud.ranges.empty() && gHud.program != 0 &&
                          CreateHudFramebuffers();
    if (redirect) {
        KeepHudUpright();
    }
    WriteHudBrackets(redirect);
    float coverage = 0.0f;
    for (const FadeRange& fade : gHud.fades) {
        coverage = std::max(coverage, fade.coverage);
    }
    gXr.sceneOpacity = redirect ? 1.0f - std::clamp(coverage, 0.0f, 1.0f) : 1.0f;
    static bool sLoggedFade = false;
    if ((gXr.sceneOpacity < 1.0f) != sLoggedFade) {
        sLoggedFade = gXr.sceneOpacity < 1.0f;
        LOGI("Scene fade %s (opacity %.2f)", sLoggedFade ? "started" : "ended", gXr.sceneOpacity);
    }
    if (redirect != gHud.loggedRedirect) {
        gHud.loggedRedirect = redirect;
        LOGI("UI %s (%zu range(s))", redirect ? "on its layers" : "in the eye images", gHud.ranges.size());
    }
    gHud.redirect = redirect;
}

// The copy into the HUD swapchain: a UI framebuffer (uRect: offset and size
// in texture coordinates) over a black card of alpha uBacking, premultiplied.
constexpr const char* kHudCopyFs = R"(#version 300 es
precision highp float;
uniform sampler2D uHud;
uniform vec4 uRect;
uniform float uBacking;
in vec2 vPos;
out vec4 oColor;
void main() {
    vec4 c = texture(uHud, uRect.xy + vPos * uRect.zw);
    oColor = c + (1.0 - c.a) * vec4(0.0, 0.0, 0.0, uBacking);
}
)";

// Failure only costs the UI layers (the UI then stays in the eye images).
void CreateHudCopy() {
    const GLuint vs = CompileShader(GL_VERTEX_SHADER, kGameCopyVs);
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kHudCopyFs);
    if (vs != 0 && fs != 0) {
        GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        GLint ok = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok) {
            gHud.program = program;
            gHud.rectLoc = glGetUniformLocation(program, "uRect");
            gHud.backingLoc = glGetUniformLocation(program, "uBacking");
            GLint prevProgram = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
            glUseProgram(program);
            glUniform1i(glGetUniformLocation(program, "uHud"), 0);
            glUseProgram((GLuint) prevProgram);
            glGenVertexArrays(1, &gHud.vao);
            // Linear and clamped whatever Fast3D set on its textures: the copy
            // scales 1920x1080 down to the swapchain.
            glGenSamplers(1, &gHud.sampler);
            glSamplerParameteri(gHud.sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glSamplerParameteri(gHud.sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glSamplerParameteri(gHud.sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glSamplerParameteri(gHud.sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            LOGE("HUD copy program failed to link");
            glDeleteProgram(program);
        }
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
}

bool CreateHudSwapchain() {
    if (gHud.swapchain != XR_NULL_HANDLE || gHud.swapchainFailed) {
        return gHud.swapchain != XR_NULL_HANDLE;
    }
    gHud.swapchainFailed = true;
    XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = gXr.srgbWriteControl ? GL_SRGB8_ALPHA8 : GL_RGBA8; // as the main swapchain
    info.sampleCount = 1;
    info.width = kHudImageW;
    info.height = kHudKinds * kHudImageH;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!Ok(xrCreateSwapchain(gXr.session, &info, &gHud.swapchain), "xrCreateSwapchain (HUD)")) {
        gHud.swapchain = XR_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(gHud.swapchain, 0, &count, nullptr);
    gHud.images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR });
    if (count == 0 || !Ok(xrEnumerateSwapchainImages(gHud.swapchain, count, &count,
                                                     reinterpret_cast<XrSwapchainImageBaseHeader*>(gHud.images.data())),
                          "xrEnumerateSwapchainImages (HUD)")) {
        return false;
    }
    glGenFramebuffers(1, &gHud.drawFbo);
    gHud.swapchainFailed = false;
    LOGI("HUD swapchain %dx%d, %u images", kHudImageW, kHudKinds * kHudImageH, count);
    return true;
}

XrSwapchain HudSwapchain() {
    return gHud.swapchain;
}

// Diagnostic (gXrHudCoverage 1, read live), every ~5 s while the UI is on its layers: each UI framebuffer
// scaled down to 64x36, read back, and the share of pixels with alpha > 0
// logged with their bounding box (cells; row 0 = top of the screen if the
// framebuffer is upright, i.e. GL top row).
constexpr int kCoverageW = 64, kCoverageH = 36, kCoveragePresents = 150;

void LogHudCoverage() {
    static GLuint tex = 0, drawFbo = 0, readFbo = 0;
    static int countdown = 0;
    if (CVarGetInteger("gXrHudCoverage", 0) == 0 || --countdown > 0) {
        return;
    }
    countdown = kCoveragePresents;
    if (tex == 0) {
        GLint prevTex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kCoverageW, kCoverageH);
        glBindTexture(GL_TEXTURE_2D, (GLuint) prevTex);
        glGenFramebuffers(1, &drawFbo);
        glGenFramebuffers(1, &readFbo);
    }
    BlitState blit;
    GLint packAlign = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &packAlign);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    std::string line;
    for (int k = 0; k < kHudKinds; k++) {
        int w = 0, h = 0;
        const GLuint src = HudTexture(k, &w, &h);
        if (src == 0 || w <= 0 || h <= 0) {
            line += " | none";
            continue;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBlitFramebuffer(0, 0, w, h, 0, 0, kCoverageW, kCoverageH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, drawFbo);
        uint8_t px[kCoverageW * kCoverageH * 4];
        glReadPixels(0, 0, kCoverageW, kCoverageH, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int count = 0, x0 = kCoverageW, x1 = -1, y0 = kCoverageH, y1 = -1;
        for (int y = 0; y < kCoverageH; y++) {
            for (int x = 0; x < kCoverageW; x++) {
                if (px[(y * kCoverageW + x) * 4 + 3] > 8) {
                    const int row = kCoverageH - 1 - y; // GL rows are bottom-up
                    count++;
                    x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, row), y1 = std::max(y1, row);
                }
            }
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), " | %s %.1f%% cols %d-%d rows %d-%d",
                      k == PORT_XR_HUD_PANEL ? "panel"
                      : k == PORT_XR_HUD_PLANE ? "plane"
                      : k == PORT_XR_HUD_PARTY ? "party"
                      : k == PORT_XR_HUD_STAGE ? "stage"
                                               : "sprites",
                      100.0f * count / (kCoverageW * kCoverageH), x0, x1, y0, y1);
        line += buf;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, packAlign);
    LOGI("UI coverage%s", line.c_str());
}

// Copies the UI of the pair just drawn into the HUD swapchain (with the room
// image being released). False: no UI layers for this image.
bool CopyHudImage() {
    if (!gHud.redirect || !CreateHudSwapchain()) {
        return false;
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    if (!Ok(xrAcquireSwapchainImage(gHud.swapchain, &acquire, &index), "xrAcquireSwapchainImage (HUD)") ||
        !Ok(xrWaitSwapchainImage(gHud.swapchain, &wait), "xrWaitSwapchainImage (HUD)")) {
        return false;
    }
    {
        BlitState blit;
        DrawState state;
        const float backing = std::clamp(CVarGetFloat(kHudBackingCVar, kDefaultHudBacking), 0.0f, 1.0f);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gHud.drawFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gHud.images[index].image, 0);
        glUseProgram(gHud.program);
        glUniform4f(gHud.rectLoc, 0.0f, 0.0f, 1.0f, 1.0f);
        glUniform1f(gHud.backingLoc, backing);
        glBindVertexArray(gHud.vao);
        glActiveTexture(GL_TEXTURE0);
        glBindSampler(0, gHud.sampler);
        for (int k = 0; k < kHudKinds; k++) {
            glViewport(0, k * kHudImageH, kHudImageW, kHudImageH);
            const GLuint texture = HudTexture(k);
            if (texture == 0) {
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // scissor is off (BlitState)
                glClear(GL_COLOR_BUFFER_BIT);
                continue;
            }
            glBindTexture(GL_TEXTURE_2D, texture);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }
    LogHudCoverage();
    gHud.imagePlaneValid = gHud.planeValid;
    gHud.imagePlane = gHud.plane;
    gHud.imagePartyValid = gHud.partyValid;
    gHud.imageParty = gHud.party;
    gHud.imageStageValid = gHud.stageValid;
    gHud.imageStage = gHud.stage;
    gHud.imageSpriteQuads = gHud.spriteQuads;
    XrSwapchainImageReleaseInfo release{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    return Ok(xrReleaseSwapchainImage(gHud.swapchain, &release), "xrReleaseSwapchainImage (HUD)");
}

XrQuaternionf QuatMul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

// Turns a quad's +z (the side it shows) from `at` toward the LOCAL origin
// (the default head position), keeping it upright.
XrQuaternionf FacingOrigin(const XrVector3f& at) {
    const float len = std::max(std::sqrt(at.x * at.x + at.y * at.y + at.z * at.z), 1e-4f);
    const float nx = -at.x / len, ny = -at.y / len, nz = -at.z / len;
    const float yaw = std::atan2(nx, nz), pitch = -std::asin(std::clamp(ny, -1.0f, 1.0f));
    return QuatMul({ 0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw) },
                   { std::sin(0.5f * pitch), 0.0f, 0.0f, std::cos(0.5f * pitch) });
}

// The steady panel (see the section comment), read live.
HudPlane HudPanelPose() {
    const RoomParams room = ReadRoomParams();
    const float width = std::clamp(CVarGetFloat(kHudPanelWidthCVar, kDefaultHudPanelWidth), 0.1f, 4.0f);
    const float up = std::clamp(CVarGetFloat(kHudPanelUpCVar, kDefaultHudPanelUp), -2.0f, 2.0f);
    const float back = std::clamp(CVarGetFloat(kHudPanelBackCVar, kDefaultHudPanelBack), -2.0f, 4.0f);
    HudPlane panel;
    panel.width = width;
    panel.height = width / kRenderAspect;
    panel.pose.position = { 0.0f, room.height + up, -room.distance - back };
    panel.pose.orientation = FacingOrigin(panel.pose.position);
    return panel;
}

const HudPlane* HudImagePlane() {
    return gHud.imagePlaneValid ? &gHud.imagePlane : nullptr;
}

const HudPlane* HudImageParty() {
    return gHud.imagePartyValid ? &gHud.imageParty : nullptr;
}

const HudPlane* HudImageStage() {
    return gHud.imageStageValid ? &gHud.imageStage : nullptr;
}

const std::vector<SpriteQuad>& HudImageSprites() {
    return gHud.imageSpriteQuads;
}

// The bounding box of a world sprite's rectangles (as built), native px.
bool SpriteBounds(const WorldSprite& sprite, float* x0, float* y0, float* x1, float* y1) {
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    auto add = [&](float x, float y) {
        minX = std::min(minX, x), maxX = std::max(maxX, x);
        minY = std::min(minY, y), maxY = std::max(maxY, y);
    };
    auto add12 = [&](uintptr_t word) { add(((word >> 12) & 0xFFF) / 4.0f, (word & 0xFFF) / 4.0f); };
    for (const Fast::F3DGfx* cmd = sprite.begin + 1; cmd < sprite.end; ++cmd) {
        const uint32_t op = (uint32_t) ((cmd->words.w0 >> 24) & 0xFF);
        if (op == 0x37 && cmd + 2 < sprite.end) { // G_TEXRECT_WIDE: lrx, lry; ulx, uly; ...
            add(SignExtend24(cmd->words.w0) / 4.0f, SignExtend24(cmd->words.w1) / 4.0f);
            add(SignExtend24(cmd[1].words.w0) / 4.0f, SignExtend24(cmd[1].words.w1) / 4.0f);
            cmd += 2;
        } else if (op == 0xE4 || op == 0xE5) { // G_TEXRECT(FLIP), then two RDPHALF words
            add12(cmd->words.w0);
            add12(cmd->words.w1);
            cmd += 2;
        } else if (op == 0xF6) { // G_FILLRECT
            add12(cmd->words.w0);
            add12(cmd->words.w1);
        }
    }
    if (maxX <= minX || maxY <= minY) {
        return false;
    }
    *x0 = minX, *y0 = minY, *x1 = maxX, *y1 = maxY;
    return true;
}

// A world sprite's quad for the pair being drawn: the sprite's box of the
// sprite framebuffer (drawn where the game put it), at the size it has against
// the scene in the game (game matrix `game`), placed so its anchor's screen
// point sits on the anchor mapped into the room like the scene, upright and
// turned toward the head: head roll doesn't tilt it.
bool SpriteQuadFor(const WorldSprite& sprite, const float game[4][4], const CameraNote& cam, const XrVector3f& head,
                   SpriteQuad* out) {
    float x0, y0, x1, y1;
    const float fx = cam.at[0] - cam.eye[0], fz = cam.at[2] - cam.eye[2];
    if (fx * fx + fz * fz < 1e-6f || !SpriteBounds(sprite, &x0, &y0, &x1, &y1)) {
        return false;
    }
    // Meters per native px: 10 world units up at the anchor, as the game shows them.
    const float up10[3] = { sprite.world[0], sprite.world[1] + 10.0f, sprite.world[2] };
    float gx, gy, gux, guy;
    if (!NativeScreenPoint(game, cam, sprite.world, &gx, &gy) || !NativeScreenPoint(game, cam, up10, &gux, &guy)) {
        return false;
    }
    const float gameLen = std::hypot(gux - gx, guy - gy);
    if (gameLen < 1e-3f) {
        return false;
    }
    const RoomParams room = ReadRoomParams();
    const float extra = std::clamp(CVarGetFloat(kHudSpriteScaleCVar, 1.0f), 0.1f, 4.0f);
    const float mpp = extra * (10.0f / room.unitsPerMeter) / gameLen;
    // The box in the sprite third: x from the screen's left edge, GL rows
    // bottom-up with the top of the screen at the top (as the UI quads show it).
    const float left = 160.0f - 120.0f * kRenderAspect;
    const float ppx = kHudImageW / (240.0f * kRenderAspect), ppy = kHudImageH / 240.0f;
    const int px0 = std::clamp((int) std::floor((x0 - left) * ppx), 0, kHudImageW);
    const int px1 = std::clamp((int) std::ceil((x1 - left) * ppx), 0, kHudImageW);
    const int py0 = std::clamp((int) std::floor((240.0f - y1) * ppy), 0, kHudImageH);
    const int py1 = std::clamp((int) std::ceil((240.0f - y0) * ppy), 0, kHudImageH);
    if (px1 <= px0 || py1 <= py0) {
        return false;
    }
    out->rect = { { px0, kHudSpriteKind * kHudImageH + py0 }, { px1 - px0, py1 - py0 } };
    const float nx0 = left + px0 / ppx, nx1 = left + px1 / ppx;            // native, as rounded
    const float nyTop = 240.0f - py1 / ppy, nyBottom = 240.0f - py0 / ppy; // native y down
    const double world[3] = { sprite.world[0], sprite.world[1], sprite.world[2] };
    double r[3];
    WorldToRoom(world, cam, room, r);
    const XrVector3f anchor = { (float) r[0], (float) r[1], (float) r[2] };
    // Stage sprites (HP bars) face the stage's front like the battle menus, so
    // they stay put when the head moves; the others turn toward the head.
    const XrQuaternionf facing = sprite.stageFacing
                                     ? XrQuaternionf{ 0.0f, 0.0f, 0.0f, 1.0f }
                                     : FacingOrigin({ anchor.x - head.x, anchor.y - head.y, anchor.z - head.z });
    const float ax[3] = { 1.0f, 0.0f, 0.0f }, ay[3] = { 0.0f, 1.0f, 0.0f };
    float right[3], upAxis[3];
    Rotate(facing, ax, right);
    Rotate(facing, ay, upAxis);
    const float dx = (0.5f * (nx0 + nx1) - sprite.screen[0]) * mpp;
    const float dy = (sprite.screen[1] - 0.5f * (nyTop + nyBottom)) * mpp;
    out->plane.pose.orientation = facing;
    out->plane.pose.position = { anchor.x + dx * right[0] + dy * upAxis[0], anchor.y + dx * right[1] + dy * upAxis[1],
                                 anchor.z + dx * right[2] + dy * upAxis[2] };
    out->plane.width = (nx1 - nx0) * mpp;
    out->plane.height = (nyBottom - nyTop) * mpp;
    if (--gHud.spriteLogCountdown <= 0) {
        gHud.spriteLogCountdown = kHudPlaneLogPresents;
        LOGI("Sprite quad: anchor %.0f %.0f %.0f (screen %d,%d), box %.0f,%.0f-%.0f,%.0f px -> %.3f x %.3f m at "
             "%+.2f %+.2f %+.2f m",
             sprite.world[0], sprite.world[1], sprite.world[2], sprite.screen[0], sprite.screen[1], x0, y0, x1, y1,
             out->plane.width, out->plane.height, out->plane.pose.position.x, out->plane.pose.position.y,
             out->plane.pose.position.z);
    }
    return true;
}

// The party layer for the pair being drawn (see the section comment): its
// anchor mapped into the room like the scene (frame camera `cam`), moved by
// the offsets; the layer faces room +z (leaning back by the tilt), at a
// constant scale, with the anchor's screen point on that spot.
bool HudAnchoredLayer(const CameraNote& cam, const HudLayerAnchor& anchor, HudPlane* out, bool mirrored = false) {
    const float fx = cam.at[0] - cam.eye[0], fz = cam.at[2] - cam.eye[2];
    if (!anchor.valid || fx * fx + fz * fz < 1e-6f) {
        return false;
    }
    const RoomParams room = ReadRoomParams();
    const double world[3] = { anchor.world[0], anchor.world[1], anchor.world[2] };
    double r[3];
    WorldToRoom(world, cam, room, r);
    const float width = std::clamp(CVarGetFloat(kHudPartyWidthCVar, kDefaultHudPartyWidth), 0.1f, 4.0f);
    // The stage layer mirrors the party layer, with its own offset across
    // (tuned with the owner: the menus' 15 cm mirrored, plus 8 cm further in).
    const float offsetRight = mirrored
                                  ? std::clamp(CVarGetFloat(kHudStageRightCVar, kDefaultHudStageRight), -2.0f, 2.0f)
                                  : std::clamp(CVarGetFloat(kHudPartyRightCVar, kDefaultHudPartyRight), -2.0f, 2.0f);
    const float up = std::clamp(CVarGetFloat(kHudPartyUpCVar, kDefaultHudPartyUp), -2.0f, 2.0f);
    const float back = std::clamp(CVarGetFloat(kHudPartyBackCVar, kDefaultHudPartyBack), -2.0f, 2.0f);
    const float tilt = std::clamp(CVarGetFloat(kHudPartyTiltCVar, kDefaultHudPartyTilt), -60.0f, 60.0f) *
                       (float) M_PI / 180.0f;
    const XrVector3f spot = { (float) r[0] + offsetRight, (float) r[1] + up, (float) r[2] - back };
    // Leaning back = turning about +x by -tilt (the top goes away from the viewer).
    const XrQuaternionf facing = { std::sin(-0.5f * tilt), 0.0f, 0.0f, std::cos(-0.5f * tilt) };
    const float metersPerPx = width / (240.0f * kRenderAspect);
    // The screen center relative to the anchor's screen point (native y down).
    const float dx = (160.0f - anchor.screen[0]) * metersPerPx, dy = (anchor.screen[1] - 120.0f) * metersPerPx;
    const float ax[3] = { 1.0f, 0.0f, 0.0f }, ay[3] = { 0.0f, 1.0f, 0.0f };
    float right[3], upAxis[3];
    Rotate(facing, ax, right);
    Rotate(facing, ay, upAxis);
    out->pose.orientation = facing;
    out->pose.position = { spot.x + dx * right[0] + dy * upAxis[0], spot.y + dx * right[1] + dy * upAxis[1],
                           spot.z + dx * right[2] + dy * upAxis[2] };
    out->width = width;
    out->height = width / kRenderAspect;
    return true;
}

// The world point the camera (game matrix m: clip = [world 1]*m) shows at
// native screen pixel (px, py), at the depth of its look-at point. Solves
// clip.x = nx*w0, clip.y = ny*w0, clip.w = w0 for the point.
bool PicturePoint(const float m[4][4], const CameraNote& cam, double px, double py, double out[3]) {
    const double vcx = 0.5 * ((double) cam.viewport[0] + cam.viewport[2]);
    const double vhw = 0.5 * ((double) cam.viewport[2] - cam.viewport[0]);
    const double vcy = 0.5 * ((double) cam.viewport[1] + cam.viewport[3]);
    const double vhh = 0.5 * ((double) cam.viewport[3] - cam.viewport[1]);
    if (vhw <= 0.0 || vhh <= 0.0) {
        return false;
    }
    const double nx = (px - vcx) / vhw, ny = (vcy - py) / vhh;
    const double w0 = cam.at[0] * (double) m[0][3] + cam.at[1] * (double) m[1][3] + cam.at[2] * (double) m[2][3] +
                      m[3][3];
    if (w0 <= 0.0) {
        return false;
    }
    const int cols[3] = { 0, 1, 3 };
    const double target[3] = { nx * w0, ny * w0, w0 };
    double a[3][3], b[3];
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 3; i++) {
            a[r][i] = m[i][cols[r]];
        }
        b[r] = target[r] - m[3][cols[r]];
    }
    auto det3 = [](const double x[3][3]) {
        return x[0][0] * (x[1][1] * x[2][2] - x[1][2] * x[2][1]) - x[0][1] * (x[1][0] * x[2][2] - x[1][2] * x[2][0]) +
               x[0][2] * (x[1][0] * x[2][1] - x[1][1] * x[2][0]);
    };
    const double det = det3(a);
    if (std::fabs(det) < 1e-12) {
        return false;
    }
    for (int i = 0; i < 3; i++) { // Cramer's rule
        double ai[3][3];
        std::memcpy(ai, a, sizeof(ai));
        for (int r = 0; r < 3; r++) {
            ai[r][i] = b[r];
        }
        out[i] = det3(ai) / det;
    }
    return true;
}

// The messages' quad for the pair being drawn: the frame camera's picture
// plane (see the section comment), mapped into the room like the scene. The
// quad shows the whole 16:9 screen (native x 160 +- 120 * aspect).
bool HudPlaneFromCamera(const float m[4][4], const CameraNote& cam) {
    const float fx = cam.at[0] - cam.eye[0], fz = cam.at[2] - cam.eye[2];
    if (fx * fx + fz * fz < 1e-6f) {
        return false;
    }
    const RoomParams room = ReadRoomParams();
    double wc[3], wr[3], wu[3];
    if (!PicturePoint(m, cam, 160.0, 120.0, wc) || !PicturePoint(m, cam, 160.0 + 120.0 * kRenderAspect, 120.0, wr) ||
        !PicturePoint(m, cam, 160.0, 0.0, wu)) {
        return false;
    }
    double c[3], r[3], u[3];
    WorldToRoom(wc, cam, room, c);
    WorldToRoom(wr, cam, room, r);
    WorldToRoom(wu, cam, room, u);
    double x[3] = { r[0] - c[0], r[1] - c[1], r[2] - c[2] };
    double y[3] = { u[0] - c[0], u[1] - c[1], u[2] - c[2] };
    const double halfW = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (halfW < 1e-6) {
        return false;
    }
    for (double& v : x) {
        v /= halfW;
    }
    const double dot = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    for (int i = 0; i < 3; i++) {
        y[i] -= dot * x[i];
    }
    const double halfH = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (halfH < 1e-6) {
        return false;
    }
    for (double& v : y) {
        v /= halfH;
    }
    // z = x cross y: toward the camera, the side the quad faces.
    const double z[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
    // Rotation matrix (columns x, y, z) -> quaternion.
    const double trace = x[0] + y[1] + z[2];
    double qw, qx, qy, qz;
    if (trace > 0.0) {
        const double s = 2.0 * std::sqrt(trace + 1.0);
        qw = 0.25 * s;
        qx = (y[2] - z[1]) / s;
        qy = (z[0] - x[2]) / s;
        qz = (x[1] - y[0]) / s;
    } else if (x[0] > y[1] && x[0] > z[2]) {
        const double s = 2.0 * std::sqrt(1.0 + x[0] - y[1] - z[2]);
        qw = (y[2] - z[1]) / s;
        qx = 0.25 * s;
        qy = (y[0] + x[1]) / s;
        qz = (z[0] + x[2]) / s;
    } else if (y[1] > z[2]) {
        const double s = 2.0 * std::sqrt(1.0 + y[1] - x[0] - z[2]);
        qw = (z[0] - x[2]) / s;
        qx = (y[0] + x[1]) / s;
        qy = 0.25 * s;
        qz = (z[1] + y[2]) / s;
    } else {
        const double s = 2.0 * std::sqrt(1.0 + z[2] - x[0] - y[1]);
        qw = (x[1] - y[0]) / s;
        qx = (z[0] + x[2]) / s;
        qy = (z[1] + y[2]) / s;
        qz = 0.25 * s;
    }
    const double scale = std::clamp(CVarGetFloat(kHudScaleCVar, 1.0f), 0.05f, 2.0f);
    double anchorWorld[3] = { cam.at[0], cam.at[1], cam.at[2] }, a[3];
    WorldToRoom(anchorWorld, cam, room, a);
    HudPlane& plane = gHud.plane;
    plane.pose.orientation = { (float) qx, (float) qy, (float) qz, (float) qw };
    plane.pose.position = { (float) (a[0] + scale * (c[0] - a[0])), (float) (a[1] + scale * (c[1] - a[1])),
                            (float) (a[2] + scale * (c[2] - a[2])) };
    plane.width = (float) (2.0 * halfW * scale);
    plane.height = (float) (2.0 * halfH * scale);
    if (--gHud.logCountdown <= 0) {
        gHud.logCountdown = kHudPlaneLogPresents;
        LOGI("HUD plane: center %+.2f %+.2f %+.2f m, %.2f x %.2f m (scale %.2f), normal %+.2f %+.2f %+.2f, skew %.3f, "
             "viewport %.0f,%.0f-%.0f,%.0f",
             plane.pose.position.x, plane.pose.position.y, plane.pose.position.z, plane.width, plane.height, scale, z[0], z[1], z[2], dot / halfH, cam.viewport[0],
             cam.viewport[1], cam.viewport[2], cam.viewport[3]);
    }
    return true;
}

// Decides, at the start of each stereo pair, whether it is drawn as a room
// view, and samples the eye poses for the refresh the pair is due on.
void BeginStereoPair() {
    bool room = false;
    if (gXr.cutoutDraw && CVarGetInteger(kRoomViewCVar, 1) != 0 && gXr.running) {
        XrTime time = gXr.nextPresentTime;
        if (time == 0 && gXr.lastDisplayTime != 0) {
            time = gXr.lastDisplayTime + gXr.displayPeriod;
        }
        room = time != 0 && SampleRoomViews(time);
    }
    if (room != gXr.loggedRoom) {
        gXr.loggedRoom = room;
        LOGI("Room view %s", room ? "on" : "off");
    }
    gXr.pairRoom = room;
    gRoomViewActive = room;
    PatchHud(room);
}

bool Initialize() {
    LOGI("Initializing OpenXR");
    if (!CreateInstance() || !CreateSession() || !CreateSwapchain() || !CreateInput()) {
        return false;
    }
    CreateGameCopy();
    CreateHudCopy();
    if (!AttachPad()) {
        LOGE("Continuing without Touch controller input");
    }
    LOGI("OpenXR ready");
    return true;
}

} // namespace

extern "C" void port_xr_note_camera(const void* mtx, const float eye[3], const float at[3], const short vpScale[4],
                                    const short vpTrans[4], int camID) {
    CameraNote note;
    std::memcpy(note.eye, eye, sizeof(note.eye));
    std::memcpy(note.at, at, sizeof(note.at));
    // Vp: scale and translate in quarter pixels (as Interpreter::CalcAndSetViewport).
    const float halfW = std::fabs((float) vpScale[0]) / 4.0f, halfH = std::fabs((float) vpScale[1]) / 4.0f;
    const float cx = vpTrans[0] / 4.0f, cy = vpTrans[1] / 4.0f;
    note.viewport[0] = cx - halfW;
    note.viewport[1] = cy - halfH;
    note.viewport[2] = cx + halfW;
    note.viewport[3] = cy + halfH;
    std::lock_guard<std::mutex> lock(gCameraMutex);
    // The previous tick of the same camera (the display contexts alternate, so
    // its matrix is another Mtx): what interpolated presents start from.
    static std::unordered_map<int, CameraNote> sLastById;
    if (auto last = sLastById.find(camID); last != sLastById.end()) {
        note.hasPrev = true;
        std::memcpy(note.prevEye, last->second.eye, sizeof(note.prevEye));
        std::memcpy(note.prevAt, last->second.at, sizeof(note.prevAt));
    }
    sLastById[camID] = note;
    gCameras[mtx] = note;
}

extern "C" int port_xr_background_cutout(const void* list, int candidate) {
    const bool cutout = candidate != 0 && gCutoutAvailable && CVarGetInteger(kDioramaCVar, 1) != 0;
    std::lock_guard<std::mutex> lock(gBackgroundMutex);
    gBackgroundCutout[list] = cutout;
    return cutout ? 1 : 0;
}

extern "C" void port_xr_frame_background(const void* list) {
    bool cutout = false;
    if (list != nullptr) {
        std::lock_guard<std::mutex> lock(gBackgroundMutex);
        auto it = gBackgroundCutout.find(list);
        cutout = it != gBackgroundCutout.end() && it->second;
    }
    if (cutout != gXr.cutoutDraw) {
        LOGI("Diorama cut-out %s", cutout ? "on" : "off");
    }
    gXr.cutoutDraw = cutout;
}

extern "C" void port_xr_hud_frame_start(const void* list, const void* camMtx) {
    std::lock_guard<std::mutex> lock(gHudMutex);
    gHudNotes[list].clear();
    gSpriteNotes[list].clear();
    gFadeNotes[list].clear();
    gSceneNotes[list].clear();
    gLayerAnchorNotes[list].clear();
    gHudCameras[list] = camMtx;
    ++gHudBuild;
}

extern "C" void port_xr_note_hud_anchor(const void* list, int kind, const float world[3], int screenX,
                                        int screenY) {
    if (kind < 0 || kind >= kHudKinds) {
        return;
    }
    HudLayerAnchor anchor;
    anchor.valid = true;
    std::memcpy(anchor.world, world, sizeof(anchor.world));
    anchor.screen[0] = screenX;
    anchor.screen[1] = screenY;
    std::lock_guard<std::mutex> lock(gHudMutex);
    gLayerAnchorNotes[list].emplace_back(kind, anchor);
}

extern "C" void port_xr_note_hud(const void* list, void* begin, void* end, int kind) {
    if (begin == nullptr || end <= begin || kind < 0 || kind >= kHudKinds) {
        return;
    }
    std::lock_guard<std::mutex> lock(gHudMutex);
    gHudNotes[list].push_back({ static_cast<Fast::F3DGfx*>(begin), static_cast<Fast::F3DGfx*>(end), kind });
}

extern "C" void port_xr_anchor_hud_element(const void* elem, int camID, float x, float y, float z, int screenX,
                                           int screenY) {
    std::lock_guard<std::mutex> lock(gHudMutex);
    gHudAnchors[elem] = { camID, { x, y, z }, { screenX, screenY }, gHudBuild, false };
}

extern "C" void port_xr_anchor_stage_sprite(const void* elem, int camID, float x, float y, float z, int screenX,
                                            int screenY) {
    std::lock_guard<std::mutex> lock(gHudMutex);
    gHudAnchors[elem] = { camID, { x, y, z }, { screenX, screenY }, gHudBuild, true };
}

extern "C" int port_xr_hud_element_camera(const void* elem) {
    std::lock_guard<std::mutex> lock(gHudMutex);
    auto it = gHudAnchors.find(elem);
    return it != gHudAnchors.end() && gHudBuild - it->second.stamp <= 2 ? it->second.cam : -1;
}

extern "C" void port_xr_note_scene_range(const void* list, void* begin, void* end) {
    if (begin == nullptr || end <= begin) {
        return;
    }
    std::lock_guard<std::mutex> lock(gHudMutex);
    gSceneNotes[list].push_back({ static_cast<Fast::F3DGfx*>(begin), static_cast<Fast::F3DGfx*>(end), -1 });
}

extern "C" void port_xr_note_scene_fade(const void* list, void* begin, void* end, float coverage) {
    if (begin == nullptr || end <= begin) {
        return;
    }
    std::lock_guard<std::mutex> lock(gHudMutex);
    gFadeNotes[list].push_back(
        { static_cast<Fast::F3DGfx*>(begin), static_cast<Fast::F3DGfx*>(end), std::clamp(coverage, 0.0f, 1.0f) });
}

extern "C" void port_xr_note_world_sprite(const void* list, void* begin, void* end, const void* elem,
                                          const void* mtx) {
    if (begin == nullptr || end <= begin) {
        return;
    }
    std::lock_guard<std::mutex> lock(gHudMutex);
    auto it = gHudAnchors.find(elem);
    if (it == gHudAnchors.end()) {
        return;
    }
    WorldSprite sprite;
    sprite.begin = static_cast<Fast::F3DGfx*>(begin);
    sprite.end = static_cast<Fast::F3DGfx*>(end);
    sprite.mtx = mtx;
    std::memcpy(sprite.world, it->second.world, sizeof(sprite.world));
    sprite.screen[0] = it->second.screen[0];
    sprite.screen[1] = it->second.screen[1];
    sprite.stageFacing = it->second.stageFacing;
    gSpriteNotes[list].push_back(std::move(sprite));
}

extern "C" void port_xr_frame_main(const void* list) {
    std::lock_guard<std::mutex> lock(gHudMutex);
    auto it = gHudNotes.find(list);
    if (it != gHudNotes.end()) {
        gHud.ranges = it->second;
    } else {
        gHud.ranges.clear();
    }
    auto cam = gHudCameras.find(list);
    gHud.camMtx = cam != gHudCameras.end() ? cam->second : nullptr;
    auto scenes = gSceneNotes.find(list);
    gHud.sceneRanges.clear();
    if (scenes != gSceneNotes.end()) {
        gHud.sceneRanges = scenes->second;
    }
    auto fades = gFadeNotes.find(list);
    gHud.fades.clear();
    if (fades != gFadeNotes.end()) {
        gHud.fades = fades->second;
    }
    auto sprites = gSpriteNotes.find(list);
    gHud.sprites.clear();
    if (sprites != gSpriteNotes.end()) {
        gHud.sprites = sprites->second;
    }
    for (HudLayerAnchor& anchor : gHud.anchors) {
        anchor.valid = false;
    }
    if (auto anchors = gLayerAnchorNotes.find(list); anchors != gLayerAnchorNotes.end()) {
        for (const auto& [kind, anchor] : anchors->second) {
            gHud.anchors[kind] = anchor;
        }
    }
    // The innermost UI range around [begin, end], or null.
    auto enclosing = [](const Fast::F3DGfx* begin, const Fast::F3DGfx* end, const HudRange* self) {
        const HudRange* best = nullptr;
        for (const HudRange& range : gHud.ranges) {
            if (&range != self && begin > range.begin && end < range.end && (!best || range.begin > best->begin)) {
                best = &range;
            }
        }
        return best;
    };
    for (HudRange& range : gHud.ranges) {
        const HudRange* outer = enclosing(range.begin, range.end, &range);
        range.parent = outer ? outer->kind : -1;
    }
    // Keep each sprite's commands as built (they are moved per eye), and
    // find the UI range it sits in.
    for (WorldSprite& sprite : gHud.sprites) {
        sprite.saved.clear();
        for (Fast::F3DGfx* cmd = sprite.begin + 1; cmd < sprite.end; ++cmd) {
            sprite.saved.emplace_back(cmd->words.w0, cmd->words.w1);
        }
        const HudRange* outer = enclosing(sprite.begin, sprite.end, nullptr);
        sprite.kind = outer ? outer->kind : -1;
    }
    for (HudRange& scene : gHud.sceneRanges) {
        const HudRange* outer = enclosing(scene.begin, scene.end, nullptr);
        scene.kind = outer ? outer->kind : -1;
    }
}

extern "C" int port_xr_room_view_active(void) {
    return gRoomViewActive ? 1 : 0;
}

extern "C" void port_xr_set_present_fraction(float fraction) {
    gXr.presentFraction = fraction;
}

extern "C" void port_xr_set_present_rate(int fps) {
    gXr.presentRate = fps;
}

extern "C" int port_xr_paces_frames(void) {
    return gXr.mode == Mode::Active && gXr.running && RefreshesPerPresent() > 0;
}

extern "C" void port_xr_set_eye(int eye) {
    gXr.eye = eye;
    if (eye == 0) {
        BeginStereoPair();
    }
    if (eye == 0 && gHud.redirect) {
        ClearHudTextures();
    }
    if (eye == 1 && gHud.redirect) {
        SkipHudForSecondEye();
    }
    if (eye < 0) {
        // Pair done: later draws of this list (flat presents) draw the UI as built.
        if (gHud.redirect) {
            WriteHudBrackets(false);
            gHud.redirect = false;
        }
        gXr.sceneOpacity = 1.0f;
        RestoreSprites();
    }
}

extern "C" int port_xr_stereo_active(void) {
    return gXr.mode == Mode::Active && gXr.running && CVarGetInteger(kStereoCVar, 1) != 0;
}

// The camera as of this present: the noted eye/at are the game tick's (30 Hz),
// but between ticks the matrix is interpolated, and the room view must move
// with it or the follow-camera mapping steps while interpolated objects glide
// (Mario judders against the room at 120 fps). From matrix m (clip = [world 1]*m,
// view x perspective): the eye is the point with clip x = y = w = 0, the view
// direction the gradient of w; `at` lies along it at the focus distance
// interpolated between the previous tick and this one (the battle camera zooms
// by changing it: a stepped distance pumped the stage in and out).
bool CameraFromMatrix(const float m[4][4], const CameraNote& noted, CameraNote* out) {
    const int cols[3] = { 0, 1, 3 };
    double a[3][3], b[3];
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 3; i++) {
            a[r][i] = m[i][cols[r]];
        }
        b[r] = -(double) m[3][cols[r]];
    }
    auto det3 = [](const double x[3][3]) {
        return x[0][0] * (x[1][1] * x[2][2] - x[1][2] * x[2][1]) - x[0][1] * (x[1][0] * x[2][2] - x[1][2] * x[2][0]) +
               x[0][2] * (x[1][0] * x[2][1] - x[1][1] * x[2][0]);
    };
    const double det = det3(a);
    if (std::fabs(det) < 1e-12) {
        return false;
    }
    double eye[3];
    for (int i = 0; i < 3; i++) { // Cramer's rule
        double ai[3][3];
        std::memcpy(ai, a, sizeof(ai));
        for (int r = 0; r < 3; r++) {
            ai[r][i] = b[r];
        }
        eye[i] = det3(ai) / det;
    }
    const double f[3] = { m[0][3], m[1][3], m[2][3] };
    const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    const double distNow = Distance(noted.eye, noted.at);
    const double distPrev = noted.hasPrev ? Distance(noted.prevEye, noted.prevAt) : distNow;
    const double t = std::clamp((double) gXr.presentFraction, 0.0, 1.0);
    const double dist = distPrev + t * (distNow - distPrev);
    if (fl < 1e-12 || dist <= 0.0) {
        return false;
    }
    // Sanity: an interpolated camera stays near the tick's (else keep the note).
    const double dx = eye[0] - noted.eye[0], dy = eye[1] - noted.eye[1], dz = eye[2] - noted.eye[2];
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > std::max(distNow, distPrev)) {
        return false;
    }
    *out = noted;
    for (int i = 0; i < 3; i++) {
        out->eye[i] = (float) eye[i];
        out->at[i] = (float) (eye[i] + dist * f[i] / fl);
    }
    return true;
}

const std::unordered_map<Mtx*, MtxF>& port_xr_eye_replacements(const std::unordered_map<Mtx*, MtxF>& base, int eye) {
    gXr.eyeReplacements = base;
    const StereoParams params = ReadStereoParams();
    const bool room = gXr.pairRoom;
    int& countdown = room ? gXr.roomCheckCountdown : gXr.stereoCheckCountdown;
    const bool check = eye == 1 && --countdown <= 0;
    if (check) {
        countdown = room ? kRoomCheckPresents : kStereoCheckPresents;
    }

    if (eye == 0) {
        gHud.planeValid = false;
        gHud.partyValid = false;
        gHud.stageValid = false;
        gHud.spriteQuads.clear();
    }
    const XrVector3f head = { 0.5f * (gXr.pairViews[0].pose.position.x + gXr.pairViews[1].pose.position.x),
                              0.5f * (gXr.pairViews[0].pose.position.y + gXr.pairViews[1].pose.position.y),
                              0.5f * (gXr.pairViews[0].pose.position.z + gXr.pairViews[1].pose.position.z) };
    RestoreSprites(); // this eye moves them from where the game put them
    std::lock_guard<std::mutex> lock(gCameraMutex);
    for (const auto& [key, noted] : gCameras) {
        Mtx* mtx = const_cast<Mtx*>(static_cast<const Mtx*>(key));
        // Shift the matrix this present would otherwise show: the
        // interpolated value when there is one, else game memory.
        float source[4][4];
        if (auto lerped = base.find(mtx); lerped != base.end()) {
            std::memcpy(source, lerped->second.mf, sizeof(source));
        } else {
            DecodeMtx(mtx, source);
        }
        CameraNote cam = noted;
        if (room) {
            CameraFromMatrix(source, noted, &cam); // follow the interpolated camera
        }
        if (room && eye == 0 && key == gHud.camMtx) {
            gHud.planeValid = HudPlaneFromCamera(source, cam);
            gHud.partyValid = HudAnchoredLayer(cam, gHud.anchors[PORT_XR_HUD_PARTY], &gHud.party);
            gHud.stageValid = HudAnchoredLayer(cam, gHud.anchors[PORT_XR_HUD_STAGE], &gHud.stage, true);
        }
        MtxF shifted;
        if (room) {
            if (RoomEyeMatrix(source, cam, eye, shifted.mf)) {
                gXr.eyeReplacements[mtx] = shifted;
                for (const WorldSprite& sprite : gHud.sprites) {
                    if (sprite.mtx != key) {
                        continue;
                    }
                    if (!gHud.redirect) {
                        ShiftSprite(sprite, source, shifted.mf, cam); // in the eye images
                    } else if (eye == 0 && gHud.spriteQuads.size() < (size_t) kMaxSpriteQuads) {
                        SpriteQuad quad;
                        if (SpriteQuadFor(sprite, source, cam, head, &quad)) {
                            gHud.spriteQuads.push_back(quad);
                        }
                    }
                }
                if (check) {
                    RoomCheck(source, cam);
                }
            }
        } else if (ShiftForEye(source, Distance(cam.eye, cam.at), eye, params, shifted.mf)) {
            gXr.eyeReplacements[mtx] = shifted;
            if (check) {
                StereoCheck(source, cam);
            }
        }
    }
    return gXr.eyeReplacements;
}

// libultraship's SDL backend calls SDL_GL_SwapWindow once per presented
// frame; the Quest link redirects it here (-Wl,--wrap=SDL_GL_SwapWindow).
extern "C" void __wrap_SDL_GL_SwapWindow(SDL_Window* window) {
    if (gXr.mode == Mode::Untried) {
        gXr.mode = Initialize() ? Mode::Active : Mode::Disabled;
        if (gXr.mode == Mode::Disabled) {
            LOGE("OpenXR unavailable; presenting through the flat SDL surface");
        }
    }
    if (gXr.mode != Mode::Active) {
        __real_SDL_GL_SwapWindow(window);
        return;
    }
    if (gXr.eye == 0) {
        // Left eye of a stereo pair: the right eye's swap submits both.
        if (gXr.running) {
            StashLeftEye(window);
        }
        return;
    }

    PollEvents();
    if (gXr.running) {
        SubmitFrame(window);
    } else {
        // Session not running (headset asleep, system UI): there is no frame
        // to wait on, so throttle the game loop instead of spinning.
        SDL_Delay(10);
    }
}

#endif // __ANDROID__ && PAPERBOAT_QUEST_XR
