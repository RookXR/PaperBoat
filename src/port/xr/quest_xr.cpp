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

#include <libultraship/bridge/consolevariablebridge.h>

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
#include <cmath>
#include <cstdint>
#include <cstring>
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

enum class Mode { Untried, Active, Disabled };

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
};

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
    // Runtimes report rates such as 119.99, so compare with a tolerance.
    for (float rate : rates) {
        if (std::fabs(rate - 120.0f) < 0.5f) {
            if (Ok(requestRate(gXr.session, rate), "xrRequestDisplayRefreshRateFB")) {
                LOGI("Display refresh rate %.2f Hz requested", rate);
            }
            return;
        }
    }
    LOGI("120 Hz unavailable; keeping runtime default refresh rate");
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

// The left eye's swap: hold its image until the right eye's swap, so both
// halves of a swapchain image always come from the same frame (repeat XR
// frames re-show the last released image).
void StashLeftEye(SDL_Window* window) {
    int srcW = 0, srcH = 0;
    GetDrawableSize(window, &srcW, &srcH);
    BlitState state;
    BlitEye(0, srcW, srcH, gXr.stashFbo, 0);
    gXr.stashValid = true;
}

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
            BlitEye(0, srcW, srcH, gXr.blitFbo, gXr.width);
        } else {
            BlitEye(0, srcW, srcH, gXr.blitFbo, 0);
        }
    }
    gXr.stashValid = false;

    XrSwapchainImageReleaseInfo release{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (!Ok(xrReleaseSwapchainImage(gXr.swapchain, &release), "xrReleaseSwapchainImage")) {
        return false;
    }
    gXr.imageStereo = stereo;
    return true;
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

void EndFrame(const XrFrameState& frame, bool showScreen) {
    XrCompositionLayerQuad quads[2] = { { XR_TYPE_COMPOSITION_LAYER_QUAD }, { XR_TYPE_COMPOSITION_LAYER_QUAD } };
    const XrCompositionLayerBaseHeader* layers[] = { reinterpret_cast<XrCompositionLayerBaseHeader*>(&quads[0]),
                                                     reinterpret_cast<XrCompositionLayerBaseHeader*>(&quads[1]) };
    XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = frame.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (showScreen) {
        // The quads show the most recently released swapchain image: one per
        // eye at the same pose for a stereo pair, else the left half to both.
        const int count = gXr.imageStereo ? 2 : 1;
        for (int i = 0; i < count; ++i) {
            XrCompositionLayerQuad& quad = quads[i];
            quad.space = gXr.local;
            quad.eyeVisibility = !gXr.imageStereo ? XR_EYE_VISIBILITY_BOTH
                                                  : (i == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT);
            quad.subImage.swapchain = gXr.swapchain;
            quad.subImage.imageRect = { { i * gXr.width, 0 }, { gXr.width, gXr.height } };
            quad.pose.orientation.w = 1.0f;
            quad.pose.position = { 0.0f, 0.0f, -kScreenDistance };
            quad.size = { kScreenHeight * (float) gXr.width / (float) gXr.height, kScreenHeight };
        }
        endInfo.layerCount = (uint32_t) count;
        endInfo.layers = layers;
    }
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

bool Initialize() {
    LOGI("Initializing OpenXR");
    if (!CreateInstance() || !CreateSession() || !CreateSwapchain() || !CreateInput()) {
        return false;
    }
    if (!AttachPad()) {
        LOGE("Continuing without Touch controller input");
    }
    LOGI("OpenXR ready");
    return true;
}

} // namespace

extern "C" void port_xr_note_camera(const void* mtx, const float eye[3], const float at[3]) {
    CameraNote note;
    std::memcpy(note.eye, eye, sizeof(note.eye));
    std::memcpy(note.at, at, sizeof(note.at));
    std::lock_guard<std::mutex> lock(gCameraMutex);
    gCameras[mtx] = note;
}

extern "C" void port_xr_set_present_rate(int fps) {
    gXr.presentRate = fps;
}

extern "C" int port_xr_paces_frames(void) {
    return gXr.mode == Mode::Active && gXr.running && RefreshesPerPresent() > 0;
}

extern "C" void port_xr_set_eye(int eye) {
    gXr.eye = eye;
}

extern "C" int port_xr_stereo_active(void) {
    return gXr.mode == Mode::Active && gXr.running && CVarGetInteger(kStereoCVar, 1) != 0;
}

const std::unordered_map<Mtx*, MtxF>& port_xr_eye_replacements(const std::unordered_map<Mtx*, MtxF>& base, int eye) {
    gXr.eyeReplacements = base;
    const StereoParams params = ReadStereoParams();
    const bool check = eye == 1 && --gXr.stereoCheckCountdown <= 0;
    if (check) {
        gXr.stereoCheckCountdown = kStereoCheckPresents;
    }

    std::lock_guard<std::mutex> lock(gCameraMutex);
    for (const auto& [key, cam] : gCameras) {
        Mtx* mtx = const_cast<Mtx*>(static_cast<const Mtx*>(key));
        // Shift the matrix this present would otherwise show: the
        // interpolated value when there is one, else game memory.
        float source[4][4];
        if (auto lerped = base.find(mtx); lerped != base.end()) {
            std::memcpy(source, lerped->second.mf, sizeof(source));
        } else {
            DecodeMtx(mtx, source);
        }
        MtxF shifted;
        if (ShiftForEye(source, Distance(cam.eye, cam.at), eye, params, shifted.mf)) {
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
