#include "android_quest.h"
#include "openxr_pcvr_math.h"
#include "recomp.h"
#include "goemon_vr_camera.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/system_properties.h>

#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>

#include "plume_vulkan.h"

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "hle/rt64_application.h"
#include "rhi/rt64_render_hooks.h"

namespace {

constexpr float kUnitsPerMeter = 100.0f;
constexpr float kSnapDegrees = 45.0f;
constexpr float kSnapPress = 0.75f;
constexpr float kSnapRecenter = 0.30f;
constexpr float kTriggerPress = 0.60f;
constexpr float kTriggerRelease = 0.40f;
// Eye image size if the runtime doesn't report one (Quest 3 panel, one eye).
// The dual-eye surface (about 4128x2208) stalls the compositor; this is one image for both eyes.
constexpr uint32_t kFallbackEyeWidth = 2064;
constexpr uint32_t kFallbackEyeHeight = 2208;
// Level resolution, relative to the headset's own recommended per-eye image.
// About 1.3 of this only makes up for the picture covering both eyes' view
// plus overscan; the rest is supersampling. Capped at what the runtime allows.
// 2.0 (up to 4096 x 4096) was too much: the Quest bogged down and levels
// never appeared. Lower it further (1.3, then 1.2) if levels still stutter.
// 1.6 (Smash 0.4.6: 2688x2816 at 60 a second with 2x MSAA, GPU ~55%).
constexpr float kResolutionScale = 1.6f;
// While RT64 draws with the ubershader after a GPU hang (it happens while the
// Quest records video, see GetGpuHangFallback), it renders at this share of the
// normal resolution so the slower shader keeps up.
constexpr double kHangFallbackResolution = 0.5;
// It also runs below 30 fps then (the recorder uses the GPU too), so the
// compositor turns each picture further: draw a wider margin meanwhile.
constexpr float kOverscanFallbackDeg = 18.0f;
constexpr uint32_t kMaxEyeSide = 3584;
// Hard ceiling on pixels per frame, whatever the headset recommends:
// about 3000 x 3000.
constexpr double kMaxEyePixels = 9.0e6;
// "Smooth motion" on the ROM window: in levels RT64 draws an in-between
// picture for every game frame, 60 pictures a second instead of the game's 30.
// Each one costs a whole frame of GPU work, so the picture gets about 2/3 of
// the sharp picture's pixels (2520x2640 -> about 2048x2144).
constexpr int kSmoothFps = 60;
// Owner on 0.1.14 (2x MSAA): "still very jaggy". The middle of the view had only
// ~7.6 picture pixels a degree (2048 wide over 134 degrees); the headset's own
// recommended image gives ~12. 8.0e6 (Smash's) allows 2688x2816.
constexpr double kMaxEyePixelsSmooth = 8.0e6;
// Menus keep the old 2208-line render. The floating screen covers a small
// part of your view, so more lines there would only add shimmer.
constexpr double kScreenResolutionMultiplier = 2208.0 / 240.0;
// Skybox_DrawStandard/DrawWave start their strips at the sky's sub-texel
// offset: up to 3.75 px down and 7.75 px right of the corner. That gap is never
// cleared, so it holds leftovers from old frames and flickers as the camera
// moves. A TV's border hides it; here it sits at the top edge of your view, so
// the layer leaves those rows and columns out.
constexpr float kSkyGapTopPx = 0.0f;    // of 240 (Goemon: nothing to crop)
constexpr float kSkyGapLeftPx = 0.0f;   // of 320
// Tell the compositor the picture is pitched like the game's chase camera.
// Declaring it level while the camera looks down makes yaw spin the world
// around a tilted axis, which reads as the level bending as you turn.
constexpr bool kLayerFollowsCameraPitch = true;
// Up/down: the camera looks where your head looks (from the game's chase
// position). With the game's own downward tilt the picture sat centred below
// your eye line, so holding your head level showed black past its top edge,
// and that edge jumped every time the game re-tilted its camera.
// false = keep the game's tilt (the old behaviour).
constexpr bool kHeadPitchDrivesCamera = true;
// First-person view (right stick click): the camera sits this far above
// Bomberman's feet, and the near clip comes in so the floor isn't cut away.
// Try other heights live with `adb shell setprop debug.goemon.fpheight <n>`.
constexpr float kFirstPersonEyeHeight = 85.0f;
constexpr float kFirstPersonNearPlane = 12.0f;
constexpr float kMaxLookUpDeg = 70.0f;
constexpr float kMaxLookDownDeg = 80.0f;
// Extra view drawn past the panel edge on every side. Head motion between
// game frames otherwise uncovers black at the edges of your view.
// 5 still let the edge show at extreme eye angles; 7 costs ~8% sharpness.
// 13 was for 30 pictures a second (each turned for 3 refreshes); at 60 the compositor
// turns a picture for 1 extra refresh, so 8 is enough (Smash 0.4.5) and the
// pixels go to the middle of the view.
constexpr float kOverscanDeg = 8.0f;
// Title, menus and pause: a floating screen in front of you instead of the
// picture filling your whole view.
constexpr float kScreenDistanceM = 2.6f;
constexpr float kScreenHeightM = 1.5f;
// HUD (health, bombs, gems, score) and other screen-space 2D in levels. The
// game lays it out for the corners of a TV; spread over the whole headset view
// it landed outside what you can see. RT64 now pulls it into a 4:3 box this
// many degrees above and below the middle of your view.
// The HUD box: half its height (degrees), and how far its middle sits below the
// view's middle. History: 22 sat right above the character (PC), 28 and 35 (no drop)
// too high; 35 dropped 17 too low, and the text boxes felt too close (owner, test 3).
// Live: `adb shell setprop debug.goemon.hudsize <deg>` / `debug.goemon.huddown <deg>`.
constexpr float kHudHalfHeightDeg = 26.0f;
// Head movement (6DOF): game units per meter of head travel. Goemon is about 20
// units tall and the VR eye sits at his head height, ~20 units over the ground,
// so a seated player's ~1.3 m eye height is ~15 units a meter. Live:
// `adb shell setprop debug.goemon.units <n>` (0 = the eye ignores head movement).
constexpr int kGoemonUnitsPerMeter = 15;
constexpr float kHudDropDeg = 18.0f;
// RT64 aspect for menus: the game's full 4:3 frame so nothing at the sides is
// cut off. Levels use the eye image's own shape (world_aspect()).
constexpr double kScreenAspectTarget = 4.0 / 3.0;
constexpr float kStickYawSpeed = 2.0f;
// First person turns faster (degrees per game frame at full tilt, ~30 fps).
constexpr float kFirstPersonStickYawSpeed = 4.5f;
constexpr float kStickYawDeadzone = 0.20f;
// Smooth turning speed with the right stick at full tilt.
constexpr float kTurnDegPerSecond = 120.0f;

enum class InitState { Untried, Failed, Ready };

struct XrApi {
    PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_xrInitializeLoaderKHR InitializeLoaderKHR = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
    PFN_xrCreateInstance CreateInstance = nullptr;
    PFN_xrDestroyInstance DestroyInstance = nullptr;
    PFN_xrResultToString ResultToString = nullptr;
    PFN_xrGetSystem GetSystem = nullptr;
    PFN_xrPollEvent PollEvent = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrDestroySession DestroySession = nullptr;
    PFN_xrBeginSession BeginSession = nullptr;
    PFN_xrEndSession EndSession = nullptr;
    PFN_xrEnumerateViewConfigurationViews EnumerateViewConfigurationViews = nullptr;
    PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
    PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
    PFN_xrDestroySpace DestroySpace = nullptr;
    PFN_xrLocateViews LocateViews = nullptr;
    PFN_xrWaitFrame WaitFrame = nullptr;
    PFN_xrBeginFrame BeginFrame = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
    PFN_xrCreateActionSet CreateActionSet = nullptr;
    PFN_xrDestroyActionSet DestroyActionSet = nullptr;
    PFN_xrCreateAction CreateAction = nullptr;
    PFN_xrStringToPath StringToPath = nullptr;
    PFN_xrSuggestInteractionProfileBindings SuggestInteractionProfileBindings = nullptr;
    PFN_xrAttachSessionActionSets AttachSessionActionSets = nullptr;
    PFN_xrSyncActions SyncActions = nullptr;
    PFN_xrGetActionStateBoolean GetActionStateBoolean = nullptr;
    PFN_xrGetActionStateFloat GetActionStateFloat = nullptr;
    PFN_xrGetActionStateVector2f GetActionStateVector2f = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR GetVulkanGraphicsRequirementsKHR = nullptr;
    PFN_xrGetVulkanInstanceExtensionsKHR GetVulkanInstanceExtensionsKHR = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR GetVulkanDeviceExtensionsKHR = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR GetVulkanGraphicsDeviceKHR = nullptr;
};

struct EyeChain {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkanKHR> images;
    int32_t acquired = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    // An image acquired but not yet free (the compositor was still showing it).
    bool wait_pending = false;
    uint32_t pending_index = 0;
};

// What a picture is shown with: the pose and field of view it was drawn for.
struct LayerDesc {
    bool valid = false;
    bool quad = false;
    XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    XrFovf fov{};
    float quad_width = kScreenHeightM * 4.0f / 3.0f;
    XrRect2Di rect{{0, 0}, {0, 0}};  // part of the image shown; empty means all of it
};

// One draw on the display-list thread and the pictures RT64 presents for it
// (one, or two in levels with smooth motion), handed over on RT64's present
// thread (on_presented_frame).
struct Capture {
    std::mutex mu;
    uint64_t serial = 0;
    uint64_t token = 0;           // armed for the next present of this draw
    bool copied = false;          // its first picture reached a slot
    // The first RT64 present the armed draw can produce. Presents older than
    // this belong to an earlier frame (the queue lags under load, e.g. while
    // the Quest is recording) and must not be copied in its place.
    uint64_t min_present_id = 0;
    uint32_t stale_skipped = 0;
    LayerDesc pending;            // the layer of the frame being drawn
    bool pending_world = false;
    bool pending_cut = false;     // the camera jumped: no in-between pose blend
    // The frame RT64 is presenting now (its in-between picture comes first),
    // and the game frame before it, whose pose an in-between picture blends with.
    uint64_t active_present = 0;
    LayerDesc active_cur;
    LayerDesc active_prev;
    bool active_world = false;
    bool active_has_prev = false;
    LayerDesc last;
    bool last_world = false;
    bool have_last = false;
};

XrApi xr;
void* loader = nullptr;
std::atomic<InitState> init_state{InitState::Untried};
XrInstance instance = XR_NULL_HANDLE;
XrSystemId system_id = XR_NULL_SYSTEM_ID;
XrSession session = XR_NULL_HANDLE;
XrSpace local_space = XR_NULL_HANDLE;
XrActionSet action_set = XR_NULL_HANDLE;
XrAction action_jump = XR_NULL_HANDLE;
XrAction action_throw = XR_NULL_HANDLE;
XrAction action_move = XR_NULL_HANDLE;
XrAction action_snap = XR_NULL_HANDLE;
XrAction action_detonate = XR_NULL_HANDLE;
XrAction action_drop = XR_NULL_HANDLE;
XrAction action_start = XR_NULL_HANDLE;
XrAction action_view_toggle = XR_NULL_HANDLE;
XrAction action_l = XR_NULL_HANDLE;
XrAction action_cup = XR_NULL_HANDLE;
XrAction action_cdown = XR_NULL_HANDLE;
XrAction action_cleft = XR_NULL_HANDLE;
XrAction action_cright = XR_NULL_HANDLE;
// Asked for by the right stick click (input thread). The game thread turns it
// into fp_active, and while active the camera heading is
// fp_base_deg - head yaw: it starts where Bomberman faces and follows the headset.
std::atomic<bool> first_person{false};
// Right stick click: undo smooth turning (aim_camera picks it up).
std::atomic<bool> body_reset_requested{false};
// First person on/off (right stick click).
std::atomic<bool> first_person_on{false};
bool fp_active = false;
float fp_base_deg = 0.0f;
XrPath path_hand_left = XR_NULL_PATH;
XrPath path_hand_right = XR_NULL_PATH;
std::atomic<bool> session_begun{false};
bool actions_attached = false;
int64_t swap_format = 0;
bool swap_needs_blit = false;
Capture capture;
// Frame thread only.
XrFrameState frame_state{XR_TYPE_FRAME_STATE};
XrPosef eye_pose[2]{};
float eye_offset_m[2] = {-0.032f, 0.032f};
bool views_located = false;
// A picture has gone to the headset.
std::atomic<bool> ever_presented{false};
bool touch_plus_ext = false;
// XR_FB_composition_layer_settings: lets the compositor super-sample our
// larger-than-panel picture instead of point-sampling it (less shimmer).
bool layer_settings_ext = false;
// XR_FB_display_refresh_rate: lets us ask for 90 Hz (see request_refresh_rate).
bool refresh_rate_ext = false;
// XR_EXT_performance_settings: lets smooth motion ask for higher GPU clocks.
bool perf_settings_ext = false;
// The display-list thread's copy of the head state below (take_head_state).
bool headset_fov_ready = false;
float proj_x_scale = 1.6084f;
float proj_y_scale = 2.1445f;
XrFovf shared_fov{};
XrFovf world_fov{};  // the whole rendered level image: shared_fov plus overscan and the sky-gap edges.
// Eye image size, chosen from the runtime's recommended size when the session starts.
uint32_t eye_width = kFallbackEyeWidth;
uint32_t eye_height = kFallbackEyeHeight;

// Levels render at the eye image's shape.
double world_aspect() {
    return static_cast<double>(eye_width) / static_cast<double>(eye_height);
}

// RT64 is set to a Manual aspect of world_aspect() for levels. Its projection
// processor then multiplies the world projection's X column by
// (4:3) / aspect. Without undoing that, the picture is squeezed into a narrower
// horizontal FOV than the layer claims, so the world swims when the head turns.
float rt64_aspect_scale() {
    return static_cast<float>(world_aspect() / (4.0 / 3.0));
}
bool pose_locked = false;
XrPosef locked_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
std::string instance_extensions;
std::string device_extensions;
uint32_t vulkan_max_api = 0;
JavaVM* java_vm = nullptr;
jobject activity = nullptr;
bool loader_initialized = false;

plume::VulkanDevice* vk_device = nullptr;
plume::VulkanCommandQueue* vk_queue = nullptr;
VkCommandPool copy_pool = VK_NULL_HANDLE;
VkCommandBuffer copy_cmd = VK_NULL_HANDLE;
VkFence copy_fence = VK_NULL_HANDLE;

// The Quest runtime submits its own GPU work on the queue handed to it in
// XrGraphicsBindingVulkanKHR (RT64's present queue) during swapchain
// create/destroy/acquire/release and xrBeginFrame/xrEndFrame. RT64's threads
// submit to that same VkQueue under plume's mutex, so those calls must hold it
// too. Without it the two raced and hung the GPU once the Quest started
// recording (the runtime does extra queue work for the capture).
// Set at session creation: the mutex of the queue OpenXR was given, or null
// when it has a queue of its own (then xr_mu already serializes its calls).
std::mutex* xr_queue_mutex = nullptr;

std::unique_lock<std::mutex> lock_xr_queue() {
    if (xr_queue_mutex != nullptr) {
        return std::unique_lock<std::mutex>(*xr_queue_mutex);
    }
    return std::unique_lock<std::mutex>();
}

std::mutex xr_mu;
std::atomic<uint32_t> vr_controls{0};
std::atomic<uint32_t> head_yaw_bits{0};
std::atomic<uint32_t> head_pitch_bits{0};
// Head pitch from level, radians, +up. Sampled with the yaw in locate_views.
std::atomic<uint32_t> head_pitch_abs_bits{0};
std::atomic<uint32_t> stick_lx_bits{0};
std::atomic<uint32_t> stick_ly_bits{0};
std::atomic<uint32_t> stick_rx_bits{0};
// Test switch "turnleft"/"turnright" (debug.goemon.press): the right stick from a PC.
std::atomic<uint32_t> test_stick_rx_bits{0};
std::atomic<uint32_t> button_a{0};
std::atomic<uint32_t> button_b{0};
std::atomic<uint32_t> button_z{0};
std::atomic<uint32_t> button_r{0};
std::atomic<uint32_t> button_start{0};
std::atomic<uint32_t> button_l{0};
std::atomic<uint32_t> button_cup{0};
std::atomic<uint32_t> button_cdown{0};
std::atomic<uint32_t> button_cleft{0};
std::atomic<uint32_t> button_cright{0};
std::atomic<uint32_t> stick_ry_bits{0};
float snap_degrees = 0.0f;
float stick_yaw_degrees = 0.0f;
bool stick_yaw_seeded = false;
std::atomic<uint32_t> aim_qx{0};
std::atomic<uint32_t> aim_qy{0};
std::atomic<uint32_t> aim_qz{0};
std::atomic<uint32_t> aim_qw{0x3f800000u};
std::atomic<uint32_t> aim_seq{0};
std::atomic<uint32_t> aim_ref_qx{0};
std::atomic<uint32_t> aim_ref_qy{0};
std::atomic<uint32_t> aim_ref_qz{0};
std::atomic<uint32_t> aim_ref_qw{0x3f800000u};
std::atomic<uint32_t> aim_ready{0};
// What the game thread used to build each gameplay frame. aim_view writes one
// per frame; the display-list hook takes the one whose matrix matches its task.
// Pairing them by matrix address keeps the picture, its projection and the
// pose we hand the compositor from the same frame, even with two frames in flight.
struct FrameRecord {
    uint32_t dl_phys = 0;    // where the display list was when the camera was drawn (physical)
    uint32_t proj_phys = 0;  // its projection matrix (physical)
    uint64_t seq = 0;
    float yaw = 0.0f;        // head yaw applied to the camera, radians, +left
    float pitch = 0.0f;      // pitch of the camera that drew the frame, radians, +up
    bool fresh = false;
    bool first_person = false;  // drawn from Bomberman's head (near plane moved in)
    bool cut = false;           // RT64 doesn't blend the camera into this frame
    bool has_head_pos = false;   // the eye moved with the head (6DOF) for this frame
    XrVector3f head_pos{0.0f, 0.0f, 0.0f};  // ... from here (LOCAL space)
};
std::mutex record_mu;
FrameRecord frame_records[16];
uint64_t record_seq = 0;
uint64_t record_taken_seq = 0;
// The last few task lists (Goemon alternates two), so the fallback below never
// takes a camera that was drawn into another task's list.
uint32_t recent_lists[4] = {};

// The head as the frame thread last located it (locate_views, under head_mu).
// The display-list thread works from its own copy of these (the globals of the
// same names), taken at the start of each display list by take_head_state.
struct HeadState {
    bool valid = false;            // position known
    float yaw_abs = 0.0f;          // which way the head faces, radians
    XrVector3f mid{0.0f, 0.0f, 0.0f};
    XrVector3f anchor{0.0f, 0.0f, 0.0f};
    bool anchor_set = false;
    XrFovf world_fov{};
    float proj_x_scale = 1.6084f;
    float proj_y_scale = 2.1445f;
    bool fov_ready = false;
};
std::mutex head_mu;
HeadState head_live;

// Latest head pose, for placing the menu screen. Display-list thread only.
bool head_valid = false;
float head_yaw_abs = 0.0f;
XrVector3f head_mid{0.0f, 0.0f, 0.0f};
bool screen_placed = false;
bool last_frame_screen = true;
XrPosef screen_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -kScreenDistanceM}};

XrVector3f aim_anchor{0.0f, 0.0f, 0.0f};
bool aim_anchor_set = false;

void take_head_state() {
    std::lock_guard<std::mutex> lock(head_mu);
    head_valid = head_live.valid;
    head_yaw_abs = head_live.yaw_abs;
    head_mid = head_live.mid;
    aim_anchor = head_live.anchor;
    aim_anchor_set = head_live.anchor_set;
    world_fov = head_live.world_fov;
    proj_x_scale = head_live.proj_x_scale;
    proj_y_scale = head_live.proj_y_scale;
    headset_fov_ready = head_live.fov_ready;
}

struct SavedAim {
    bool active = false;
    float at[3]{};
    float up[3]{};
    float rot[3]{};
    // First person: the game's own eye and Bomberman's flags, put back after the frame.
    bool eye_moved = false;
    float eye[3]{};
    uint8_t player_flags = 0;
};
SavedAim saved_aim;
bool snap_ready = true;
bool left_trigger_held = false;
bool right_trigger_held = false;
bool math_tested = false;

void vr_log(const char* fmt, ...) {
    char buf[640];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    __android_log_print(ANDROID_LOG_INFO, "OpenXR", "%s", buf);
    std::fprintf(stdout, "[OpenXR] %s\n", buf);
    std::fflush(stdout);
    FILE* file = std::fopen("openxr.log", "a");
    if (file != nullptr) {
        std::fprintf(file, "[OpenXR] %s\n", buf);
        std::fclose(file);
    }
}

// Test switches for the screen-recording GPU hang, set from a PC with
// `adb shell setprop debug.goemon.vr <bits>` (read about once a second):
//   1 = don't copy the picture into the eye images (they keep the old one)
//   2 = no compositor super-sampling on the level layer
//   4 = RT64 doesn't rebuild the hidden flat-window swapchain (see rt64_present_queue.cpp)
//   8 = RT64 draws nothing once a picture has been shown (the headset keeps the last one)
// `debug.goemon.vrscale` (read once at start) overrides kResolutionScale.
constexpr int kDebugNoCopy = 1;
constexpr int kDebugNoSuperSampling = 2;
constexpr int kDebugNoDraw = 8;
// Explosion investigation (live): draw levels with the game's own narrow
// projection, and turn the HUD pull-in off.
constexpr int kDebugNoWiden = 16384;
constexpr int kDebugNoHudInset = 32768;
std::atomic<int> debug_flags{0};

int debug_prop_int(const char* name, int fallback) {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get(name, value) <= 0) {
        return fallback;
    }
    return std::atoi(value);
}

// Test-only switches (debug.goemon.press, debug.goemon.interp) count only until
// debug.goemon.testuntil, a time on the headset's clock (seconds since 1970)
// set by the test run, so a lost connection can't leave them on for the owner.
bool test_switches_live() {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.goemon.testuntil", value);
    const long long until = std::atoll(value);
    return until > 0 && static_cast<long long>(std::time(nullptr)) < until;
}

float debug_prop_float(const char* name, float fallback) {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get(name, value) <= 0) {
        return fallback;
    }
    const float parsed = static_cast<float>(std::atof(value));
    return parsed > 0.0f ? parsed : fallback;
}

// Test switch, read once when plume creates its queues:
// `adb shell setprop debug.goemon.xrqueue 1` keeps the last native queue out
// of RT64's hands so the OpenXR runtime gets a queue of its own.
extern "C" int bmhero_plume_reserve_last_queue(void) {
    static const int reserve = debug_prop_int("debug.goemon.xrqueue", 0);
    return reserve != 0 ? 1 : 0;
}

// Global priority of RT64's Vulkan queues, read when plume creates the device.
// LOW is the screen-recording fix: at the default (medium) priority or higher,
// RT64's fast pipelines hang the GPU while the Quest records or casts; at low
// priority our work yields to the capture and nothing hangs (found in the
// WWF No Mercy port). Test override, read at launch:
// `adb shell setprop debug.goemon.qprio low|medium|high|realtime|default`
// ("default" = no priority request at all; unset or "0" = low).
extern "C" int bmhero_plume_queue_priority(void) {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.goemon.qprio", value);
    const std::string name = value;
    int priority = VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR;
    if (name == "default") {
        priority = 0;
    } else if (name == "medium") {
        priority = VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;
    } else if (name == "high") {
        priority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
    } else if (name == "realtime") {
        priority = VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR;
    }
    vr_log("queue priority setting \"%s\" -> %d", value, priority);
    return priority;
}

extern "C" void bmhero_plume_log(const char* message) {
    vr_log("plume: %s", message);
}

void refresh_debug_flags() {
    static uint32_t calls = 0;
    if ((calls++ % 30u) != 0u) {
        return;
    }
    const int flags = debug_prop_int("debug.goemon.vr", 0);
    if (flags != debug_flags.exchange(flags)) {
        vr_log("test switches now %d (no copy %d, no super-sampling %d, no window rebuild %d, no draw %d, no widen %d, no hud inset %d)",
            flags, (flags & kDebugNoCopy) != 0, (flags & kDebugNoSuperSampling) != 0, (flags & 4) != 0, (flags & kDebugNoDraw) != 0,
            (flags & kDebugNoWiden) != 0, (flags & kDebugNoHudInset) != 0);
    }
}

// Logs a call that blocked for longer than `limit_ms`, a few times per name.
struct SlowCallLog {
    const char* name;
    double limit_ms;
    int* budget;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~SlowCallLog() {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (ms > limit_ms && *budget > 0) {
            --*budget;
            vr_log("slow: %s took %.0f ms", name, ms);
        }
    }
};

uint32_t float_bits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bits_float(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint8_t* vram_bytes(uint8_t* rdram, uint32_t vram) {
    uint64_t addr = static_cast<uint64_t>(static_cast<int32_t>(vram));
    return rdram + (addr - 0xFFFFFFFF80000000ULL);
}

void write_f32(uint8_t* rdram, uint32_t vram, float value) {
    std::memcpy(vram_bytes(rdram, vram), &value, sizeof(value));
}

float read_f32(uint8_t* rdram, uint32_t vram) {
    float value = 0.0f;
    std::memcpy(&value, vram_bytes(rdram, vram), sizeof(value));
    return value;
}

int32_t read_s32(uint8_t* rdram, uint32_t vram) {
    int32_t value = 0;
    std::memcpy(&value, vram_bytes(rdram, vram), sizeof(value));
    return value;
}

// RDRAM is stored as native 32-bit words, so a byte lives at address ^ 3.
int8_t read_s8(uint8_t* rdram, uint32_t vram) {
    return static_cast<int8_t>(*vram_bytes(rdram, vram ^ 3u));
}

void write_u8(uint8_t* rdram, uint32_t vram, uint8_t value) {
    *vram_bytes(rdram, vram ^ 3u) = value;
}

struct Quat {
    float x;
    float y;
    float z;
    float w;
};

Quat quat_mul(Quat a, Quat b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat quat_inv(Quat q) {
    return {-q.x, -q.y, -q.z, q.w};
}

void quat_rotate(Quat q, float& x, float& y, float& z) {
    Quat v{x, y, z, 0.0f};
    Quat r = quat_mul(quat_mul(q, v), quat_inv(q));
    x = r.x;
    y = r.y;
    z = r.z;
}

bool load_stable_quat(const std::atomic<uint32_t>& sx, const std::atomic<uint32_t>& sy, const std::atomic<uint32_t>& sz, const std::atomic<uint32_t>& sw, const std::atomic<uint32_t>& seq, Quat& out) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t before = seq.load();
        if ((before & 1u) != 0u) {
            continue;
        }
        out.x = bits_float(sx.load());
        out.y = bits_float(sy.load());
        out.z = bits_float(sz.load());
        out.w = bits_float(sw.load());
        if (seq.load() == before) {
            return true;
        }
    }
    return false;
}

void store_aim_quat(Quat q) {
    aim_seq.fetch_add(1u);
    aim_qx.store(float_bits(q.x));
    aim_qy.store(float_bits(q.y));
    aim_qz.store(float_bits(q.z));
    aim_qw.store(float_bits(q.w));
    aim_seq.fetch_add(1u);
}

void run_math_self_test() {
    if (math_tested) {
        return;
    }
    math_tested = true;
    float look[16];
    vr_look_at(0.0f, 0.0f, 500.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, look);
    float eye[3], right[3], up[3], lookv[3];
    vr_basis_from_lookat(look, eye, right, up, lookv);
    float inv[16];
    float back[16];
    bool shifted = vr_shift_lookat(look, 6.4f, 0.0f, 0.0f, inv) && vr_invert_matrix(inv, back);
    float eye2[3], right2[3], up2[3], look2[3];
    vr_basis_from_lookat(back, eye2, right2, up2, look2);
    float yaw0 = vr_yaw_from_quat(0.0f, 0.0f, 0.0f, 1.0f);
    float half = 3.14159265f / 4.0f;
    float yaw_left = vr_yaw_from_quat(0.0f, std::sin(half), 0.0f, std::cos(half));
    auto chase_eye_x = [](float rot_y_deg) {
        float view_rot_y = (rot_y_deg + 90.0f) * 0.0174532925f;
        return 500.0f * std::cos(view_rot_y);
    };
    bool pass = std::fabs(eye[2] - 500.0f) < 1.0e-3f && std::fabs(right[0] - 1.0f) < 1.0e-3f && std::fabs(lookv[2] - 1.0f) < 1.0e-3f;
    pass = pass && shifted && std::fabs(eye2[0] - 6.4f) < 1.0e-2f && std::fabs(eye2[2] - 500.0f) < 1.0e-2f;
    pass = pass && std::fabs(right2[0] - 1.0f) < 1.0e-3f && std::fabs(look2[2] - 1.0f) < 1.0e-3f;
    float pitch0 = vr_pitch_from_quat(0.0f, 0.0f, 0.0f, 1.0f);
    float pitch_half = 15.0f * 3.14159265f / 180.0f * 0.5f;
    float pitch_down = vr_pitch_from_quat(std::sin(pitch_half), 0.0f, 0.0f, std::cos(pitch_half));
    pass = pass && std::fabs(yaw0) < 1.0e-4f && std::fabs(yaw_left - 3.14159265f / 2.0f) < 1.0e-3f;
    pass = pass && std::fabs(pitch0) < 1.0e-4f && std::fabs(pitch_down - 15.0f * 3.14159265f / 180.0f) < 1.0e-3f;
    pass = pass && std::fabs(chase_eye_x(0.0f)) < 1.0e-3f && chase_eye_x(-10.0f) > 1.0f;
    const float s45 = std::sin(half);
    const float c45 = std::cos(half);
    float lx = 0.0f;
    float ly = 0.0f;
    float lz = -1.0f;
    quat_rotate(Quat{0.0f, s45, 0.0f, c45}, lx, ly, lz);
    pass = pass && std::fabs(lx + 1.0f) < 1.0e-3f && std::fabs(ly) < 1.0e-3f && std::fabs(lz) < 1.0e-3f;
    lx = 0.0f;
    ly = 0.0f;
    lz = -1.0f;
    quat_rotate(Quat{s45, 0.0f, 0.0f, c45}, lx, ly, lz);
    pass = pass && std::fabs(lx) < 1.0e-3f && std::fabs(ly - 1.0f) < 1.0e-3f && std::fabs(lz) < 1.0e-3f;
    vr_log("%s", pass ? "math self-test passed" : "math self-test FAILED");
}

bool xr_ok(XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    char text[XR_MAX_RESULT_STRING_SIZE] = {};
    if (instance != XR_NULL_HANDLE && xr.ResultToString != nullptr) {
        xr.ResultToString(instance, result, text);
        vr_log("%s failed: %s (%d)", what, text, static_cast<int>(result));
    } else {
        vr_log("%s failed: XrResult %d", what, static_cast<int>(result));
    }
    return false;
}

template <typename T>
bool load_pfn(XrInstance xr_instance, const char* name, T* out) {
    PFN_xrVoidFunction fn = nullptr;
    XrResult result = xr.GetInstanceProcAddr(xr_instance, name, &fn);
    if (!XR_SUCCEEDED(result) || fn == nullptr) {
        vr_log("missing OpenXR entry %s", name);
        *out = nullptr;
        return false;
    }
    *out = reinterpret_cast<T>(fn);
    return true;
}

void split_extensions(const std::string& text, std::vector<std::string>* out) {
    std::string current;
    for (char c : text) {
        if (c == ' ') {
            if (!current.empty()) {
                out->push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out->push_back(current);
    }
}

std::string query_extension_string(PFN_xrGetVulkanInstanceExtensionsKHR instance_fn, PFN_xrGetVulkanDeviceExtensionsKHR device_fn, bool device_list) {
    uint32_t count = 0;
    XrResult result = device_list
        ? device_fn(instance, system_id, 0, &count, nullptr)
        : instance_fn(instance, system_id, 0, &count, nullptr);
    if (!XR_SUCCEEDED(result) || count == 0) {
        return {};
    }
    std::vector<char> buffer(count, '\0');
    result = device_list
        ? device_fn(instance, system_id, count, &count, buffer.data())
        : instance_fn(instance, system_id, count, &count, buffer.data());
    if (!XR_SUCCEEDED(result)) {
        return {};
    }
    return std::string(buffer.data());
}

bool capture_activity() {
    if (java_vm != nullptr && activity != nullptr) {
        return true;
    }
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject local = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env == nullptr || local == nullptr) {
        vr_log("SDL did not provide a JNI activity. Staying flat.");
        return false;
    }
    if (env->GetJavaVM(&java_vm) != JNI_OK || java_vm == nullptr) {
        env->DeleteLocalRef(local);
        vr_log("JavaVM is unavailable. Staying flat.");
        return false;
    }
    activity = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    return activity != nullptr;
}

std::string native_library_dir() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (env == nullptr || activity == nullptr) {
        return {};
    }
    jclass activity_class = env->GetObjectClass(activity);
    jmethodID get_info = env->GetMethodID(activity_class, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    jobject info = env->CallObjectMethod(activity, get_info);
    if (env->ExceptionCheck() || info == nullptr) {
        env->ExceptionClear();
        env->DeleteLocalRef(activity_class);
        return {};
    }
    jclass info_class = env->GetObjectClass(info);
    jfieldID dir_field = env->GetFieldID(info_class, "nativeLibraryDir", "Ljava/lang/String;");
    jstring dir = static_cast<jstring>(env->GetObjectField(info, dir_field));
    std::string path;
    if (dir != nullptr) {
        const char* chars = env->GetStringUTFChars(dir, nullptr);
        path = chars != nullptr ? chars : "";
        if (chars != nullptr) {
            env->ReleaseStringUTFChars(dir, chars);
        }
        env->DeleteLocalRef(dir);
    }
    env->DeleteLocalRef(info_class);
    env->DeleteLocalRef(info);
    env->DeleteLocalRef(activity_class);
    return path;
}

bool load_loader() {
    if (xr.GetInstanceProcAddr != nullptr) {
        return true;
    }
    loader = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
    if (loader == nullptr) {
        const std::string dir = native_library_dir();
        if (!dir.empty()) {
            const std::string full = dir + "/libopenxr_loader.so";
            loader = dlopen(full.c_str(), RTLD_NOW | RTLD_LOCAL);
        }
    }
    if (loader == nullptr) {
        vr_log("libopenxr_loader.so was not found (%s). Staying flat.", dlerror());
        return false;
    }
    xr.GetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(loader, "xrGetInstanceProcAddr"));
    if (xr.GetInstanceProcAddr == nullptr) {
        vr_log("xrGetInstanceProcAddr is missing. Staying flat.");
        return false;
    }
    if (!load_pfn(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &xr.InitializeLoaderKHR)) {
        return false;
    }
    if (!capture_activity()) {
        return false;
    }
    XrLoaderInitInfoAndroidKHR info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    info.applicationVM = java_vm;
    info.applicationContext = activity;
    if (!xr_ok(xr.InitializeLoaderKHR(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&info)), "xrInitializeLoaderKHR")) {
        return false;
    }
    loader_initialized = true;
    bool ok = true;
    ok = load_pfn(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &xr.EnumerateInstanceExtensionProperties) && ok;
    ok = load_pfn(XR_NULL_HANDLE, "xrCreateInstance", &xr.CreateInstance) && ok;
    return ok;
}

bool extension_present() {
    uint32_t count = 0;
    if (!xr_ok(xr.EnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties")) {
        return false;
    }
    XrExtensionProperties blank{};
    blank.type = XR_TYPE_EXTENSION_PROPERTIES;
    std::vector<XrExtensionProperties> properties(count, blank);
    if (!xr_ok(xr.EnumerateInstanceExtensionProperties(nullptr, count, &count, properties.data()), "xrEnumerateInstanceExtensionProperties")) {
        return false;
    }
    bool vulkan = false;
    touch_plus_ext = false;
    layer_settings_ext = false;
    refresh_rate_ext = false;
    for (const XrExtensionProperties& property : properties) {
        if (std::strcmp(property.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) {
            vulkan = true;
        } else if (std::strcmp(property.extensionName, XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME) == 0) {
            touch_plus_ext = true;
        } else if (std::strcmp(property.extensionName, XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME) == 0) {
            layer_settings_ext = true;
        } else if (std::strcmp(property.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0) {
            refresh_rate_ext = true;
        } else if (std::strcmp(property.extensionName, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME) == 0) {
            perf_settings_ext = true;
        }
    }
    if (!vulkan) {
        vr_log("The OpenXR runtime has no Vulkan enable extension. Staying flat.");
        return false;
    }
    vr_log("Quest Touch Plus profile %s", touch_plus_ext ? "enabled" : "missing");
    vr_log("compositor super-sampling %s", layer_settings_ext ? "available" : "missing");
    return true;
}

bool load_instance_procs() {
    bool ok = true;
    ok = load_pfn(instance, "xrDestroyInstance", &xr.DestroyInstance) && ok;
    ok = load_pfn(instance, "xrResultToString", &xr.ResultToString) && ok;
    ok = load_pfn(instance, "xrGetSystem", &xr.GetSystem) && ok;
    ok = load_pfn(instance, "xrPollEvent", &xr.PollEvent) && ok;
    ok = load_pfn(instance, "xrCreateSession", &xr.CreateSession) && ok;
    ok = load_pfn(instance, "xrDestroySession", &xr.DestroySession) && ok;
    ok = load_pfn(instance, "xrBeginSession", &xr.BeginSession) && ok;
    ok = load_pfn(instance, "xrEndSession", &xr.EndSession) && ok;
    ok = load_pfn(instance, "xrEnumerateViewConfigurationViews", &xr.EnumerateViewConfigurationViews) && ok;
    ok = load_pfn(instance, "xrEnumerateSwapchainFormats", &xr.EnumerateSwapchainFormats) && ok;
    ok = load_pfn(instance, "xrCreateSwapchain", &xr.CreateSwapchain) && ok;
    ok = load_pfn(instance, "xrDestroySwapchain", &xr.DestroySwapchain) && ok;
    ok = load_pfn(instance, "xrEnumerateSwapchainImages", &xr.EnumerateSwapchainImages) && ok;
    ok = load_pfn(instance, "xrAcquireSwapchainImage", &xr.AcquireSwapchainImage) && ok;
    ok = load_pfn(instance, "xrWaitSwapchainImage", &xr.WaitSwapchainImage) && ok;
    ok = load_pfn(instance, "xrReleaseSwapchainImage", &xr.ReleaseSwapchainImage) && ok;
    ok = load_pfn(instance, "xrCreateReferenceSpace", &xr.CreateReferenceSpace) && ok;
    ok = load_pfn(instance, "xrDestroySpace", &xr.DestroySpace) && ok;
    ok = load_pfn(instance, "xrLocateViews", &xr.LocateViews) && ok;
    ok = load_pfn(instance, "xrWaitFrame", &xr.WaitFrame) && ok;
    ok = load_pfn(instance, "xrBeginFrame", &xr.BeginFrame) && ok;
    ok = load_pfn(instance, "xrEndFrame", &xr.EndFrame) && ok;
    ok = load_pfn(instance, "xrCreateActionSet", &xr.CreateActionSet) && ok;
    ok = load_pfn(instance, "xrDestroyActionSet", &xr.DestroyActionSet) && ok;
    ok = load_pfn(instance, "xrCreateAction", &xr.CreateAction) && ok;
    ok = load_pfn(instance, "xrStringToPath", &xr.StringToPath) && ok;
    ok = load_pfn(instance, "xrSuggestInteractionProfileBindings", &xr.SuggestInteractionProfileBindings) && ok;
    ok = load_pfn(instance, "xrAttachSessionActionSets", &xr.AttachSessionActionSets) && ok;
    ok = load_pfn(instance, "xrSyncActions", &xr.SyncActions) && ok;
    ok = load_pfn(instance, "xrGetActionStateBoolean", &xr.GetActionStateBoolean) && ok;
    ok = load_pfn(instance, "xrGetActionStateFloat", &xr.GetActionStateFloat) && ok;
    ok = load_pfn(instance, "xrGetActionStateVector2f", &xr.GetActionStateVector2f) && ok;
    ok = load_pfn(instance, "xrGetVulkanGraphicsRequirementsKHR", &xr.GetVulkanGraphicsRequirementsKHR) && ok;
    ok = load_pfn(instance, "xrGetVulkanInstanceExtensionsKHR", &xr.GetVulkanInstanceExtensionsKHR) && ok;
    ok = load_pfn(instance, "xrGetVulkanDeviceExtensionsKHR", &xr.GetVulkanDeviceExtensionsKHR) && ok;
    ok = load_pfn(instance, "xrGetVulkanGraphicsDeviceKHR", &xr.GetVulkanGraphicsDeviceKHR) && ok;
    return ok;
}

bool create_openxr_instance() {
    if (instance != XR_NULL_HANDLE) {
        return true;
    }
    if (!load_loader() || !extension_present()) {
        return false;
    }
    std::vector<const char*> extension_names;
    extension_names.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (touch_plus_ext) {
        extension_names.push_back(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
    }
    if (layer_settings_ext) {
        extension_names.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    }
    if (refresh_rate_ext) {
        extension_names.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
    if (perf_settings_ext) {
        extension_names.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    }
    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(create_info.applicationInfo.applicationName, sizeof(create_info.applicationInfo.applicationName), "Goemon 64: Recompiled");
    create_info.applicationInfo.applicationVersion = 1;
    std::snprintf(create_info.applicationInfo.engineName, sizeof(create_info.applicationInfo.engineName), "Goemon64Recomp");
    create_info.applicationInfo.engineVersion = 1;
    create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    create_info.enabledExtensionCount = static_cast<uint32_t>(extension_names.size());
    create_info.enabledExtensionNames = extension_names.data();
    if (!xr_ok(xr.CreateInstance(&create_info, &instance), "xrCreateInstance")) {
        vr_log("No Quest OpenXR runtime. The window stays flat.");
        return false;
    }
    return load_instance_procs();
}

bool acquire_system() {
    if (system_id != XR_NULL_SYSTEM_ID) {
        return true;
    }
    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    for (int attempt = 0; attempt < 8; ++attempt) {
        XrResult result = xr.GetSystem(instance, &system_info, &system_id);
        if (result == XR_SUCCESS) {
            return true;
        }
        if (result != XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
            xr_ok(result, "xrGetSystem");
            return false;
        }
        vr_log("OpenXR system is not ready (attempt %d). Retrying.", attempt + 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    vr_log("No headset system. The window stays flat.");
    return false;
}

void cache_vulkan_requirements() {
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (xr_ok(xr.GetVulkanGraphicsRequirementsKHR(instance, system_id, &requirements), "xrGetVulkanGraphicsRequirementsKHR")) {
        vulkan_max_api = static_cast<uint32_t>(requirements.maxApiVersionSupported);
        vr_log("Vulkan API range min %u max %u", static_cast<unsigned>(requirements.minApiVersionSupported), vulkan_max_api);
    }
    instance_extensions = query_extension_string(xr.GetVulkanInstanceExtensionsKHR, nullptr, false);
    device_extensions = query_extension_string(nullptr, xr.GetVulkanDeviceExtensionsKHR, true);
    vr_log("instance extensions: %s", instance_extensions.empty() ? "(none)" : instance_extensions.c_str());
    vr_log("device extensions: %s", device_extensions.empty() ? "(none)" : device_extensions.c_str());
}

XrAction create_action(const char* name, const char* localized, XrActionType type) {
    XrPath hands[2] = {path_hand_left, path_hand_right};
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    std::snprintf(info.actionName, sizeof(info.actionName), "%s", name);
    std::snprintf(info.localizedActionName, sizeof(info.localizedActionName), "%s", localized);
    // Quest leaves actions inactive unless they carry the hand paths.
    info.countSubactionPaths = 2;
    info.subactionPaths = hands;
    XrAction action = XR_NULL_HANDLE;
    if (!xr_ok(xr.CreateAction(action_set, &info, &action), name)) {
        return XR_NULL_HANDLE;
    }
    return action;
}

bool suggest_touch_bindings() {
    struct Binding {
        XrAction action;
        const char* path;
    };
    Binding bindings[] = {
        {action_jump, "/user/hand/right/input/a/click"},
        {action_throw, "/user/hand/right/input/b/click"},
        {action_move, "/user/hand/left/input/thumbstick"},
        {action_snap, "/user/hand/right/input/thumbstick"},
        {action_detonate, "/user/hand/left/input/trigger/value"},
        {action_drop, "/user/hand/right/input/trigger/value"},
        // Start/pause on the left controller's menu button (the owner's choice; was X).
        {action_start, "/user/hand/left/input/menu/click"},
        // Owner's layout (2026-10-17): left stick click = first person on/off, right
        // stick click = camera behind Goemon, left grip = the minimap (C-right; the
        // game doesn't use L).
        {action_view_toggle, "/user/hand/left/input/thumbstick/click"},
        {action_l, "/user/hand/left/input/squeeze/value"},
        {action_cdown, "/user/hand/right/input/squeeze/value"},
        {action_cup, "/user/hand/left/input/y/click"},
        {action_cleft, "/user/hand/left/input/x/click"},
        {action_cright, "/user/hand/right/input/thumbstick/click"},
    };
    XrActionSuggestedBinding suggested[std::size(bindings)]{};
    for (size_t i = 0; i < std::size(bindings); ++i) {
        suggested[i].action = bindings[i].action;
        if (!xr_ok(xr.StringToPath(instance, bindings[i].path, &suggested[i].binding), bindings[i].path)) {
            return false;
        }
    }
    const char* profiles[] = {
        "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/meta/touch_controller_plus",
    };
    bool any = false;
    for (const char* profile_path : profiles) {
        if (!touch_plus_ext && std::strcmp(profile_path, "/interaction_profiles/meta/touch_controller_plus") == 0) {
            continue;
        }
        XrPath profile = XR_NULL_PATH;
        if (!XR_SUCCEEDED(xr.StringToPath(instance, profile_path, &profile))) {
            vr_log("profile path unavailable: %s", profile_path);
            continue;
        }
        XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        info.interactionProfile = profile;
        info.countSuggestedBindings = static_cast<uint32_t>(std::size(suggested));
        info.suggestedBindings = suggested;
        if (xr_ok(xr.SuggestInteractionProfileBindings(instance, &info), profile_path)) {
            any = true;
            vr_log("bindings accepted for %s", profile_path);
        }
    }
    return any;
}

bool create_actions() {
    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(set_info.actionSetName, sizeof(set_info.actionSetName), "gameplay");
    std::snprintf(set_info.localizedActionSetName, sizeof(set_info.localizedActionSetName), "Gameplay");
    if (!xr_ok(xr.CreateActionSet(instance, &set_info, &action_set), "xrCreateActionSet")) {
        return false;
    }
    XrPath hand_left = XR_NULL_PATH;
    XrPath hand_right = XR_NULL_PATH;
    if (!xr_ok(xr.StringToPath(instance, "/user/hand/left", &hand_left), "left hand") ||
        !xr_ok(xr.StringToPath(instance, "/user/hand/right", &hand_right), "right hand")) {
        return false;
    }
    path_hand_left = hand_left;
    path_hand_right = hand_right;
    action_jump = create_action("jump", "Jump", XR_ACTION_TYPE_BOOLEAN_INPUT);
    action_throw = create_action("throw", "Throw", XR_ACTION_TYPE_BOOLEAN_INPUT);
    action_move = create_action("move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT);
    action_snap = create_action("snap_turn", "Snap Turn", XR_ACTION_TYPE_VECTOR2F_INPUT);
    action_detonate = create_action("detonate", "Detonate", XR_ACTION_TYPE_FLOAT_INPUT);
    action_drop = create_action("drop", "Drop", XR_ACTION_TYPE_FLOAT_INPUT);
    action_start = create_action("start", "Start", XR_ACTION_TYPE_BOOLEAN_INPUT);
    action_view_toggle = create_action("view_toggle", "First Person View", XR_ACTION_TYPE_BOOLEAN_INPUT);
    action_l = create_action("map", "Map (C Right)", XR_ACTION_TYPE_FLOAT_INPUT);
    action_cdown = create_action("c_down", "C Down", XR_ACTION_TYPE_FLOAT_INPUT);
    action_cup = create_action("c_up", "C Up", XR_ACTION_TYPE_BOOLEAN_INPUT);
    action_cleft = create_action("c_left", "C Left", XR_ACTION_TYPE_BOOLEAN_INPUT);
    action_cright = create_action("camera_behind", "Camera Behind Goemon", XR_ACTION_TYPE_BOOLEAN_INPUT);
    if (action_jump == XR_NULL_HANDLE || action_throw == XR_NULL_HANDLE || action_move == XR_NULL_HANDLE ||
        action_snap == XR_NULL_HANDLE || action_detonate == XR_NULL_HANDLE || action_drop == XR_NULL_HANDLE ||
        action_start == XR_NULL_HANDLE || action_view_toggle == XR_NULL_HANDLE || action_l == XR_NULL_HANDLE ||
        action_cdown == XR_NULL_HANDLE || action_cup == XR_NULL_HANDLE || action_cleft == XR_NULL_HANDLE || action_cright == XR_NULL_HANDLE) {
        return false;
    }
    if (!suggest_touch_bindings()) {
        vr_log("Touch bindings were not accepted. Controller input will be inactive.");
    }
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &action_set;
    if (!xr_ok(xr.AttachSessionActionSets(session, &attach), "xrAttachSessionActionSets")) {
        return false;
    }
    actions_attached = true;
    return true;
}

bool create_copy_commands() {
    if (copy_cmd != VK_NULL_HANDLE) {
        return true;
    }
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = vk_queue->familyIndex;
    if (vkCreateCommandPool(vk_device->vk, &pool_info, nullptr, &copy_pool) != VK_SUCCESS) {
        vr_log("vkCreateCommandPool failed. Staying flat.");
        return false;
    }
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = copy_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(vk_device->vk, &alloc, &copy_cmd) != VK_SUCCESS) {
        vr_log("vkAllocateCommandBuffers failed. Staying flat.");
        return false;
    }
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(vk_device->vk, &fence_info, nullptr, &copy_fence) != VK_SUCCESS) {
        vr_log("vkCreateFence failed. Staying flat.");
        return false;
    }
    return true;
}

// Levels drawn with RT64's in-between frames at this many pictures a second.
// Always 60 (owner, 2026-10-17: "make 60fps the default, remove the option at the
// rom selection"). Test override: `adb shell setprop debug.goemon.interp sharp`
// during a test run (see test_switches_live) gives the game's own 30.
int interp_fps() {
    static int fps = -1;
    if (fps < 0) {
        char forced[PROP_VALUE_MAX] = {};
        if (test_switches_live()) {
            __system_property_get("debug.goemon.interp", forced);
        }
        const std::string force = forced;
        bool smooth = false;
        if (force == "smooth" || force == "60") {
            smooth = true;
            vr_log("picture mode forced by debug.goemon.interp: smooth motion");
        } else if (force == "sharp") {
            vr_log("picture mode forced by debug.goemon.interp: sharp picture");
        } else {
            smooth = true;
        }
        fps = smooth ? kSmoothFps : 0;
        vr_log("level in-between frames: %s", smooth ? "on, 60 pictures a second" : "off, the game's 30 a second");
    }
    return fps;
}

// Level image size: the headset's recommended per-eye size times
// kResolutionScale, within what the runtime allows. Multiples of 8 keep
// RT64's 320 x 240 scaling and the GPU tiles tidy.
void choose_eye_size(const XrViewConfigurationView& view) {
    const uint32_t rec_w = view.recommendedImageRectWidth;
    const uint32_t rec_h = view.recommendedImageRectHeight;
    const uint32_t max_w = std::min<uint32_t>(view.maxImageRectWidth, kMaxEyeSide);
    const uint32_t max_h = std::min<uint32_t>(view.maxImageRectHeight, kMaxEyeSide);
    if (rec_w < 256u || rec_h < 256u || max_w < 256u || max_h < 256u) {
        vr_log("runtime gave no usable eye size (rec %ux%u max %ux%u), using %ux%u", rec_w, rec_h, max_w, max_h, eye_width, eye_height);
        return;
    }
    const float scale = std::min(debug_prop_float("debug.goemon.vrscale", kResolutionScale), 2.0f);
    auto pick = [scale](uint32_t recommended, uint32_t limit) {
        uint32_t size = static_cast<uint32_t>(std::lround(static_cast<double>(recommended) * scale));
        size = std::min(size, limit);
        return std::max<uint32_t>(size & ~7u, 512u);
    };
    eye_width = pick(rec_w, max_w);
    eye_height = pick(rec_h, max_h);
    const double pixels = static_cast<double>(eye_width) * static_cast<double>(eye_height);
    const double max_pixels = interp_fps() > 0 ? kMaxEyePixelsSmooth : kMaxEyePixels;
    if (pixels > max_pixels) {
        const double shrink = std::sqrt(max_pixels / pixels);
        eye_width = std::max<uint32_t>(static_cast<uint32_t>(eye_width * shrink) & ~7u, 512u);
        eye_height = std::max<uint32_t>(static_cast<uint32_t>(eye_height * shrink) & ~7u, 512u);
    }
    vr_log("eye image %ux%u (headset recommends %ux%u, allows up to %ux%u, scale %.2f, %s)",
        eye_width, eye_height, rec_w, rec_h, view.maxImageRectWidth, view.maxImageRectHeight, scale,
        interp_fps() > 0 ? "smooth motion" : "sharp picture");
}

bool create_slots();
void start_frame_thread();

bool ensure_session(RT64::Application* app) {
    if (init_state == InitState::Failed) {
        return false;
    }
    if (init_state == InitState::Ready) {
        return true;
    }
    init_state = InitState::Failed;
    if (instance == XR_NULL_HANDLE || system_id == XR_NULL_SYSTEM_ID) {
        vr_log("OpenXR did not start. Staying flat.");
        return false;
    }
    if (app == nullptr || app->device == nullptr || app->presentGraphicsWorker == nullptr || app->presentGraphicsWorker->commandQueue == nullptr) {
        vr_log("RT64 device is not ready. Staying flat.");
        return false;
    }
    if (app->chosenGraphicsAPI != RT64::UserConfiguration::GraphicsAPI::Vulkan) {
        vr_log("RT64 is not using Vulkan. Staying flat.");
        return false;
    }
    vk_device = dynamic_cast<plume::VulkanDevice*>(app->device.get());
    vk_queue = dynamic_cast<plume::VulkanCommandQueue*>(app->presentGraphicsWorker->commandQueue.get());
    if (vk_device == nullptr || vk_queue == nullptr || vk_device->vk == VK_NULL_HANDLE || vk_queue->queue == nullptr) {
        vr_log("Could not read the Vulkan device. Staying flat.");
        return false;
    }

    uint32_t xr_queue_index = vk_queue->queueIndex;
    xr_queue_mutex = vk_queue->queue->mutex.get();
    const plume::VulkanQueueFamily& family = vk_device->queueFamilies[vk_queue->familyIndex];
    if (bmhero_plume_reserve_last_queue() != 0 && family.queues.size() > 1 && family.queues.back().virtualQueues.empty()) {
        xr_queue_index = static_cast<uint32_t>(family.queues.size() - 1);
        xr_queue_mutex = nullptr;
    }
    vr_log("OpenXR uses queue %u of %zu in family %u (%zu RT64 queues on it)", xr_queue_index, family.queues.size(),
        vk_queue->familyIndex, family.queues[xr_queue_index].virtualQueues.size());

    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = vk_device->renderInterface->instance;
    binding.physicalDevice = vk_device->physicalDevice;
    binding.device = vk_device->vk;
    binding.queueFamilyIndex = vk_queue->familyIndex;
    binding.queueIndex = xr_queue_index;
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &binding;
    session_info.systemId = system_id;
    if (!xr_ok(xr.CreateSession(instance, &session_info, &session), "xrCreateSession")) {
        return false;
    }

    uint32_t view_count = 0;
    if (!xr_ok(xr.EnumerateViewConfigurationViews(instance, system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr), "xrEnumerateViewConfigurationViews") || view_count < 2) {
        vr_log("OpenXR did not report two views. Staying flat.");
        return false;
    }
    std::vector<XrViewConfigurationView> config_views(view_count, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (xr_ok(xr.EnumerateViewConfigurationViews(instance, system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, config_views.data()), "xrEnumerateViewConfigurationViews")) {
        choose_eye_size(config_views[0]);
    }

    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    if (!xr_ok(xr.CreateReferenceSpace(session, &space_info, &local_space), "xrCreateReferenceSpace")) {
        return false;
    }
    if (!create_actions() || !create_copy_commands() || !create_slots()) {
        return false;
    }
    init_state = InitState::Ready;
    start_frame_thread();
    vr_log("Vulkan session created. Stereo starts when the headset is visible.");
    vr_log("Touch: left stick move (forward = where you look), A jump, B attack, right trigger Z, left trigger R, left grip C-right (minimap), right grip C-down, Y C-up, X C-left, left stick click first person, left menu button Start, right stick left/right turns, right stick click camera behind Goemon, right stick up/down camera distance.");
    return true;
}

void set_controls_active(bool active) {
    vr_controls.store(active ? 1u : 0u);
}

// The game runs at 30 fps: at 90 Hz each picture is shown exactly 3 times
// (with the frame pacing below). The runtime default, 72 Hz, can only show
// them 2, 3, 2, 3 times, which makes motion judder. Smooth motion's 60
// pictures a second need 120 Hz (each shown twice; at 90 Hz they would
// alternate 1 and 2). Override, read when the session begins:
// `adb shell setprop debug.goemon.hz 72|80|90|120`.
constexpr float kDisplayHz = 90.0f;
constexpr float kDisplayHzSmooth = 120.0f;

// Smooth motion draws twice as many pictures; the Quest otherwise keeps the
// GPU at a low clock level and pictures come late more often (Wave Race:
// level 2 -> 4 with sustained high). The sharp picture keeps the system's
// choice, as it always had. `debug.goemon.perf` (session start): high, boost,
// low, off, or unset/0 for the default.
void request_performance() {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.goemon.perf", value);
    std::string choice = value;
    if (choice.empty() || choice == "0") {
        choice = interp_fps() > 0 ? "high" : "off";
    }
    if (choice == "off") {
        vr_log("clock levels left to the system");
        return;
    }
    if (!perf_settings_ext) {
        vr_log("clock levels can't be chosen (no XR_EXT_performance_settings)");
        return;
    }
    PFN_xrPerfSettingsSetPerformanceLevelEXT set_level = nullptr;
    if (!load_pfn(instance, "xrPerfSettingsSetPerformanceLevelEXT", &set_level)) {
        return;
    }
    XrPerfSettingsLevelEXT level = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
    if (choice == "boost") {
        level = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
    } else if (choice == "low") {
        level = XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT;
    }
    const bool gpu = xr_ok(set_level(session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, level), "xrPerfSettingsSetPerformanceLevelEXT GPU");
    const bool cpu = xr_ok(set_level(session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, level), "xrPerfSettingsSetPerformanceLevelEXT CPU");
    vr_log("clock levels requested: %s (GPU %s, CPU %s)", choice.c_str(), gpu ? "ok" : "refused", cpu ? "ok" : "refused");
}

void request_refresh_rate() {
    if (!refresh_rate_ext) {
        vr_log("display refresh rate can't be chosen (no XR_FB_display_refresh_rate)");
        return;
    }
    PFN_xrEnumerateDisplayRefreshRatesFB enumerate_rates = nullptr;
    PFN_xrRequestDisplayRefreshRateFB request_rate = nullptr;
    if (!load_pfn(instance, "xrEnumerateDisplayRefreshRatesFB", &enumerate_rates) ||
        !load_pfn(instance, "xrRequestDisplayRefreshRateFB", &request_rate)) {
        return;
    }
    uint32_t count = 0;
    if (!xr_ok(enumerate_rates(session, 0, &count, nullptr), "xrEnumerateDisplayRefreshRatesFB") || count == 0) {
        return;
    }
    std::vector<float> rates(count);
    if (!xr_ok(enumerate_rates(session, count, &count, rates.data()), "xrEnumerateDisplayRefreshRatesFB")) {
        return;
    }
    const float target = debug_prop_float("debug.goemon.hz", interp_fps() > 0 ? kDisplayHzSmooth : kDisplayHz);
    float chosen = 0.0f;
    std::string listed;
    for (float rate : rates) {
        char text[16];
        std::snprintf(text, sizeof(text), "%s%.0f", listed.empty() ? "" : " ", rate);
        listed += text;
        if (std::fabs(rate - target) < 0.5f) {
            chosen = rate;
        }
    }
    if (chosen <= 0.0f) {
        vr_log("display rates %s Hz; %.0f Hz isn't one of them, keeping the default", listed.c_str(), target);
        return;
    }
    if (xr_ok(request_rate(session, chosen), "xrRequestDisplayRefreshRateFB")) {
        vr_log("display rates %s Hz; requested %.0f Hz", listed.c_str(), chosen);
    }
}

void on_session_state(XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_READY:
        if (!session_begun) {
            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (xr_ok(xr.BeginSession(session, &begin), "xrBeginSession")) {
                session_begun = true;
                set_controls_active(true);
                vr_log("session begun");
                request_refresh_rate();
                request_performance();
            }
        }
        break;
    case XR_SESSION_STATE_SYNCHRONIZED:
    case XR_SESSION_STATE_VISIBLE:
    case XR_SESSION_STATE_FOCUSED:
        set_controls_active(true);
        break;
    case XR_SESSION_STATE_STOPPING:
        set_controls_active(false);
        if (session_begun && xr.EndSession != nullptr) {
            xr.EndSession(session);
            session_begun = false;
            vr_log("session stopped");
        }
        break;
    case XR_SESSION_STATE_LOSS_PENDING:
    case XR_SESSION_STATE_EXITING:
        set_controls_active(false);
        session_begun = false;
        init_state = InitState::Failed;
        vr_log("OpenXR session ended. Staying flat.");
        break;
    default:
        break;
    }
}

void poll_events() {
    if (xr.PollEvent == nullptr || instance == XR_NULL_HANDLE) {
        return;
    }
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xr.PollEvent(instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            vr_log("session state %d", static_cast<int>(changed->state));
            on_session_state(changed->state);
        } else {
            // Anything else the runtime tells us (performance, refresh rate,
            // display changes) might line up with a recording starting.
            vr_log("OpenXR event type %d", static_cast<int>(event.type));
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void latch_trigger(bool& held, float value, std::atomic<uint32_t>& out) {
    if (!held && value >= kTriggerPress) {
        held = true;
    } else if (held && value <= kTriggerRelease) {
        held = false;
    }
    out.store(held ? 1u : 0u);
}

float clamp_stick(float value) {
    return std::clamp(value, -1.0f, 1.0f);
}

void sync_actions() {
    if (!actions_attached) {
        return;
    }
    XrActiveActionSet active{action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (!xr_ok(xr.SyncActions(session, &sync), "xrSyncActions")) {
        return;
    }
    auto read_bool = [](XrAction action, XrPath hand, std::atomic<uint32_t>& out) -> bool {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = action;
        get.subactionPath = hand;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xr.GetActionStateBoolean(session, &get, &state)) && state.isActive) {
            out.store(state.currentState ? 1u : 0u);
            return true;
        }
        out.store(0u);
        return false;
    };
    auto read_stick = [](XrAction action, XrPath hand, std::atomic<uint32_t>& out_x, std::atomic<uint32_t>& out_y) -> bool {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = action;
        get.subactionPath = hand;
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        float x = 0.0f;
        float y = 0.0f;
        bool active = false;
        if (XR_SUCCEEDED(xr.GetActionStateVector2f(session, &get, &state)) && state.isActive) {
            x = clamp_stick(state.currentState.x);
            y = clamp_stick(state.currentState.y);
            active = true;
        }
        out_x.store(float_bits(x));
        out_y.store(float_bits(y));
        return active;
    };
    auto read_trigger = [](XrAction action, XrPath hand, bool& held, std::atomic<uint32_t>& out) {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = action;
        get.subactionPath = hand;
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        float value = 0.0f;
        if (XR_SUCCEEDED(xr.GetActionStateFloat(session, &get, &state)) && state.isActive) {
            value = state.currentState;
        }
        latch_trigger(held, value, out);
    };
    const bool jump_active = read_bool(action_jump, path_hand_right, button_a);
    const bool throw_active = read_bool(action_throw, path_hand_right, button_b);
    const bool move_active = read_stick(action_move, path_hand_left, stick_lx_bits, stick_ly_bits);
    read_stick(action_snap, path_hand_right, stick_rx_bits, stick_ry_bits);
    // Left trigger R, right trigger Z (sub-weapon).
    read_trigger(action_detonate, path_hand_left, left_trigger_held, button_r);
    read_trigger(action_drop, path_hand_right, right_trigger_held, button_z);
    read_bool(action_start, path_hand_left, button_start);
    static bool left_grip_held = false;
    static bool right_grip_held = false;
    read_trigger(action_l, path_hand_left, left_grip_held, button_cright);   // left grip: the minimap
    read_trigger(action_cdown, path_hand_right, right_grip_held, button_cdown);
    read_bool(action_cup, path_hand_left, button_cup);
    read_bool(action_cleft, path_hand_left, button_cleft);
    // Right stick click: the camera goes straight behind Goemon.
    static std::atomic<uint32_t> behind_down{0};
    static bool behind_was_down = false;
    read_bool(action_cright, path_hand_right, behind_down);
    const bool behind_now = behind_down.load() != 0;
    if (behind_now && !behind_was_down) {
        body_reset_requested.store(true);
        vr_log("view: camera behind Goemon");
    }
    behind_was_down = behind_now;
    // Left stick click: first person on/off (off puts the camera behind Goemon).
    static std::atomic<uint32_t> view_toggle_down{0};
    static bool view_toggle_was_down = false;
    read_bool(action_view_toggle, path_hand_left, view_toggle_down);
    const bool view_toggle_now = view_toggle_down.load() != 0;
    if (view_toggle_now && !view_toggle_was_down) {
        const bool fp = !first_person_on.load();
        first_person_on.store(fp);
        vr_log("view: %s", fp ? "first person" : "chase camera behind Goemon");
    }
    view_toggle_was_down = view_toggle_now;
    static int sync_count = 0;
    static bool reported_touch = false;
    sync_count += 1;
    if (!reported_touch && (move_active || jump_active || throw_active || sync_count >= 90)) {
        vr_log("touch active move=%d jump=%d throw=%d after %d syncs", move_active ? 1 : 0, jump_active ? 1 : 0, throw_active ? 1 : 0, sync_count);
        reported_touch = true;
    }
}

bool locate_views() {
    views_located = false;
    XrView views[2]{};
    views[0].type = XR_TYPE_VIEW;
    views[1].type = XR_TYPE_VIEW;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = frame_state.predictedDisplayTime;
    locate.space = local_space;
    uint32_t count = 0;
    if (!xr_ok(xr.LocateViews(session, &locate, &view_state, 2, &count, views), "xrLocateViews") || count < 2) {
        return false;
    }
    eye_pose[0] = views[0].pose;
    eye_pose[1] = views[1].pose;
    HeadState head;
    {
        std::lock_guard<std::mutex> lock(head_mu);
        head = head_live;
    }
    if ((view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0) {
        const XrQuaternionf q = views[0].pose.orientation;
        const float qlen = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (qlen > 0.5f && qlen < 1.5f) {
            Quat unit{q.x / qlen, q.y / qlen, q.z / qlen, q.w / qlen};
            if (aim_ready.load() == 0u) {
                aim_ref_qx.store(float_bits(unit.x));
                aim_ref_qy.store(float_bits(unit.y));
                aim_ref_qz.store(float_bits(unit.z));
                aim_ref_qw.store(float_bits(unit.w));
            }
            Quat ref{bits_float(aim_ref_qx.load()), bits_float(aim_ref_qy.load()), bits_float(aim_ref_qz.load()), bits_float(aim_ref_qw.load())};
            Quat delta = quat_mul(unit, quat_inv(ref));
            const float dlen = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z + delta.w * delta.w);
            if (dlen > 0.5f && dlen < 1.5f) {
                delta.x /= dlen;
                delta.y /= dlen;
                delta.z /= dlen;
                delta.w /= dlen;
            }
            // Sideways look stays level. Pitch and roll from the headset were
            // tipping the horizon and making a turn feel like a fall.
            const float yaw = vr_yaw_from_quat(delta.x, delta.y, delta.z, delta.w);
            const float half_yaw = yaw * 0.5f;
            const Quat yaw_only{0.0f, std::sin(half_yaw), 0.0f, std::cos(half_yaw)};
            Quat level = quat_mul(yaw_only, ref);
            const float llen = std::sqrt(level.x * level.x + level.y * level.y + level.z * level.z + level.w * level.w);
            if (llen > 0.5f && llen < 1.5f) {
                level.x /= llen;
                level.y /= llen;
                level.z /= llen;
                level.w /= llen;
            }
            store_aim_quat(level);
            aim_ready.store(1u);
            head_yaw_bits.store(float_bits(yaw * (180.0f / 3.1415926535f)));
            head_pitch_bits.store(0u);
            {
                // Where the head points up/down, measured from level (not from
                // where it pointed at startup), so level means level.
                float hx = 0.0f;
                float hy = 0.0f;
                float hz = -1.0f;
                quat_rotate(unit, hx, hy, hz);
                head_pitch_abs_bits.store(float_bits(std::asin(std::clamp(hy, -1.0f, 1.0f))));
            }
            // The layer orientation is built per frame from the yaw and pitch the
            // game actually drew with (see world_layer), not from this sample.
            // The rendered camera does not move with the head. A moving layer
            // position makes the runtime slide the picture when you look around.
            const float mid_x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
            const float mid_y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
            const float mid_z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
            if (!head.anchor_set && (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0) {
                head.anchor.x = mid_x;
                head.anchor.y = mid_y;
                head.anchor.z = mid_z;
                head.anchor_set = true;
                vr_log("headset position locked, world stays still");
            }
            // Where the head is and which way it faces, for placing the menu screen.
            head.yaw_abs = vr_yaw_from_quat(unit.x, unit.y, unit.z, unit.w);
            head.mid = XrVector3f{mid_x, mid_y, mid_z};
            head.valid = (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        }
    }
    // One angle for both eyes, wide enough to cover the headset. The old
    // overlap left a black border, which is the floating window.
    float half_h = std::max(-views[0].fov.angleLeft, views[0].fov.angleRight);
    half_h = std::max(half_h, std::max(-views[1].fov.angleLeft, views[1].fov.angleRight));
    float half_v = std::max(views[0].fov.angleUp, -views[0].fov.angleDown);
    half_v = std::max(half_v, std::max(views[1].fov.angleUp, -views[1].fov.angleDown));
    if (half_h > 0.25f && half_h < 1.5f && half_v > 0.25f && half_v < 1.5f) {
        shared_fov.angleLeft = -half_h;
        shared_fov.angleRight = half_h;
        shared_fov.angleUp = half_v;
        shared_fov.angleDown = -half_v;
        // Draw a little past the panel edge. The compositor turns the last
        // picture to follow your head until the next game frame (30 fps here);
        // without spare picture at the edges that turn shows black, which
        // flickers at the corners of your view.
        const float margin = (RT64::GetGpuHangFallback() ? kOverscanFallbackDeg : kOverscanDeg) * (3.1415926535f / 180.0f);
        // The top rows and left columns the sky can leave unpainted are cut
        // off the layer (see kSkyGapTopPx), so draw that much wider to keep
        // the full margin on those sides too.
        const float keep_v = 1.0f - 2.0f * (kSkyGapTopPx / 240.0f);
        const float keep_h = 1.0f - 2.0f * (kSkyGapLeftPx / 320.0f);
        const float wide_h = std::atan(std::tan(std::min(half_h + margin, 1.35f)) / keep_h);
        const float wide_v = std::atan(std::tan(std::min(half_v + margin, 1.35f)) / keep_v);
        head.world_fov.angleLeft = -wide_h;
        head.world_fov.angleRight = wide_h;
        head.world_fov.angleUp = wide_v;
        head.world_fov.angleDown = -wide_v;
        head.proj_x_scale = 1.0f / std::tan(wide_h);
        head.proj_y_scale = 1.0f / std::tan(wide_v);
        if (!head.fov_ready) {
            vr_log("shared fov %.0f x %.0f deg, both eyes, drawn at %.0f x %.0f",
                half_h * 2.0f * (180.0f / 3.1415926535f),
                half_v * 2.0f * (180.0f / 3.1415926535f),
                wide_h * 2.0f * (180.0f / 3.1415926535f),
                wide_v * 2.0f * (180.0f / 3.1415926535f));
        }
        head.fov_ready = true;
    }
    {
        std::lock_guard<std::mutex> lock(head_mu);
        head_live = head;
    }

    if ((view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0) {
        float yaw_rad = vr_yaw_from_quat(views[0].pose.orientation.x, views[0].pose.orientation.y, views[0].pose.orientation.z, views[0].pose.orientation.w);
        float head_right_x = std::cos(yaw_rad);
        float head_right_z = -std::sin(yaw_rad);
        float mid_x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
        float mid_z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
        for (int eye = 0; eye < 2; ++eye) {
            float dx = views[eye].pose.position.x - mid_x;
            float dz = views[eye].pose.position.z - mid_z;
            eye_offset_m[eye] = dx * head_right_x + dz * head_right_z;
        }
    } else {
        eye_offset_m[0] = -0.032f;
        eye_offset_m[1] = 0.032f;
    }
    views_located = true;
    return true;
}

bool format_supports(VkFormat format, VkFormatFeatureFlags needed) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(vk_device->physicalDevice, format, &properties);
    return (properties.optimalTilingFeatures & needed) == needed;
}

bool choose_format() {
    if (swap_format != 0) {
        return true;
    }
    uint32_t count = 0;
    if (!xr_ok(xr.EnumerateSwapchainFormats(session, 0, &count, nullptr), "xrEnumerateSwapchainFormats")) {
        return false;
    }
    std::vector<int64_t> formats(count);
    if (!xr_ok(xr.EnumerateSwapchainFormats(session, count, &count, formats.data()), "xrEnumerateSwapchainFormats")) {
        return false;
    }
    const int64_t preferred[] = {
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_R8G8B8A8_SRGB,
    };
    for (int64_t want : preferred) {
        if (std::find(formats.begin(), formats.end(), want) == formats.end()) {
            continue;
        }
        const bool blit = want != VK_FORMAT_B8G8R8A8_UNORM;
        const VkFormatFeatureFlags needed = blit ? VK_FORMAT_FEATURE_BLIT_DST_BIT : VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if (!format_supports(static_cast<VkFormat>(want), needed)) {
            continue;
        }
        swap_format = want;
        swap_needs_blit = blit;
        vr_log("swapchain format %d%s", static_cast<int>(swap_format), blit ? " (blit)" : "");
        return true;
    }
    vr_log("No usable color swapchain format.");
    for (int64_t format : formats) {
        vr_log("available format %d", static_cast<int>(format));
    }
    return false;
}

bool create_swapchain(EyeChain& eye, uint32_t width, uint32_t height) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    // SAMPLED is what lets the Quest compositor read the eye image.
    // Without it the layer submits and the headset stays black.
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = swap_format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    XrResult created = XR_ERROR_RUNTIME_FAILURE;
    {
        auto queue_lock = lock_xr_queue();
        created = xr.CreateSwapchain(session, &info, &eye.swapchain);
    }
    if (!xr_ok(created, "xrCreateSwapchain")) {
        return false;
    }
    uint32_t count = 0;
    if (!xr_ok(xr.EnumerateSwapchainImages(eye.swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages")) {
        return false;
    }
    XrSwapchainImageVulkanKHR image{};
    image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    eye.images.assign(count, image);
    if (!xr_ok(xr.EnumerateSwapchainImages(eye.swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data())), "xrEnumerateSwapchainImages")) {
        return false;
    }
    eye.width = width;
    eye.height = height;
    eye.acquired = -1;
    eye.wait_pending = false;
    return true;
}

// ---- picture slots ----------------------------------------------------------
//
// Four slots, each with its own swapchain; both eyes show the same image.
// RT64's present thread copies each picture into a slot the headset isn't
// showing and marks it ready (on_presented_frame); the frame thread puts it up
// at the right refresh. A swapchain shows its last released image, so the
// picture on screen can't change under the compositor while the next one is
// copied. With smooth motion a game frame brings two pictures at once, so the
// slots are a small queue: shown in the order they were made, each for its
// share of refreshes (Wave Race 64 port, 0.1.1 and 0.1.4).
constexpr int kSlots = 4;
struct Slot {
    EyeChain chain;
    LayerDesc desc;
    bool writing = false;   // a picture is being copied into it
    bool ready = false;     // a copied picture the headset hasn't shown yet
    bool world = false;
    uint64_t seq = 0;       // order the pictures were made in
    uint32_t count = 1;     // pictures its game frame brought (2 with smooth motion)
};
std::mutex slot_mu;
Slot slots[kSlots];
int front_slot = -1;        // the slot the headset shows (-1: none yet)
uint64_t slot_seq = 0;

// Pictures copied but not shown yet. slot_mu held.
int waiting_pictures() {
    int waiting = 0;
    for (int i = 0; i < kSlots; ++i) {
        waiting += (slots[i].ready && i != front_slot) ? 1 : 0;
    }
    return waiting;
}

// A slot to copy the next picture into: a free one, or else the oldest picture
// not shown yet (the queue is behind; dropping it keeps the delay short).
int claim_slot() {
    std::lock_guard<std::mutex> lock(slot_mu);
    int oldest = -1;
    for (int i = 0; i < kSlots; ++i) {
        if (i == front_slot || slots[i].writing) {
            continue;
        }
        if (!slots[i].ready) {
            slots[i].writing = true;
            return i;
        }
        if (oldest < 0 || slots[i].seq < slots[oldest].seq) {
            oldest = i;
        }
    }
    if (oldest >= 0) {
        slots[oldest].ready = false;
        slots[oldest].writing = true;
    }
    return oldest;
}

bool create_slots() {
    if (!choose_format()) {
        return false;
    }
    for (Slot& slot : slots) {
        if (!create_swapchain(slot.chain, eye_width, eye_height)) {
            return false;
        }
    }
    vr_log("picture swapchains %d x %ux%u", kSlots, eye_width, eye_height);
    return true;
}

void release_swapchain(XrSwapchain swapchain) {
    if (swapchain == XR_NULL_HANDLE || xr.ReleaseSwapchainImage == nullptr) {
        return;
    }
    XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    auto queue_lock = lock_xr_queue();
    xr.ReleaseSwapchainImage(swapchain, &info);
}

// Acquires the next image of a slot's swapchain and waits until the
// compositor is done with it. Present thread only.
bool acquire_slot_image(EyeChain& chain) {
    if (!chain.wait_pending) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t index = 0;
        XrResult acquired = XR_ERROR_RUNTIME_FAILURE;
        {
            auto queue_lock = lock_xr_queue();
            acquired = xr.AcquireSwapchainImage(chain.swapchain, &acquire, &index);
        }
        if (!xr_ok(acquired, "xrAcquireSwapchainImage")) {
            return false;
        }
        chain.pending_index = index;
        chain.wait_pending = true;
    }
    // XR_TIMEOUT_EXPIRED counts as "success" to XR_SUCCEEDED, but it means the
    // compositor is still showing this image (it is slower while the Quest
    // records). Writing into it then tears or flickers the picture. Wait in
    // short steps; if it still isn't free, skip this picture and finish the
    // wait next time instead of writing early.
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 100000000LL;  // 100 ms
    XrResult result = XR_TIMEOUT_EXPIRED;
    for (int attempt = 0; attempt < 5 && result == XR_TIMEOUT_EXPIRED; ++attempt) {
        result = xr.WaitSwapchainImage(chain.swapchain, &wait);
    }
    if (result == XR_TIMEOUT_EXPIRED) {
        static int logs = 0;
        if (logs < 5) {
            ++logs;
            vr_log("picture image still in use by the compositor, skipping a picture");
        }
        return false;
    }
    chain.wait_pending = false;
    if (result != XR_SUCCESS) {
        xr_ok(result, "xrWaitSwapchainImage");
        release_swapchain(chain.swapchain);
        return false;
    }
    const uint32_t index = chain.pending_index;
    if (index >= chain.images.size() || chain.images[index].image == VK_NULL_HANDLE) {
        release_swapchain(chain.swapchain);
        return false;
    }
    chain.acquired = static_cast<int32_t>(index);
    return true;
}

bool wait_frame() {
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    frame_state = {XR_TYPE_FRAME_STATE};
    XrResult waited = XR_ERROR_RUNTIME_FAILURE;
    {
        static int slow_budget = 20;
        SlowCallLog slow{"xrWaitFrame", 150.0, &slow_budget};
        waited = xr.WaitFrame(session, &wait, &frame_state);
    }
    return xr_ok(waited, "xrWaitFrame");
}

bool begin_frame() {
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    XrResult begun = XR_ERROR_RUNTIME_FAILURE;
    {
        static int slow_budget = 20;
        SlowCallLog slow{"xrBeginFrame", 50.0, &slow_budget};
        auto queue_lock = lock_xr_queue();
        begun = xr.BeginFrame(session, &begin);
    }
    return xr_ok(begun, "xrBeginFrame");
}

float read_mtx(const uint8_t* mtx, int row, int col) {
    const int stored = col ^ 1;
    int16_t integer = 0;
    uint16_t frac = 0;
    std::memcpy(&integer, mtx + (row * 4 + stored) * 2, sizeof(integer));
    std::memcpy(&frac, mtx + 32 + (row * 4 + stored) * 2, sizeof(frac));
    const int32_t fixed = (static_cast<int32_t>(integer) << 16) | static_cast<int32_t>(frac);
    return static_cast<float>(fixed) / 65536.0f;
}

void write_mtx(uint8_t* mtx, int row, int col, float value) {
    const int stored = col ^ 1;
    const int32_t fixed = static_cast<int32_t>(std::lround(value * 65536.0f));
    const int16_t integer = static_cast<int16_t>(fixed >> 16);
    const uint16_t frac = static_cast<uint16_t>(fixed & 0xFFFF);
    std::memcpy(mtx + (row * 4 + stored) * 2, &integer, sizeof(integer));
    std::memcpy(mtx + 32 + (row * 4 + stored) * 2, &frac, sizeof(frac));
}

// Widens the world projection of the frame being drawn. proj_phys comes from
// that frame's own task, never from D_8016E104: with two frames in flight the
// game has often moved D_8016E104 on to the next frame by now, and patching
// that one left this frame at the game's 50 degrees (a zoom flash at the edges).
void widen_game_projection(uint8_t* rdram, uint32_t proj_phys, bool first_person_frame) {
    if (rdram == nullptr || proj_phys == 0u || proj_phys > 0x1FFFFF00u) {
        return;
    }
    uint8_t* mtx = vram_bytes(rdram, 0x80000000u | proj_phys);
    const float y_scale = read_mtx(mtx, 1, 1);
    const float clip_w = read_mtx(mtx, 2, 3);
    // guPerspective writes y scale in [1][1] and -1 in [2][3]. Leave ortho HUD matrices alone.
    if (clip_w > -0.5f || y_scale < 0.3f || y_scale > 8.0f) {
        return;
    }
    // RT64 multiplies X by 1/rt64_aspect_scale() afterwards, so pre-divide here.
    const float x_scale = proj_x_scale * rt64_aspect_scale();
    write_mtx(mtx, 0, 0, x_scale);
    write_mtx(mtx, 1, 1, proj_y_scale);
    if (first_person_frame) {
        // The game clips everything closer than 100 units, which from Bomberman's
        // head cuts away the floor at his feet. guPerspective stores
        // [2][2] = (n+f)/(n-f) and [3][2] = 2nf/(n-f); recover n and f, then
        // rebuild both for a nearer n.
        const float a = read_mtx(mtx, 2, 2);
        const float b = read_mtx(mtx, 3, 2);
        if (std::fabs(a - 1.0f) > 1.0e-4f && std::fabs(a + 1.0f) > 1.0e-4f) {
            const float n = b / (a - 1.0f);
            const float f = b / (a + 1.0f);
            const float nn = kFirstPersonNearPlane;
            if (n > nn && f > n) {
                write_mtx(mtx, 2, 2, (nn + f) / (nn - f));
                write_mtx(mtx, 3, 2, (2.0f * nn * f) / (nn - f));
                static bool logged_near = false;
                if (!logged_near) {
                    vr_log("first person near plane %.0f -> %.0f (far %.0f)", n, nn, f);
                    logged_near = true;
                }
            }
        }
    }
    static bool logged = false;
    if (!logged) {
        vr_log("world projection scale %.2f %.2f (x before rt64 aspect %.2f, was y %.2f)", x_scale, proj_y_scale, proj_x_scale, y_scale);
        logged = true;
    }
}

Quat yaw_pitch_quat(float yaw, float pitch) {
    const Quat q_yaw{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    const Quat q_pitch{std::sin(pitch * 0.5f), 0.0f, 0.0f, std::cos(pitch * 0.5f)};
    return quat_mul(q_yaw, q_pitch);
}

XrQuaternionf to_xr(Quat q) {
    return XrQuaternionf{q.x, q.y, q.z, q.w};
}

void push_frame_record(FrameRecord rec) {
    std::lock_guard<std::mutex> lock(record_mu);
    rec.seq = ++record_seq;
    FrameRecord* oldest = &frame_records[0];
    for (FrameRecord& slot : frame_records) {
        if (slot.seq < oldest->seq) {
            oldest = &slot;
        }
    }
    *oldest = rec;
}

// A display list is at most this long (Goemon's are far shorter).
constexpr uint32_t kMaxDisplayListBytes = 0x80000u;

// The cameras aim_camera aimed for this task: those drawn into its display list
// (between its start and kMaxDisplayListBytes past it) since the previous task.
// Every one's projection is widened; the pose comes from the last one.
bool take_frame_record(uint8_t* rdram, const OSTask* task, FrameRecord& out) {
    if (task == nullptr) {
        return false;
    }
    const uint32_t data_phys = static_cast<uint32_t>(task->t.data_ptr) & 0x1FFFFFFFu;
    std::lock_guard<std::mutex> lock(record_mu);
    bool found = false;
    uint64_t newest = 0;
    for (FrameRecord& slot : frame_records) {
        if (!slot.fresh || slot.seq <= record_taken_seq || slot.dl_phys < data_phys || slot.dl_phys - data_phys >= kMaxDisplayListBytes) {
            continue;
        }
        widen_game_projection(rdram, slot.proj_phys, false);
        slot.fresh = false;
        if (!found || slot.seq > out.seq) {
            out = slot;
        }
        found = true;
        newest = std::max(newest, slot.seq);
    }
    bool by_order = false;
    auto in_other_list = [data_phys](uint32_t dl) {
        for (uint32_t list : recent_lists) {
            if (list != 0u && list != data_phys && dl >= list && dl - list < kMaxDisplayListBytes) {
                return true;
            }
        }
        return false;
    };
    if (!found) {
        // Not in this list's range (the list might call into another buffer):
        // the oldest camera aimed since the previous task is this frame's,
        // unless it was drawn into another task's list (the next frame's).
        FrameRecord* oldest = nullptr;
        for (FrameRecord& slot : frame_records) {
            if (slot.fresh && slot.seq > record_taken_seq && !in_other_list(slot.dl_phys) && (oldest == nullptr || slot.seq < oldest->seq)) {
                oldest = &slot;
            }
        }
        if (oldest != nullptr) {
            widen_game_projection(rdram, oldest->proj_phys, false);
            oldest->fresh = false;
            out = *oldest;
            newest = oldest->seq;
            found = true;
            by_order = true;
        }
    }
    if (found) {
        record_taken_seq = newest;
    }
    bool known = false;
    for (uint32_t list : recent_lists) {
        known = known || list == data_phys;
    }
    if (!known) {
        std::memmove(recent_lists + 1, recent_lists, sizeof(recent_lists) - sizeof(recent_lists[0]));
        recent_lists[0] = data_phys;
    }
    static int logs = 0;
    static uint32_t range_hits = 0;
    static uint32_t order_hits = 0;
    static uint32_t misses = 0;
    (found ? (by_order ? order_hits : range_hits) : misses)++;
    if (logs < 6) {
        ++logs;
        vr_log("task list %08X: %s", data_phys, !found ? "no level camera (menu screen)" : by_order ? "level camera found by order" : "level camera found in its list");
    }
    if (range_hits + order_hits + misses >= 1800) {
        vr_log("frame pairing: %u by list, %u by order, %u without a level camera", range_hits, order_hits, misses);
        range_hits = order_hits = misses = 0;
    }
    return found;
}

// Gameplay: the picture covers your whole view and is turned to the yaw and
// pitch the game camera drew it with, so the level stays still around you.
LayerDesc world_layer(const FrameRecord& rec) {
    const Quat ref{bits_float(aim_ref_qx.load()), bits_float(aim_ref_qy.load()), bits_float(aim_ref_qz.load()), bits_float(aim_ref_qw.load())};
    const float ref_yaw = vr_yaw_from_quat(ref.x, ref.y, ref.z, ref.w);
    const float pitch = kLayerFollowsCameraPitch ? rec.pitch : 0.0f;
    LayerDesc desc;
    desc.valid = true;
    desc.quad = false;
    desc.pose.orientation = to_xr(yaw_pitch_quat(ref_yaw + rec.yaw, pitch));
    // Where the picture was drawn from: the head's position at the aim (the eye
    // follows head movement, 6DOF). A layer left at the start position while the
    // picture moves with the head would be moved a second time by the runtime.
    desc.pose.position = rec.has_head_pos ? rec.head_pos : (aim_anchor_set ? aim_anchor : head_mid);
    desc.fov = world_fov;
    return desc;
}

// Title, menus and pause: a flat screen hanging in the room in front of where
// you were looking when it appeared. It stays put, so it doesn't swim when you move.
LayerDesc screen_layer(bool entering) {
    if ((entering || !screen_placed) && head_valid) {
        const float s = std::sin(head_yaw_abs);
        const float c = std::cos(head_yaw_abs);
        screen_pose.orientation = to_xr(yaw_pitch_quat(head_yaw_abs, 0.0f));
        screen_pose.position = XrVector3f{head_mid.x - kScreenDistanceM * s, head_mid.y, head_mid.z - kScreenDistanceM * c};
        if (!screen_placed) {
            vr_log("menu screen placed %.1f m ahead", kScreenDistanceM);
        }
        screen_placed = true;
    }
    LayerDesc desc;
    desc.valid = true;
    desc.quad = true;
    desc.pose = screen_pose;
    return desc;
}

// RT64 draws levels at the eye image's shape and size, and menus at the game's
// own 4:3 (so nothing at the sides gets cut off) at the old 2208 lines.
// Only a mode change rewrites the config.
void set_rt64_config(RT64::Application* app, bool world) {
    if (app == nullptr) {
        return;
    }
    const double target = world ? world_aspect() : kScreenAspectTarget;
    // Menus: as many lines as kScreenResolutionMultiplier allows while still
    // fitting the eye image, so the picture copies across 1:1 without rescaling.
    const double screen_multiplier = std::min({kScreenResolutionMultiplier,
        static_cast<double>(eye_width) / 320.0, static_cast<double>(eye_height) / 240.0});
    const bool hang_fallback = RT64::GetGpuHangFallback();
    const double multiplier = (world ? static_cast<double>(eye_height) / 240.0 : screen_multiplier)
        * (hang_fallback ? kHangFallbackResolution : 1.0);
    RT64::UserConfiguration& cfg = app->userConfig;
    // Menus keep RT64's own refresh and 2D settings, exactly as when they
    // worked; only levels get the VR ones below.
    static bool saved_menu_settings = false;
    static RT64::UserConfiguration::RefreshRate menu_refresh = RT64::UserConfiguration::RefreshRate::Original;
    static RT64::UserConfiguration::Upscale2D menu_upscale2d = RT64::UserConfiguration::Upscale2D::ScaledOnly;
    if (!saved_menu_settings) {
        menu_refresh = cfg.refreshRate;
        menu_upscale2d = cfg.upscale2D;
        saved_menu_settings = true;
    }
    // Levels, sharp picture: no in-between frames, the game's own 30 a second.
    // Smooth motion: RT64 draws an in-between picture for every game frame
    // (60 a second), and every one goes to the headset (on_presented_frame).
    // It costs a full frame of GPU each, hence the smaller picture. Off while
    // the GPU hang guard has RT64 on the slower ubershader.
    const int interp = (world && !hang_fallback) ? interp_fps() : 0;
    const auto refresh = !world ? menu_refresh
        : interp > 0 ? RT64::UserConfiguration::RefreshRate::Manual : RT64::UserConfiguration::RefreshRate::Original;
    // Levels: draw 2D at full resolution. RT64 normally samples 2D on the
    // N64's own pixel grid (about 11 screen pixels per N64 pixel here); with
    // the HUD pulled into the middle that grid no longer lines up and it
    // skipped most of the HUD's texels, which made it unreadable.
    const auto upscale2d = world ? RT64::UserConfiguration::Upscale2D::All : menu_upscale2d;
    if (cfg.aspectRatio == RT64::UserConfiguration::AspectRatio::Manual
        && cfg.extAspectRatio == RT64::UserConfiguration::AspectRatio::Manual
        && cfg.resolution == RT64::UserConfiguration::Resolution::Manual
        && std::fabs(cfg.aspectTarget - target) < 1.0e-6
        && std::fabs(cfg.extAspectTarget - target) < 1.0e-6
        && std::fabs(cfg.resolutionMultiplier - multiplier) < 1.0e-6
        && cfg.refreshRate == refresh
        && (interp == 0 || cfg.refreshRateTarget == interp)
        && cfg.upscale2D == upscale2d) {
        return;
    }
    cfg.aspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
    cfg.aspectTarget = target;
    cfg.extAspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
    cfg.extAspectTarget = target;
    cfg.resolution = RT64::UserConfiguration::Resolution::Manual;
    cfg.resolutionMultiplier = multiplier;
    cfg.downsampleMultiplier = 1;
    cfg.refreshRate = refresh;
    if (interp > 0) {
        cfg.refreshRateTarget = interp;
    }
    cfg.upscale2D = upscale2d;
    app->updateUserConfig(true);
    vr_log("rt64 %.0fx%.0f (aspect %.3f) for %s%s, refresh mode %d%s", 320.0 * multiplier * target * 0.75, 240.0 * multiplier, target,
        world ? "the level" : "the menu screen", hang_fallback ? " (lower, GPU hang guard)" : "",
        static_cast<int>(refresh), interp > 0 ? " (60 pictures a second)" : "");
}

// How far RT64 should pull screen-space 2D toward the middle of a level frame
// so the game's 4:3 HUD layout spans kHudHalfHeightDeg up and down, in its own
// 4:3 shape. RT64 maps the 320 x 240 HUD layout onto square pixels the full
// image height tall (its X is undone by the aspect setting), so:
//   vertical:   scale = tan(hud) / tan(image top)
//   horizontal: scale = tan(hud) * (W / H) / tan(image right)
void set_hud_inset(bool world) {
    if (!world) {
        RT64::SetVrRectInset(1.0f, 1.0f, 0.0f);
        return;
    }
    static uint32_t prop_reads = 0;
    static float drop_deg = kHudDropDeg;
    static float size_deg = kHudHalfHeightDeg;
    if ((prop_reads++ % 60u) == 0u) {
        const int drop_prop = debug_prop_int("debug.goemon.huddown", -1000);
        const int size_prop = debug_prop_int("debug.goemon.hudsize", -1000);
        const float next_drop = drop_prop == -1000 ? kHudDropDeg : static_cast<float>(std::clamp(drop_prop, -30, 45));
        const float next_size = size_prop == -1000 ? kHudHalfHeightDeg : static_cast<float>(std::clamp(size_prop, 10, 50));
        if (next_drop != drop_deg || next_size != size_deg) {
            drop_deg = next_drop;
            size_deg = next_size;
            vr_log("HUD box %.0f degrees half height, middle %.0f degrees below the view's middle", size_deg, drop_deg);
        }
    }
    const float t_box = std::tan(size_deg * (3.1415926535f / 180.0f));
    const float t_up = std::tan(world_fov.angleUp);
    const float t_right = std::tan(world_fov.angleRight);
    if (t_up <= t_box || t_right <= 0.0f) {
        RT64::SetVrRectInset(1.0f, 1.0f, 0.0f);
        return;
    }
    const float scale_y = t_box / t_up;
    const float scale_x = std::min(1.0f, t_box * static_cast<float>(world_aspect()) / t_right);
    // Moved down by the drop, kept inside the picture (the layer spans +-t_up).
    float t_drop = std::tan(drop_deg * (3.1415926535f / 180.0f));
    t_drop = std::clamp(t_drop, -(t_up - t_box), t_up - t_box);
    RT64::SetVrRectInset(scale_x, scale_y, t_drop / (2.0f * t_up));
}

// Match the layer to the picture RT64 actually drew and where it landed in the
// eye image. RT64's settings land a frame late after a mode switch, and the
// picture's shape tells us which aspect it used (the render target is 320 x 240
// scaled, with width scaled by aspect / (4:3)). The rows and columns the sky
// can leave unpainted are cut off here (see kSkyGapTopPx).
void fit_layer_to_image(LayerDesc& desc, float image_aspect, uint32_t rect_w, uint32_t rect_h) {
    if (image_aspect < 0.2f || image_aspect > 4.0f || rect_w < 64u || rect_h < 64u) {
        return;
    }
    const int32_t cut_top = static_cast<int32_t>(std::ceil(static_cast<float>(rect_h) * (kSkyGapTopPx / 240.0f)));
    const int32_t cut_left = static_cast<int32_t>(std::ceil(static_cast<float>(rect_w) * (kSkyGapLeftPx / 320.0f)));
    const float keep_v = static_cast<float>(static_cast<int32_t>(rect_h) - cut_top) / static_cast<float>(rect_h);
    const float keep_h = static_cast<float>(static_cast<int32_t>(rect_w) - cut_left) / static_cast<float>(rect_w);
    desc.rect.offset = XrOffset2Di{cut_left, cut_top};
    desc.rect.extent = XrExtent2Di{static_cast<int32_t>(rect_w) - cut_left, static_cast<int32_t>(rect_h) - cut_top};
    if (desc.quad) {
        desc.quad_width = kScreenHeightM * image_aspect * keep_h / keep_v;
        return;
    }
    // The projection was widened for the level aspect. At another aspect RT64
    // scales X by world_aspect/aspect less, so the picture spans that much more.
    const float spread = image_aspect / static_cast<float>(world_aspect());
    const float t_right = std::tan(desc.fov.angleRight) * spread;
    const float t_left = -t_right;
    const float t_up = std::tan(desc.fov.angleUp);
    const float t_down = std::tan(desc.fov.angleDown);
    // Rows and columns are evenly spaced in tangent space, so the cut moves the
    // top and left edges in by the same share of the span.
    desc.fov.angleRight = std::atan(t_right);
    desc.fov.angleLeft = std::atan(t_left + (t_right - t_left) * (1.0f - keep_h));
    desc.fov.angleDown = std::atan(t_down);
    desc.fov.angleUp = std::atan(t_up - (t_up - t_down) * (1.0f - keep_v));
}

// The part of the eye image to show; the whole image if fit_layer_to_image never ran.
XrRect2Di layer_rect(const LayerDesc& desc, const EyeChain& eye) {
    if (desc.rect.extent.width > 0 && desc.rect.extent.height > 0) {
        return desc.rect;
    }
    return XrRect2Di{{0, 0}, {static_cast<int32_t>(eye.width), static_cast<int32_t>(eye.height)}};
}

void end_frame(const LayerDesc* desc, XrSwapchain swapchain, const EyeChain* chain) {
    XrCompositionLayerProjectionView proj_views[2]{};
    XrCompositionLayerProjection proj_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    // Ask the compositor to filter the level picture down to the panel
    // properly. Levels only: on the menu screen quad it left the title, menus
    // and pause black.
    XrCompositionLayerSettingsFB layer_settings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
    layer_settings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB;
    if (layer_settings_ext && (debug_flags.load() & kDebugNoSuperSampling) == 0) {
        proj_layer.next = &layer_settings;
    }
    const XrCompositionLayerBaseHeader* headers[1] = {};
    if (desc != nullptr && desc->valid && swapchain != XR_NULL_HANDLE && chain != nullptr) {
        if (desc->quad) {
            // The picture shown once, in depth.
            quad_layer.space = local_space;
            quad_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad_layer.subImage.swapchain = swapchain;
            quad_layer.subImage.imageRect = layer_rect(*desc, *chain);
            quad_layer.pose = desc->pose;
            quad_layer.size = XrExtent2Df{desc->quad_width, kScreenHeightM};
            headers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad_layer);
        } else {
            // Same image, pose and fov for both eyes: the pose the picture was drawn for.
            for (int eye = 0; eye < 2; ++eye) {
                proj_views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                proj_views[eye].pose = desc->pose;
                proj_views[eye].fov = desc->fov;
                proj_views[eye].subImage.swapchain = swapchain;
                proj_views[eye].subImage.imageRect = layer_rect(*desc, *chain);
            }
            proj_layer.space = local_space;
            proj_layer.viewCount = 2;
            proj_layer.views = proj_views;
            headers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj_layer);
        }
    }
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = frame_state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (headers[0] != nullptr && frame_state.shouldRender) {
        end.layerCount = 1;
        end.layers = headers;
    }
    XrResult ended = XR_ERROR_RUNTIME_FAILURE;
    {
        static int slow_budget = 20;
        SlowCallLog slow{"xrEndFrame", 50.0, &slow_budget};
        auto queue_lock = lock_xr_queue();
        ended = xr.EndFrame(session, &end);
    }
    xr_ok(ended, "xrEndFrame");
}

// Frame pacing. At 90 Hz and the game's 30 fps every picture should stay on
// screen for exactly 3 refreshes; with smooth motion (60 pictures a second at
// 120 Hz) exactly 2. Pictures arrive a little unevenly (more so while the Quest
// records), and then some were shown 2 times and the next 4: judder. The game's
// frame rate is measured from the game frames handed over (not from pictures,
// so the hold doesn't fall back to 1 while the rate moves from 30 to 60 at a
// level's start, Wave Race 0.1.4), and a picture of a frame that brought N
// pictures stays up 1/N of that frame's refreshes.
// Live switch: `adb shell setprop debug.goemon.pace off` turns it off.
struct Pacing {
    uint32_t hold = 1;
    double base = 3.0;                     // refreshes per game frame (unrounded)
    double game_fps = 30.0;
    std::chrono::steady_clock::time_point last_check = std::chrono::steady_clock::now();
    XrTime front_since = 0;                // display time the shown picture first went up
    uint32_t shown[8] = {};                // pictures shown 1x .. 7x+ since the last log line
    uint32_t pictures = 0;
    uint32_t frames = 0;
    XrTime backlog_since = 0;              // a frame or more has waited behind the next picture since then
    uint32_t skipped = 0;                  // pictures skipped to catch up, since the last log line
};
Pacing pacing;
std::atomic<uint32_t> picture_arrivals{0};
// When the last game frames arrived (their first picture), for the median gap.
std::mutex arrival_mu;
std::chrono::steady_clock::time_point arrival_times[24];
uint32_t arrival_next = 0;
uint32_t arrival_count = 0;

void note_arrival() {
    picture_arrivals.fetch_add(1);
    std::lock_guard<std::mutex> lock(arrival_mu);
    arrival_times[arrival_next] = std::chrono::steady_clock::now();
    arrival_next = (arrival_next + 1u) % static_cast<uint32_t>(std::size(arrival_times));
    arrival_count = std::min<uint32_t>(arrival_count + 1u, static_cast<uint32_t>(std::size(arrival_times)));
}

// The typical time between game frames over the last second (median gap), so
// one stall (RT64 rebuilding its targets when the game switches between the
// level and a menu frame) doesn't change the hold: counting frames per half
// second made it 4 refreshes for half a second there, then skip to catch up.
// 0 when fewer than 6 frames came in that second.
double median_frame_gap(std::chrono::steady_clock::time_point now) {
    std::vector<double> gaps;
    std::lock_guard<std::mutex> lock(arrival_mu);
    const uint32_t size = static_cast<uint32_t>(std::size(arrival_times));
    for (uint32_t i = 1; i < arrival_count; ++i) {
        const auto& newer = arrival_times[(arrival_next + size - i) % size];
        const auto& older = arrival_times[(arrival_next + size - i - 1u) % size];
        if (now - newer > std::chrono::seconds(1)) {
            break;
        }
        gaps.push_back(std::chrono::duration<double>(newer - older).count());
    }
    if (gaps.size() < 6) {
        return 0.0;
    }
    std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
    return gaps[gaps.size() / 2];
}

// `debug.goemon.pacetrace N` (launch): logs the next N level pictures' arrivals
// and switches with times, to see where uneven holds come from.
std::atomic<int> pace_trace_left{-1};
bool pace_trace() {
    int left = pace_trace_left.load();
    if (left < 0) {
        left = debug_prop_int("debug.goemon.pacetrace", 0);
        pace_trace_left.store(std::max(left, 0));
    }
    return left > 0;
}
double trace_ms() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void update_pacing(XrDuration period) {
    const auto now = std::chrono::steady_clock::now();
    const double since = std::chrono::duration<double>(now - pacing.last_check).count();
    if (since < 0.5) {
        return;
    }
    const uint32_t arrivals = picture_arrivals.exchange(0);
    const double gap = median_frame_gap(now);
    if (gap > 0.0) {
        pacing.game_fps = 1.0 / gap;
    } else {
        // Hardly any frames (loading): the old count, so a hold doesn't
        // outlast a slow stretch.
        pacing.game_fps = pacing.game_fps * 0.4 + (arrivals / since) * 0.6;
    }
    pacing.last_check = now;
    const double hz = period > 0 ? 1e9 / static_cast<double>(period) : 0.0;
    double base = pacing.game_fps > 5.0 ? hz / pacing.game_fps : 1.0;
    if (std::fabs(base - std::round(base)) < 0.3) {
        base = std::round(base);
    }
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.goemon.pace", value);
    if (std::strcmp(value, "off") == 0) {
        base = 1.0;
    }
    pacing.base = std::max(1.0, base);
    const uint32_t hold = static_cast<uint32_t>(std::lround(pacing.base));
    if (hold != pacing.hold) {
        vr_log("pacing: each game frame up for %u refreshes (%.1f game fps at %.0f Hz)", hold, pacing.game_fps, hz);
        pacing.hold = hold;
    }
}

// Refreshes a picture stays up for.
uint32_t picture_hold(uint32_t count) {
    return static_cast<uint32_t>(std::max<long>(1, std::lround(pacing.base / static_cast<double>(std::max<uint32_t>(count, 1u)))));
}

// ---- the frame thread -----------------------------------------------------------
//
// Runs at the display rate whatever the game does: waits for the headset's
// next refresh, shows the oldest ready picture once the current one has had
// its share of refreshes, samples the head and the controllers. The game
// never waits on the headset (Wave Race 0.1.1: waiting on the game's thread
// made it slow and the headset reuse old frames).
std::atomic<bool> frame_thread_running{false};

void frame_thread_main() {
    vr_log("frame thread started");
    while (true) {
        bool begun = false;
        {
            std::lock_guard<std::mutex> lock(xr_mu);
            poll_events();
            if (init_state != InitState::Ready) {
                break;
            }
            begun = session_begun;
            if (begun) {
                sync_actions();
            }
        }
        if (!begun) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (!wait_frame()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (!begin_frame()) {
            continue;
        }
        const XrDuration period = frame_state.predictedDisplayPeriod;
        LayerDesc desc;
        XrSwapchain swapchain = XR_NULL_HANDLE;
        const EyeChain* chain = nullptr;
        bool new_picture = false;
        {
            std::lock_guard<std::mutex> lock(slot_mu);
            int next = -1;
            for (int i = 0; i < kSlots; ++i) {
                if (i != front_slot && slots[i].ready && !slots[i].writing && (next < 0 || slots[i].seq < slots[next].seq)) {
                    next = i;
                }
            }
            if (next >= 0) {
                Slot& candidate = slots[next];
                const XrDuration on_screen = frame_state.predictedDisplayTime - pacing.front_since;
                const uint32_t hold = front_slot >= 0 ? picture_hold(slots[front_slot].count) : 1u;
                const bool held_enough = front_slot < 0 || period <= 0 ||
                    on_screen >= period * static_cast<XrDuration>(hold) - period / 2;
                // A menu picture after a level one (or the other way) goes up at once.
                const bool mode_change = front_slot >= 0 && candidate.world != slots[front_slot].world;
                if (held_enough && !mode_change && period > 0) {
                    // The game never waits for the headset, and the two keep
                    // slightly different time: a game a hair faster than the
                    // display slowly piles pictures up, and everything shows
                    // later. When a whole game frame more than usual has been
                    // waiting for 2 s (not just an uneven arrival), skip the
                    // oldest picture. (Holding the game back instead trapped it:
                    // the frame rate measured from its pictures fell with it.)
                    const int waiting = waiting_pictures();
                    if (waiting >= static_cast<int>(candidate.count) + 1) {
                        if (pacing.backlog_since == 0) {
                            pacing.backlog_since = frame_state.predictedDisplayTime;
                        } else if (frame_state.predictedDisplayTime - pacing.backlog_since > 2000000000LL) {
                            candidate.ready = false;
                            ++pacing.skipped;
                            pacing.backlog_since = 0;
                            next = -1;
                            for (int i = 0; i < kSlots; ++i) {
                                if (i != front_slot && slots[i].ready && !slots[i].writing && (next < 0 || slots[i].seq < slots[next].seq)) {
                                    next = i;
                                }
                            }
                        }
                    } else {
                        pacing.backlog_since = 0;
                    }
                }
                if (next >= 0 && (held_enough || mode_change)) {
                    Slot& candidate = slots[next];
                    if (front_slot >= 0 && pacing.front_since != 0 && period > 0) {
                        const int shown = static_cast<int>(std::lround(static_cast<double>(on_screen) / static_cast<double>(period)));
                        ++pacing.shown[std::clamp(shown, 1, 7)];
                    }
                    if (pace_trace() && candidate.world) {
                        int waiting = 0;
                        for (int i = 0; i < kSlots; ++i) {
                            waiting += (slots[i].ready && i != next) ? 1 : 0;
                        }
                        vr_log("pace: %.1f show #%llu after %.2f refreshes, %d more waiting", trace_ms(),
                            static_cast<unsigned long long>(candidate.seq), period > 0 ? static_cast<double>(on_screen) / static_cast<double>(period) : 0.0, waiting);
                        pace_trace_left.fetch_sub(1);
                    }
                    front_slot = next;
                    candidate.ready = false;
                    pacing.front_since = frame_state.predictedDisplayTime;
                    new_picture = true;
                }
            }
            if (front_slot >= 0) {
                desc = slots[front_slot].desc;
                swapchain = slots[front_slot].chain.swapchain;
                chain = &slots[front_slot].chain;
            }
        }
        end_frame(front_slot >= 0 ? &desc : nullptr, swapchain, chain);
        if (new_picture) {
            ever_presented = true;
        }
        locate_views();
        update_pacing(period);
        ++pacing.frames;
        if (new_picture && ++pacing.pictures >= 600) {
            vr_log("pacing: %.1f game fps at %.0f Hz, hold %u; pictures shown 1x %u, 2x %u, 3x %u, 4x %u, 5x %u, 6x %u, 7x+ %u; %u refreshes, %u skipped to catch up",
                pacing.game_fps, period > 0 ? 1e9 / static_cast<double>(period) : 0.0, pacing.hold,
                pacing.shown[1], pacing.shown[2], pacing.shown[3], pacing.shown[4], pacing.shown[5], pacing.shown[6], pacing.shown[7], pacing.frames,
                pacing.skipped);
            std::fill(std::begin(pacing.shown), std::end(pacing.shown), 0u);
            pacing.skipped = 0;
            pacing.pictures = 0;
            pacing.frames = 0;
        }
    }
    frame_thread_running.store(false);
    vr_log("frame thread stopped");
}

void start_frame_thread() {
    if (frame_thread_running.exchange(true)) {
        return;
    }
    std::thread(frame_thread_main).detach();
}

void image_barrier(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(copy_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

VkImageLayout vk_layout(plume::RenderTextureLayout layout) {
    switch (layout) {
    case plume::RenderTextureLayout::GENERAL:
        return VK_IMAGE_LAYOUT_GENERAL;
    case plume::RenderTextureLayout::SHADER_READ:
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case plume::RenderTextureLayout::COLOR_WRITE:
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case plume::RenderTextureLayout::COPY_SOURCE:
    case plume::RenderTextureLayout::RESOLVE_SOURCE:
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case plume::RenderTextureLayout::COPY_DEST:
    case plume::RenderTextureLayout::RESOLVE_DEST:
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case plume::RenderTextureLayout::PRESENT:
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    default:
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
}

// Copies the picture into the eye images. A picture that fits goes in 1:1 at
// the top-left (menus render smaller than the level image); a bigger one is
// scaled to fill. out_w/out_h report the region of the eye image it occupies.
bool copy_images(VkImage source, VkFormat source_format, VkImageLayout source_layout, uint32_t source_width, uint32_t source_height, VkImage const* dests, int dest_count, uint32_t image_width, uint32_t image_height, uint32_t& out_w, uint32_t& out_h) {
    if (source == VK_NULL_HANDLE || copy_cmd == VK_NULL_HANDLE || dest_count <= 0 || vk_queue->queue == nullptr) {
        return false;
    }
    const VkFormat dest_format = static_cast<VkFormat>(swap_format);
    if (source_width == 0 || source_height == 0 || image_width == 0 || image_height == 0) {
        return false;
    }
    const bool fits = source_width <= image_width && source_height <= image_height;
    const uint32_t dest_width = fits ? source_width : image_width;
    const uint32_t dest_height = fits ? source_height : image_height;
    out_w = dest_width;
    out_h = dest_height;
    const bool scale = source_width != dest_width || source_height != dest_height;
    bool blit = source_format != dest_format || scale;
    const bool blit_supported = vk_device != nullptr
        && format_supports(dest_format, VK_FORMAT_FEATURE_BLIT_DST_BIT)
        && format_supports(source_format, VK_FORMAT_FEATURE_BLIT_SRC_BIT);
    if (blit && !blit_supported) {
        if (source_format != dest_format) {
            vr_log("eye blit is not supported for format %d", static_cast<int>(dest_format));
            return false;
        }
        blit = false;
        out_w = std::min(source_width, image_width);
        out_h = std::min(source_height, image_height);
    }
    if (vkResetCommandBuffer(copy_cmd, 0) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(copy_cmd, &begin) != VK_SUCCESS) {
        return false;
    }
    if (source_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        source_layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    image_barrier(source, source_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    bool recorded = false;
    for (int i = 0; i < dest_count; ++i) {
        VkImage dest = dests[i];
        if (dest == VK_NULL_HANDLE) {
            continue;
        }
        const uint32_t copy_width = out_w;
        const uint32_t copy_height = out_h;
        image_barrier(dest, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        if (!blit) {
            VkImageCopy region{};
            region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.srcSubresource.layerCount = 1;
            region.dstSubresource = region.srcSubresource;
            region.extent = {copy_width, copy_height, 1};
            vkCmdCopyImage(copy_cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        } else {
            VkImageBlit region{};
            region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.srcSubresource.layerCount = 1;
            region.srcOffsets[1] = {static_cast<int32_t>(source_width), static_cast<int32_t>(source_height), 1};
            region.dstSubresource = region.srcSubresource;
            region.dstOffsets[1] = {static_cast<int32_t>(dest_width), static_cast<int32_t>(dest_height), 1};
            vkCmdBlitImage(copy_cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
        }
        image_barrier(dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        recorded = true;
    }
    image_barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source_layout, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
    if (vkEndCommandBuffer(copy_cmd) != VK_SUCCESS) {
        return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &copy_cmd;
    vkResetFences(vk_device->vk, 1, &copy_fence);
    VkResult submitted = VK_ERROR_UNKNOWN;
    if (vk_queue->queue->mutex != nullptr) {
        std::lock_guard<std::mutex> queue_lock(*vk_queue->queue->mutex);
        submitted = vkQueueSubmit(vk_queue->queue->vk, 1, &submit, copy_fence);
    } else {
        submitted = vkQueueSubmit(vk_queue->queue->vk, 1, &submit, copy_fence);
    }
    if (submitted != VK_SUCCESS) {
        static int submit_logs = 0;
        if (submit_logs < 5) {
            ++submit_logs;
            vr_log("eye copy submit failed (VkResult %d)", static_cast<int>(submitted));
        }
        return false;
    }
    VkResult waited = VK_SUCCESS;
    {
        static int slow_budget = 20;
        SlowCallLog slow{"eye copy on the GPU", 100.0, &slow_budget};
        waited = vkWaitForFences(vk_device->vk, 1, &copy_fence, VK_TRUE, 2000000000ull);
    }
    if (waited != VK_SUCCESS) {
        static int wait_logs = 0;
        if (wait_logs < 5) {
            ++wait_logs;
            vr_log("eye copy did not finish (VkResult %d%s)", static_cast<int>(waited),
                waited == VK_ERROR_DEVICE_LOST ? ", GPU device lost" : "");
        }
        return false;
    }
    return recorded;
}

// ---- probe (test runs only) ----------------------------------------------------
//
// `debug.goemon.probe N` (during a test run, see test_switches_live): for the
// next N level pictures, read back an 8-row strip across the middle of each and
// log how far its content moved sideways from the previous picture. With the
// camera turning steadily (test button "turnright"), a real in-between picture
// sits halfway: the shifts come out even (e.g. 15, 15, 15). One that repeats a
// game frame's camera shows as 30, 0, 30, 0.
struct Probe {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::vector<float> last;
    bool failed = false;
};
Probe probe;
constexpr uint32_t kProbeRows = 8;
constexpr VkDeviceSize kProbeBytes = 2048ull * kProbeRows * 4ull;

bool probe_ready() {
    if (probe.failed) {
        return false;
    }
    if (probe.mapped != nullptr) {
        return true;
    }
    probe.failed = true;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = kProbeBytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vk_device->vk, &info, nullptr, &probe.buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(vk_device->vk, probe.buffer, &req);
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(vk_device->physicalDevice, &props);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) != 0 && (props.memoryTypes[i].propertyFlags & want) == want) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(vk_device->vk, &alloc, nullptr, &probe.memory) != VK_SUCCESS ||
        vkBindBufferMemory(vk_device->vk, probe.buffer, probe.memory, 0) != VK_SUCCESS ||
        vkMapMemory(vk_device->vk, probe.memory, 0, kProbeBytes, 0, &probe.mapped) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferAllocateInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_info.commandPool = copy_pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkAllocateCommandBuffers(vk_device->vk, &cmd_info, &probe.cmd) != VK_SUCCESS ||
        vkCreateFence(vk_device->vk, &fence_info, nullptr, &probe.fence) != VK_SUCCESS) {
        return false;
    }
    probe.failed = false;
    return true;
}

// Reads the strip of an RGBA8 picture into per-column brightness.
bool probe_strip(VkImage source, VkImageLayout layout, uint32_t width, uint32_t height, std::vector<float>& out) {
    if (!probe_ready() || width < 64 || height < kProbeRows * 2) {
        return false;
    }
    const uint32_t strip_w = std::min<uint32_t>(width / 2, 2048u);
    const int32_t x0 = static_cast<int32_t>((width - strip_w) / 2);
    const int32_t y0 = static_cast<int32_t>(height / 2 - kProbeRows / 2);
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    auto barrier = [](VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(probe.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkResetCommandBuffer(probe.cmd, 0) != VK_SUCCESS || vkBeginCommandBuffer(probe.cmd, &begin) != VK_SUCCESS) {
        return false;
    }
    barrier(source, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {x0, y0, 0};
    region.imageExtent = {strip_w, kProbeRows, 1};
    vkCmdCopyImageToBuffer(probe.cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, probe.buffer, 1, &region);
    barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
    if (vkEndCommandBuffer(probe.cmd) != VK_SUCCESS) {
        return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &probe.cmd;
    vkResetFences(vk_device->vk, 1, &probe.fence);
    VkResult submitted = VK_ERROR_UNKNOWN;
    {
        std::unique_lock<std::mutex> queue_lock;
        if (vk_queue->queue->mutex != nullptr) {
            queue_lock = std::unique_lock<std::mutex>(*vk_queue->queue->mutex);
        }
        submitted = vkQueueSubmit(vk_queue->queue->vk, 1, &submit, probe.fence);
    }
    if (submitted != VK_SUCCESS || vkWaitForFences(vk_device->vk, 1, &probe.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) {
        return false;
    }
    const uint8_t* px = static_cast<const uint8_t*>(probe.mapped);
    out.assign(strip_w, 0.0f);
    for (uint32_t row = 0; row < kProbeRows; ++row) {
        for (uint32_t x = 0; x < strip_w; ++x) {
            const uint8_t* p = px + (static_cast<size_t>(row) * strip_w + x) * 4;
            out[x] += (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / static_cast<float>(kProbeRows);
        }
    }
    return true;
}

// Sideways shift (pixels) that best lines up `cur` with `prev`, and the mean
// difference there and with no shift.
int probe_shift(const std::vector<float>& prev, const std::vector<float>& cur, float& best_err, float& zero_err, float* subpixel = nullptr) {
    const int n = static_cast<int>(std::min(prev.size(), cur.size()));
    const int range = std::min(160, n / 4);
    int best = 0;
    best_err = 1e30f;
    zero_err = 0.0f;
    std::vector<float> errs(static_cast<size_t>(2 * range + 1), 1e30f);
    for (int d = -range; d <= range; ++d) {
        double sum = 0.0;
        int count = 0;
        for (int x = std::max(0, -d); x < n && x + d < n; ++x) {
            const float e = cur[x + d] - prev[x];
            sum += e * e;
            ++count;
        }
        const float err = count > 0 ? static_cast<float>(sum / count) : 1e30f;
        errs[static_cast<size_t>(d + range)] = err;
        if (d == 0) {
            zero_err = err;
        }
        if (err < best_err) {
            best_err = err;
            best = d;
        }
    }
    if (subpixel != nullptr) {
        // Parabola through the best shift and its neighbours.
        *subpixel = static_cast<float>(best);
        if (best > -range && best < range) {
            const float a = errs[static_cast<size_t>(best - 1 + range)];
            const float b = errs[static_cast<size_t>(best + range)];
            const float c = errs[static_cast<size_t>(best + 1 + range)];
            const float den = a - 2.0f * b + c;
            if (den > 1e-6f) {
                *subpixel = static_cast<float>(best) + 0.5f * (a - c) / den;
            }
        }
    }
    return best;
}

// yaw: the head yaw the picture was placed with (radians, +left); px_per_tan: the
// picture's pixels per unit of tan(angle) across. A world that stays put moves by
// px_per_tan * tan(yaw change) between two pictures (to the right for a left turn).
void probe_picture(plume::VulkanTexture* source, uint32_t index, uint32_t count, float weight, float yaw, float px_per_tan) {
    static float last_yaw = 0.0f;
    // A new value of the switch arms it once; it then logs that many pictures.
    // (Each readback waits on the GPU on RT64's present thread, which costs some
    // in-between pictures while it runs.)
    static uint32_t pictures = 0;
    static int budget = 0;
    static int armed_value = 0;
    if ((pictures++ % 60u) == 0u) {
        const int value = test_switches_live() ? debug_prop_int("debug.goemon.probe", 0) : 0;
        if (value != armed_value) {
            armed_value = value;
            budget = value;
            probe.last.clear();
            if (budget > 0) {
                vr_log("probe: logging the next %d level pictures", budget);
            }
        }
    }
    if (budget <= 0 || source == nullptr) {
        return;
    }
    std::vector<float> strip;
    if (!probe_strip(source->vk, vk_layout(source->textureLayout), source->desc.width, source->desc.height, strip)) {
        vr_log("probe: readback failed");
        budget = 0;
        return;
    }
    if (!probe.last.empty()) {
        float best_err = 0.0f;
        float zero_err = 0.0f;
        float sub = 0.0f;
        const int shift = probe_shift(probe.last, strip, best_err, zero_err, &sub);
        const float expected = px_per_tan * std::tan(goemon_vr::wrap_angle(yaw - last_yaw));
        vr_log("probe: picture %u of %u (weight %.2f) moved %+d px (%+.2f) from the last one, head turn says %+.2f px (yaw %+.2f deg; diff %.1f, unmoved %.1f)",
            index + 1, count, weight, shift, sub, expected, goemon_vr::wrap_angle(yaw - last_yaw) * (180.0f / 3.1415926535f), best_err, zero_err);
    }
    last_yaw = yaw;
    probe.last.swap(strip);
    --budget;
}

Quat slerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) {
        b = Quat{-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    float wa = 1.0f - t;
    float wb = t;
    if (d < 0.9995f) {
        const float angle = std::acos(std::clamp(d, -1.0f, 1.0f));
        const float s = std::sin(angle);
        wa = std::sin((1.0f - t) * angle) / s;
        wb = std::sin(t * angle) / s;
    }
    Quat q{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len > 1.0e-6f) {
        q = Quat{q.x / len, q.y / len, q.z / len, q.w / len};
    }
    return q;
}

Quat from_xr(XrQuaternionf q) {
    return Quat{q.x, q.y, q.z, q.w};
}

// Called on RT64's present thread for every picture it presents (index
// 0..count-1, weight from the previous game frame to this one). With smooth
// motion a level frame brings two: first the in-between picture (weight 0.5),
// placed at the head pose halfway between the two game frames' poses, then the
// game frame itself. Each goes into a free slot with the layer it was drawn for.
// Menus and the sharp picture use only the first picture of a present, as
// before (menus keep RT64's own refresh setting).
void on_presented_frame(plume::RenderTexture* texture, uint32_t index, uint32_t count, float weight) {
    std::lock_guard<std::mutex> capture_lock(capture.mu);
    const uint64_t present_id = RT64::GetRenderHookPresentId();
    if (index == 0) {
        if (capture.token == 0) {
            return;
        }
        // A lagging present queue (GPU busy, e.g. while the Quest records) can
        // still be working through an earlier frame. Copying that would pair an
        // old picture with the new frame's head pose, which flickers in both
        // eyes. Leave the capture armed for the frame it belongs to.
        if (present_id < capture.min_present_id) {
            capture.stale_skipped++;
            return;
        }
        capture.token = 0;
        capture.active_present = present_id;
        capture.active_cur = capture.pending;
        capture.active_world = capture.pending_world;
        capture.active_has_prev = capture.have_last && capture.last_world && capture.pending_world && !capture.pending_cut;
        capture.active_prev = capture.last;
        capture.last = capture.pending;
        capture.last_world = capture.pending_world;
        capture.have_last = true;
    } else if (present_id != capture.active_present || capture.active_present == 0) {
        return;
    }
    // Every picture of the present only for a level in smooth motion.
    const bool all_pictures = capture.active_world && interp_fps() > 0;
    if (index > 0 && !all_pictures) {
        return;
    }
    const uint32_t pictures = all_pictures ? std::max<uint32_t>(count, 1u) : 1u;
    if (texture == nullptr) {
        return;
    }
    LayerDesc desc = capture.active_cur;
    if (pictures > 1 && weight < 0.999f && capture.active_has_prev && !desc.quad) {
        desc.pose.orientation = to_xr(slerp(from_xr(capture.active_prev.pose.orientation), from_xr(desc.pose.orientation), std::clamp(weight, 0.0f, 1.0f)));
    }
    const int slot = claim_slot();
    if (slot < 0) {
        if (index == 0) {
            capture.copied = false;
        }
        return;
    }
    EyeChain& chain = slots[slot].chain;
    bool copied = false;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    uint32_t rect_w = 0;
    uint32_t rect_h = 0;
    auto* source = dynamic_cast<plume::VulkanTexture*>(texture);
    if ((debug_flags.load() & kDebugNoCopy) != 0) {
        // Test switch: no copy (the old picture stays up).
        source = nullptr;
    }
    if (source != nullptr && source->vk != VK_NULL_HANDLE && acquire_slot_image(chain)) {
        const VkImage dest = chain.images[static_cast<size_t>(chain.acquired)].image;
        const VkFormat source_format = source->imageFormat != VK_FORMAT_UNDEFINED ? source->imageFormat : VK_FORMAT_B8G8R8A8_UNORM;
        source_width = source->desc.width;
        source_height = source->desc.height;
        copied = copy_images(source->vk, source_format, vk_layout(source->textureLayout), source_width, source_height, &dest, 1,
            chain.width, chain.height, rect_w, rect_h);
        if (copied && capture.active_world) {
            const XrQuaternionf& o = desc.pose.orientation;
            const float t_right = std::tan(desc.fov.angleRight);
            probe_picture(source, index, count, weight, vr_yaw_from_quat(o.x, o.y, o.z, o.w),
                t_right > 0.01f ? 0.5f * static_cast<float>(source->desc.width) / t_right : 0.0f);
        }
        if (source->textureLayout == plume::RenderTextureLayout::UNKNOWN) {
            source->textureLayout = plume::RenderTextureLayout::PRESENT;
        }
        release_swapchain(chain.swapchain);
        chain.acquired = -1;
    }
    if (copied) {
        const float aspect = source_height != 0 ? static_cast<float>(source_width) / static_cast<float>(source_height) : 0.0f;
        fit_layer_to_image(desc, aspect, rect_w, rect_h);
    }
    {
        std::lock_guard<std::mutex> lock(slot_mu);
        slots[slot].writing = false;
        if (copied) {
            slots[slot].desc = desc;
            slots[slot].world = capture.active_world;
            slots[slot].seq = ++slot_seq;
            slots[slot].count = pictures;
            slots[slot].ready = true;
        }
    }
    if (copied) {
        if (index == 0) {
            note_arrival();   // game frames, not pictures
        }
        if (pace_trace() && capture.active_world) {
            vr_log("pace: %.1f arrive #%llu (%u of %u, weight %.2f)", trace_ms(), static_cast<unsigned long long>(slot_seq), index + 1, count, weight);
        }
    }
    if (index == 0) {
        capture.copied = copied;
    }
    static int logs = 0;
    if (logs < 3 || (pictures > 1 && logs < 6)) {
        ++logs;
        vr_log("%s picture %u of %u (weight %.2f) into slot %d (%ux%u)", copied ? "copied" : "could not copy", index + 1, count, weight,
            slot, source_width, source_height);
    }
}

void draw_task(RT64::Application* app, const OSTask* task) {
    app->state->rsp->reset();
    app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
    app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
}

// Draws the task; RT64's present thread hands its pictures to
// on_presented_frame with `drawn` as their layer.
bool draw_and_capture(RT64::Application* app, const OSTask* task, const LayerDesc& drawn, bool world, bool cut) {
    // Only presents this draw creates (ids after `before`) may be copied.
    const uint64_t before = app->state->presentId;
    uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(capture.mu);
        token = ++capture.serial;
        capture.token = token;
        capture.pending = drawn;
        capture.pending_world = world;
        capture.pending_cut = cut;
        capture.copied = false;
        capture.min_present_id = before + 1;
    }
    draw_task(app, task);
    const uint64_t after = app->state->presentId;
    if (after != before) {
        // Pipeline compile plus a full-panel render can take longer than one
        // refresh. The hook copies before this id is signaled, so wait it out.
        if (!app->presentQueue->waitForPresentIdTimed(after, 500)) {
            static int logs = 0;
            if (logs < 10) {
                ++logs;
                vr_log("timed out waiting for the eye present");
            }
        }
    }
    bool ok = false;
    {
        // Never leave a capture armed past its frame: a later present must not
        // fill it with a different frame's picture.
        std::lock_guard<std::mutex> lock(capture.mu);
        ok = capture.token != token && capture.copied;
        capture.token = 0;
    }

    // A line every ~20 s so a log shows whether frames are being dropped.
    static uint32_t copies = 0;
    static uint32_t misses = 0;
    if (ok) {
        ++copies;
    } else {
        ++misses;
    }
    if (copies + misses >= 600) {
        uint32_t stale = 0;
        {
            std::lock_guard<std::mutex> lock(capture.mu);
            stale = capture.stale_skipped;
            capture.stale_skipped = 0;
        }
        vr_log("frames: %u shown, %u missed, %u late presents skipped", copies, misses, stale);
        copies = 0;
        misses = 0;
    }
    return ok;
}

bool real_camera(const float lookat[16]) {
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(lookat[i])) {
            return false;
        }
    }
    float eye[3], right[3], up[3], lookv[3];
    vr_basis_from_lookat(lookat, eye, right, up, lookv);
    float right_len = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    float eye_len = std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
    return right_len > 0.5f && right_len < 1.5f && eye_len > 1.0f;
}

// Level cameras aimed since the last display list (more than one per frame means
// several camera passes share the world lock; logged in on_display_list).
std::atomic<uint32_t> aims_this_list{0};

bool on_display_list(RT64::Application* app, const OSTask* task) {
    {
        const uint32_t aims = aims_this_list.exchange(0);
        static int multi_logs = 0;
        if (aims > 1u && multi_logs < 10) {
            ++multi_logs;
            vr_log("world lock: %u level cameras aimed in one frame", aims);
        }
    }
    run_math_self_test();
    refresh_debug_flags();
    if (!ensure_session(app)) {
        return false;
    }
    // The frame thread polls OpenXR and reads the controllers; until the
    // session runs, RT64 draws as usual.
    if (init_state != InitState::Ready || !session_begun) {
        return false;
    }
    take_head_state();

    // Gameplay frames that aim_view aimed with the headset fill your view.
    // Everything else (title, menus, pause, transitions) goes on the floating
    // screen at the game's own field of view, untouched.
    FrameRecord rec{};
    bool world = headset_fov_ready && app != nullptr && take_frame_record(app->core.RDRAM, task, rec);
    // A level frame now and then without its camera found (a lost record) stays
    // a level frame with the last pose, instead of flicking to the menu screen.
    static FrameRecord last_world_rec{};
    static int frames_since_world = 1000;
    if (world) {
        last_world_rec = rec;
        frames_since_world = 0;
    } else if (++frames_since_world <= 3 && headset_fov_ready) {
        rec = last_world_rec;
        rec.cut = false;
        world = true;
    }
    const bool entering_screen = !world && !last_frame_screen;
    last_frame_screen = !world;
    set_rt64_config(app, world);
    set_hud_inset(world && (debug_flags.load() & kDebugNoHudInset) == 0);
    const LayerDesc pending = world ? world_layer(rec) : screen_layer(entering_screen);

    if ((debug_flags.load() & kDebugNoDraw) != 0 && ever_presented) {
        // Test switch: no RT64 GPU work at all; the last picture stays up.
        return true;
    }
    // One render. Its pictures (two per level frame with smooth motion) go into
    // the slot queue from RT64's present thread (on_presented_frame), and the
    // frame thread shows them.
    draw_and_capture(app, task, pending, world, world && rec.cut);
    return true;
}

struct HookInstall {
    HookInstall() {
        RT64::SetRenderHookPresentedFrame(&on_presented_frame);
    }
};

HookInstall hook_install;

void read_vec3(uint8_t* rdram, uint32_t vram, float out[3]) {
    out[0] = read_f32(rdram, vram);
    out[1] = read_f32(rdram, vram + 4u);
    out[2] = read_f32(rdram, vram + 8u);
}

void write_vec3(uint8_t* rdram, uint32_t vram, const float v[3]) {
    write_f32(rdram, vram, v[0]);
    write_f32(rdram, vram + 4u, v[1]);
    write_f32(rdram, vram + 8u, v[2]);
}

// RDRAM is stored as native 32-bit words, so a halfword lives at address ^ 2.
uint16_t read_u16(uint8_t* rdram, uint32_t vram) {
    uint16_t value = 0;
    std::memcpy(&value, vram_bytes(rdram, vram ^ 2u), sizeof(value));
    return value;
}

void write_u16(uint8_t* rdram, uint32_t vram, uint16_t value) {
    std::memcpy(vram_bytes(rdram, vram ^ 2u), &value, sizeof(value));
}

// The previous camera the game drew was aimed with the headset, for spotting
// camera jumps (game thread).
bool aim_prev_applied = false;

// Smooth turning (the owner's choice over snap turning): the right stick turns
// a "body" yaw that is added to the head's yaw when the camera is aimed. The
// layer keeps only the head's yaw, so the level turns around you.
std::chrono::steady_clock::time_point body_yaw_last{};
// Head + body yaw of the last aimed camera, for turning the left stick so
// forward walks where you look (overlay_input).
std::atomic<uint32_t> move_yaw_bits{0};
std::atomic<int64_t> last_aim_ms{0};
// The world lock's memory (goemon_vr_camera.h), game thread only.
goemon_vr::LockState lock_state;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Goemon's camera films from far away with a narrow lens (about 25 degrees in
// Goemon's house). Through the headset's ~120 degree view everything would look
// tiny, so the eye moves toward the look-at point to this share of the
// distance. The right stick's up/down changes it (saved in camera_distance.txt).
constexpr float kDefaultCameraPull = 0.5f;
constexpr float kMinCameraPull = 0.15f;
constexpr float kMaxCameraPull = 1.0f;
float camera_pull = -1.0f;
bool camera_pull_dirty = false;
std::chrono::steady_clock::time_point camera_pull_changed{};

void load_camera_pull() {
    camera_pull = kDefaultCameraPull;
    if (FILE* f = std::fopen("camera_distance.txt", "rb")) {
        float value = 0.0f;
        if (std::fscanf(f, "%f", &value) == 1 && value >= kMinCameraPull && value <= kMaxCameraPull) {
            camera_pull = value;
        }
        std::fclose(f);
    }
    vr_log("camera distance %.2f of the game's (right stick up/down changes it)", camera_pull);
}

void save_camera_pull() {
    if (FILE* f = std::fopen("camera_distance.txt", "wb")) {
        std::fprintf(f, "%.3f\n", camera_pull);
        std::fclose(f);
    }
    vr_log("camera distance saved: %.2f", camera_pull);
}

bool aim_camera(uint8_t* rdram, uint32_t cam) {
    if (vr_controls.load() == 0u || aim_ready.load() == 0u || init_state != InitState::Ready || !session_begun) {
        return false;
    }
    if (!goemon_vr::is_level_camera(rdram, cam)) {
        return false;
    }
    HeadState head;
    {
        std::lock_guard<std::mutex> lock(head_mu);
        head = head_live;
    }
    if (!head.fov_ready) {
        return false;
    }
    Quat current{};
    if (!load_stable_quat(aim_qx, aim_qy, aim_qz, aim_qw, aim_seq, current)) {
        return false;
    }
    const Quat ref{bits_float(aim_ref_qx.load()), bits_float(aim_ref_qy.load()), bits_float(aim_ref_qz.load()), bits_float(aim_ref_qw.load())};
    Quat delta = quat_mul(current, quat_inv(ref));
    const float dlen = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z + delta.w * delta.w);
    if (dlen < 0.5f || dlen > 1.5f) {
        return false;
    }
    float yaw = vr_yaw_from_quat(delta.x / dlen, delta.y / dlen, delta.z / dlen, delta.w / dlen);
    // Test runs: `debug.goemon.fakeyaw <degrees>` swings the head yaw left and
    // right (2 s period) as if the head turned, for the probe (needs testuntil).
    {
        static uint32_t fake_reads = 0;
        static int fake_amp = 0;
        if ((fake_reads++ % 60u) == 0u) {
            fake_amp = test_switches_live() ? debug_prop_int("debug.goemon.fakeyaw", 0) : 0;
        }
        if (fake_amp != 0) {
            const double secs = static_cast<double>(now_ms()) / 1000.0;
            yaw += static_cast<float>(fake_amp) * (3.1415926535f / 180.0f) * static_cast<float>(std::sin(secs * 3.14159265358979));
        }
    }

    // Smooth turning.
    const auto now = std::chrono::steady_clock::now();
    float dt = body_yaw_last.time_since_epoch().count() == 0 ? 0.0f : std::chrono::duration<float>(now - body_yaw_last).count();
    body_yaw_last = now;
    dt = std::clamp(dt, 0.0f, 0.1f);
    float stick_x = bits_float(test_stick_rx_bits.load()) != 0.0f ? bits_float(test_stick_rx_bits.load()) : bits_float(stick_rx_bits.load());
    if (std::fabs(stick_x) < kStickYawDeadzone) {
        stick_x = 0.0f;
    }
    // Stick right turns right (yaw is +left).
    const float turn = -stick_x * kTurnDegPerSecond * (3.1415926535f / 180.0f) * dt;
    // World lock (goemon_vr_camera.h): the game's own swinging around Goemon
    // doesn't turn the world. `adb shell setprop debug.goemon.yawlock 0` shows
    // the game's turning instead (live).
    static uint32_t lock_reads = 0;
    static bool world_lock = true;
    if ((lock_reads++ % 60u) == 0u) {
        const bool want = debug_prop_int("debug.goemon.yawlock", 1) != 0;
        if (want != world_lock) {
            vr_log("world lock %s", want ? "on" : "off");
        }
        world_lock = want;
    }

    // Camera distance: right stick up = closer, down = farther (saved).
    if (camera_pull < 0.0f) {
        load_camera_pull();
    }
    const float stick_y = bits_float(stick_ry_bits.load());
    if (std::fabs(stick_y) > 0.5f && !first_person_on.load()) {
        camera_pull = std::clamp(camera_pull - stick_y * 0.6f * dt, kMinCameraPull, kMaxCameraPull);
        camera_pull_dirty = true;
        camera_pull_changed = now;
    } else if (camera_pull_dirty && now - camera_pull_changed > std::chrono::seconds(1)) {
        camera_pull_dirty = false;
        save_camera_pull();
    }

    goemon_vr::AimInput in;
    in.head_yaw = yaw;
    in.turn = turn;
    in.lock = world_lock;
    in.realign = body_reset_requested.exchange(false);
    in.first_person = first_person_on.load();
    // First person tuning, live: debug.goemon.fpheight / fpforward / fpnear (game units).
    static uint32_t fp_reads = 0;
    static int fp_height = 0, fp_forward = -1, fp_near = 0;
    if ((fp_reads++ % 120u) == 0u) {
        fp_height = debug_prop_int("debug.goemon.fpheight", 0);
        fp_forward = debug_prop_int("debug.goemon.fpforward", -1);
        fp_near = debug_prop_int("debug.goemon.fpnear", 0);
    }
    in.fp_eye_height = static_cast<float>(fp_height);
    in.fp_forward = static_cast<float>(fp_forward);
    in.fp_near = static_cast<float>(fp_near);
    static uint32_t units_reads = 0;
    static int units = kGoemonUnitsPerMeter;
    if ((units_reads++ % 120u) == 0u) {
        const int next = std::clamp(debug_prop_int("debug.goemon.units", kGoemonUnitsPerMeter), 0, 200);
        if (next != units || units_reads == 1u) {
            units = next;
            vr_log("head movement moves the eye: %d game units per meter%s", units, units == 0 ? " (off)" : "");
        }
    }
    // Outdoor camera: `adb shell setprop debug.goemon.outdoor 1|2|0` (live): 1 the
    // eye chases Goemon (default), 2 a standing camera like the rooms', 0 the rooms'
    // logic everywhere (the game's camera spot).
    static uint32_t outdoor_reads = 0;
    static int outdoor = 1;
    if ((outdoor_reads++ % 120u) == 0u) {
        const int next = std::clamp(debug_prop_int("debug.goemon.outdoor", 1), 0, 2);
        if (next != outdoor || outdoor_reads == 1u) {
            static const char* const names[] = {"the game's camera spot (room logic)", "chasing Goemon", "a standing camera like the rooms"};
            vr_log("outdoor camera: %s", names[next]);
        }
        outdoor = next;
    }
    in.outdoor_mode = outdoor;
    const bool head_pos_used = units > 0 && head.anchor_set && head.valid;
    if (head_pos_used) {
        // The head's movement in the frame of the way it faced at the start (the
        // game's base heading stands for that direction, not for the room's -z).
        const float ref_yaw = vr_yaw_from_quat(ref.x, ref.y, ref.z, ref.w);
        const float c = std::cos(ref_yaw);
        const float s = std::sin(ref_yaw);
        const float dx = head.mid.x - head.anchor.x;
        const float dz = head.mid.z - head.anchor.z;
        in.units_per_meter = static_cast<float>(units);
        in.head_offset[0] = dx * c - dz * s;
        in.head_offset[1] = head.mid.y - head.anchor.y;
        in.head_offset[2] = dx * s + dz * c;
    }
    const float max_up = kMaxLookUpDeg * (3.1415926535f / 180.0f);
    const float max_down = kMaxLookDownDeg * (3.1415926535f / 180.0f);
    in.pitch = std::clamp(bits_float(head_pitch_abs_bits.load()), -max_down, max_up);
    // The headset's (wider) vertical view, so the game builds its projection,
    // culling and billboards for it. The exact horizontal scale is written into
    // the frame's projection when the task is drawn (widen_game_projection).
    const float half_v = std::atan(1.0f / std::max(head.proj_y_scale, 0.05f));
    in.fovy_deg = 2.0f * half_v * (180.0f / 3.1415926535f);
    in.pull = camera_pull;
    goemon_vr::AimResult aimed;
    // Fog: from the distance to the eye (`adb shell setprop debug.goemon.fog
    // radial|flat|game`, live; default radial). "game" also drops the near-plane
    // correction (fog exactly as RT64 computes it from our camera).
    static uint32_t fog_reads = 0;
    static int fog_mode = 2;
    static goemon_vr::FogNearTable fog_table;
    bool fog_changed = false;
    if ((fog_reads++ % 120u) == 0u) {
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("debug.goemon.fog", value);
        const int next = std::strcmp(value, "game") == 0 ? 0 : std::strcmp(value, "flat") == 0 ? 1 : 2;
        if (next != fog_mode || fog_reads == 1u) {
            static const char* const names[] = {"as RT64 computes it", "the game's fog (near plane corrected)", "the game's fog from the distance to the eye"};
            vr_log("fog: %s", names[next]);
            fog_changed = true;
        }
        fog_mode = next;
    }
    if (!goemon_vr::aim(rdram, cam, in, lock_state, aimed)) {
        return false;
    }
    if (fog_table.note(aimed.far_plane, aimed.game_near)) {
        fog_changed = true;
        vr_log("fog: camera far %.0f uses the game's near %.1f (we draw from %.1f)", aimed.far_plane, aimed.game_near, goemon_vr::kVrNearPlane);
    }
    if (fog_changed) {
        RT64::SetVrFog(fog_mode == 2, fog_table.fars, fog_table.nears, fog_mode == 0 ? 0 : fog_table.count);
    }
    aims_this_list.fetch_add(1);

    // What the world lock did (cuts, follow mode), the first 60 events.
    {
        static uint32_t seen_cuts = 0;
        static uint32_t seen_follows = 0;
        static uint32_t seen_snaps = 0;
        static int events = 0;
        static const char* const reasons[] = {"?", "game camera turned >20 deg in a frame", "game eye jumped", "Goemon moved far",
            "no level camera for a while", "first level camera"};
        if (lock_state.cuts != seen_cuts && events < 60) {
            ++events;
            const int r = std::clamp(lock_state.cut_reason, 0, 5);
            vr_log("world lock: camera cut %u (%s; turn %.0f deg, moved %.0f, distance %.0f)", lock_state.cuts, reasons[r],
                lock_state.cut_delta * (180.0f / 3.1415926535f), lock_state.cut_moved, lock_state.cut_dist);
        }
        if (lock_state.follows != seen_follows && events < 60) {
            ++events;
            vr_log("world lock: following Goemon (%u), %u follow ends so far", lock_state.follows, lock_state.follow_ends);
        }
        static uint32_t seen_moves = 0;
        if (lock_state.station_moves != seen_moves && events < 60) {
            ++events;
            static const char* const why[] = {"", "Goemon too far", "Goemon too close", "Goemon off to the side", "ground height changed", "first spot"};
            vr_log("world lock: standing camera moved to a new spot (%u, %s)", lock_state.station_moves,
                why[std::clamp(lock_state.station_reason, 0, 5)]);
        }
        seen_moves = lock_state.station_moves;
        if (lock_state.snaps != seen_snaps && events < 60) {
            ++events;
            vr_log("world lock: snapped to the game's view (%u)", lock_state.snaps);
        }
        seen_cuts = lock_state.cuts;
        seen_follows = lock_state.follows;
        seen_snaps = lock_state.snaps;
    }

    FrameRecord rec;
    rec.has_head_pos = head_pos_used;
    rec.head_pos = head.mid;
    rec.dl_phys = aimed.dl_phys;
    rec.proj_phys = aimed.proj_phys;
    rec.yaw = yaw;
    rec.pitch = aimed.pitch;
    rec.fresh = true;
    rec.cut = !aim_prev_applied || aimed.cut;
    push_frame_record(rec);
    move_yaw_bits.store(float_bits(aimed.move_yaw));
    last_aim_ms.store(now_ms());

    static int logs = 0;
    if (logs < 4) {
        ++logs;
        vr_log("camera aimed: dl %08X proj %08X fovy %.0f deg (game %.0f), head yaw %.0f pitch %.0f, distance %.2f",
            rec.dl_phys, rec.proj_phys, in.fovy_deg, aimed.game_fovy_deg, yaw * (180.0f / 3.1415926535f),
            rec.pitch * (180.0f / 3.1415926535f), camera_pull);
    }
    return true;
}

void overlay_input(int player_index, uint16_t* buttons, float* x, float* y) {
    if (player_index != 0 || vr_controls.load() == 0 || buttons == nullptr) {
        return;
    }
    // N64 buttons: A 8000, B 4000, Z 2000, Start 1000, D-pad 0800/0400/0200/0100,
    // L 0020, R 0010, C 0008 (up) 0004 (down) 0002 (left) 0001 (right).
    uint16_t pressed = 0;
    pressed |= button_a.load() != 0 ? 0x8000u : 0u;       // right A: jump
    pressed |= button_b.load() != 0 ? 0x4000u : 0u;       // right B: attack
    pressed |= button_z.load() != 0 ? 0x2000u : 0u;       // right trigger: Z (sub-weapon)
    pressed |= button_r.load() != 0 ? 0x0010u : 0u;       // left trigger: R
    pressed |= button_start.load() != 0 ? 0x1000u : 0u;   // left menu button: Start
    pressed |= button_cup.load() != 0 ? 0x0008u : 0u;     // Y: C-up (look around)
    pressed |= button_cdown.load() != 0 ? 0x0004u : 0u;   // right grip: C-down
    pressed |= button_cleft.load() != 0 ? 0x0002u : 0u;   // X: C-left
    pressed |= button_cright.load() != 0 ? 0x0001u : 0u;  // left grip: C-right (minimap)
    // Outside levels (menus, map) the right stick up/down gives C-up / C-down;
    // in levels it sets the camera distance (aim_camera).
    if (now_ms() - last_aim_ms.load() >= 250) {
        const float ry = bits_float(stick_ry_bits.load());
        pressed |= ry > 0.7f ? 0x0008u : 0u;
        pressed |= ry < -0.7f ? 0x0004u : 0u;
    }
    *buttons = static_cast<uint16_t>(*buttons | pressed);

    // Replace the sticks. SDL also reads the Touch controllers, so adding would double them.
    float sx = bits_float(stick_lx_bits.load());
    float sy = bits_float(stick_ly_bits.load());
    // In a level the picture is turned by the head (and smooth turning), but
    // the game reads the stick against its own camera: turn the stick by the
    // same yaw so pushing forward walks where you look.
    // `adb shell setprop debug.goemon.lookmove 0` turns it off (live).
    static uint32_t prop_reads = 0;
    static bool look_move = true;
    if ((prop_reads++ % 60u) == 0u) {
        look_move = debug_prop_int("debug.goemon.lookmove", 1) != 0;
    }
    if (look_move && now_ms() - last_aim_ms.load() < 250) {
        const float t = bits_float(move_yaw_bits.load());
        const float c = std::cos(t);
        const float s = std::sin(t);
        const float rx = sx * c - sy * s;
        const float ry2 = sx * s + sy * c;
        sx = rx;
        sy = ry2;
    }
    // A real N64 stick reaches about 85 of 127 (the PC input code scales the
    // same way), so full tilt on the Touch stick maps there.
    const float len = std::sqrt(sx * sx + sy * sy);
    if (len > 1.0f) {
        sx /= len;
        sy /= len;
    }
    constexpr float kN64StickMax = 85.0f / 127.0f;
    if (x != nullptr) {
        *x = sx * kN64StickMax;
    }
    if (y != nullptr) {
        *y = sy * kN64StickMax;
    }

    // Test switch for runs without anyone in the headset: buttons held from a
    // PC with `adb shell setprop debug.goemon.press "start a up"` (a b z r
    // start, up down left right on the stick, turnleft/turnright on the right
    // stick); "0" lets go. Counts only during a test run (test_switches_live).
    static uint32_t press_reads = 0;
    static uint16_t press_buttons = 0;
    static float press_x = 0.0f;
    static float press_y = 0.0f;
    if ((press_reads++ % 4u) == 0u) {
        char value[PROP_VALUE_MAX] = {};
        if (test_switches_live()) {
            __system_property_get("debug.goemon.press", value);
        }
        const std::string text = std::string(" ") + value + " ";
        auto has = [&text](const char* word) { return text.find(std::string(" ") + word + " ") != std::string::npos; };
        uint16_t test_pressed = 0;
        test_pressed |= has("a") ? 0x8000u : 0u;
        test_pressed |= has("b") ? 0x4000u : 0u;
        test_pressed |= has("z") ? 0x2000u : 0u;
        test_pressed |= has("start") ? 0x1000u : 0u;
        test_pressed |= has("r") ? 0x0010u : 0u;
        press_x = has("right") ? 1.0f : has("left") ? -1.0f : 0.0f;
        press_y = has("up") ? 1.0f : has("down") ? -1.0f : 0.0f;
        test_stick_rx_bits.store(float_bits(has("turnright") ? 1.0f : has("turnleft") ? -1.0f : 0.0f));
        if (test_pressed != press_buttons) {
            vr_log("test buttons now \"%s\"", value);
        }
        press_buttons = test_pressed;
    }
    *buttons = static_cast<uint16_t>(*buttons | press_buttons);
    if (x != nullptr && press_x != 0.0f) {
        *x = press_x;
    }
    if (y != nullptr && press_y != 0.0f) {
        *y = press_y;
    }
}

void start_recording_watcher() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject local_activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env == nullptr || local_activity == nullptr) {
        return;
    }
    jclass watcher = env->FindClass("com/goemon64/recompiled/RecordingWatcher");
    if (watcher == nullptr) {
        env->ExceptionClear();
        vr_log("recording watcher class is missing");
        env->DeleteLocalRef(local_activity);
        return;
    }
    jmethodID start = env->GetStaticMethodID(watcher, "start", "(Landroid/content/Context;)V");
    if (start != nullptr) {
        env->CallStaticVoidMethod(watcher, start, local_activity);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        vr_log("recording watcher failed to start");
    }
    env->DeleteLocalRef(watcher);
    env->DeleteLocalRef(local_activity);
}

} // namespace

// The shared video list changed (RecordingWatcher.java): a Quest recording
// probably finished, so the GPU hang guard can try the fast shaders again.
extern "C" JNIEXPORT void JNICALL Java_com_goemon64_recompiled_RecordingWatcher_nativeVideosChanged(JNIEnv*, jclass, jint flags) {
    RT64::RequestGpuHangFallbackRetry(static_cast<int>(flags));
}

extern "C" void bmhero_plume_add_instance_extensions(std::vector<std::string>* extensions) {
    if (extensions == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(xr_mu);
    split_extensions(instance_extensions, extensions);
}

extern "C" int bmhero_plume_select_device(VkInstance vk_instance, VkPhysicalDevice* device, std::vector<std::string>* extensions) {
    std::lock_guard<std::mutex> lock(xr_mu);
    if (device == nullptr || instance == XR_NULL_HANDLE || system_id == XR_NULL_SYSTEM_ID || xr.GetVulkanGraphicsDeviceKHR == nullptr) {
        return 0;
    }
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    if (!XR_SUCCEEDED(xr.GetVulkanGraphicsDeviceKHR(instance, system_id, vk_instance, &chosen)) || chosen == VK_NULL_HANDLE) {
        return 0;
    }
    *device = chosen;
    if (extensions != nullptr) {
        if (device_extensions.empty() && xr.GetVulkanDeviceExtensionsKHR != nullptr) {
            device_extensions = query_extension_string(nullptr, xr.GetVulkanDeviceExtensionsKHR, true);
        }
        split_extensions(device_extensions, extensions);
    }
    return 1;
}

extern "C" uint32_t bmhero_plume_clamp_vulkan_api(uint32_t requested) {
    std::lock_guard<std::mutex> lock(xr_mu);
    if (vulkan_max_api != 0 && requested > vulkan_max_api) {
        requested = vulkan_max_api;
    }
    if (requested < VK_API_VERSION_1_1) {
        requested = VK_API_VERSION_1_1;
    }
    return requested;
}

extern "C" void goemon_quest_boot() {
    std::lock_guard<std::mutex> lock(xr_mu);
    if (loader_initialized && instance != XR_NULL_HANDLE) {
        return;
    }
    vr_log("Quest OpenXR boot");
    start_recording_watcher();
    if (!create_openxr_instance() || !acquire_system()) {
        init_state = InitState::Failed;
        return;
    }
    cache_vulkan_requirements();
    vr_log("OpenXR instance is ready for the Vulkan device");
}

// Called by the anime.c patch with a copy of the camera the game is about to
// draw the level with (a0). Returns 1 when the copy was aimed with the headset.
extern "C" void recomp_vr_aim_camera(uint8_t* rdram, recomp_context* ctx) {
    if (ctx == nullptr) {
        return;
    }
    const uint32_t cam = static_cast<uint32_t>(ctx->r4);
    const bool applied = aim_camera(rdram, cam);
    aim_prev_applied = applied;
    ctx->r2 = applied ? 1 : 0;
}

// How the sky backdrop is drawn in levels (patches/background.c), live:
// `adb shell setprop debug.goemon.vrsky 2|1|0`. 2 (default): a dome in the 3D
// scene (fill_sky_dome); 1: the older flat grid around the view (fill_background);
// 0: the game's own flat sky.
int sky_mode() {
    static uint32_t prop_reads = 0;
    static int mode = 2;
    if ((prop_reads++ % 120u) == 0u) {
        mode = debug_prop_int("debug.goemon.vrsky", 2);
    }
    return mode;
}

// The sky dome (patches/background.c): a0 = the background node, a1 = the
// patch's VrSky table, a2 = 1 for the panorama mode. Returns 1 when the patch
// should draw the dome.
extern "C" void recomp_vr_sky(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
    if (sky_mode() != 2 || rdram == nullptr || now_ms() - last_aim_ms.load() > 250) {
        return;
    }
    static goemon_vr::SkyDomeState dome;
    const bool panorama = static_cast<uint32_t>(ctx->r6) != 0u;
    if (goemon_vr::fill_sky_dome(rdram, static_cast<uint32_t>(ctx->r4), static_cast<uint32_t>(ctx->r5), panorama, lock_state, dome)) {
        ctx->r2 = 1;
        static uint32_t logged_node = 0;
        static uint32_t logged_moves = 0;
        if (dome.node != logged_node) {
            logged_node = dome.node;
            vr_log("sky dome: %s %dx%d, heading 0 at texel %.1f, level at row %.1f", panorama ? "panorama" : "backdrop",
                dome.tex_w, dome.tex_h, dome.c, dome.t0);
        } else if (dome.moves != logged_moves && dome.moves <= 20) {
            logged_moves = dome.moves;
            vr_log("sky dome scroll moved (%u): heading 0 at texel %.1f, level at row %.1f", dome.moves, dome.c, dome.t0);
        }
    }
}

// Sprites (trees, bushes) face the camera copy a0 after this turns it, level, to
// the view's base heading (goemon_vr::billboard_camera). The game gives every
// sprite one angle from the camera, so they can't each face the eye; the base
// heading only turns with the right stick, so they stay still while the head turns,
// like the rooms' world (0.1.10; 5 = following the view over ~0.7 s, 0.1.7-0.1.9,
// swung every tree after each head turn). Live:
// `adb shell setprop debug.goemon.billboard <percent per frame>`: 0 (default),
// 100 = the head's direction at once (the old swinging), -1 = the game's own.
extern "C" void recomp_vr_billboard(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
    static uint32_t reads = 0;
    static int percent = 0;
    if ((reads++ % 120u) == 0u) {
        const int want = std::clamp(debug_prop_int("debug.goemon.billboard", 0), -1, 100);
        if (want != percent || reads == 1u) {
            vr_log("sprites turn toward the view at %d%% a frame%s", want, want < 0 ? " (off: the game's own, with the head)" : "");
        }
        percent = want;
    }
    if (percent < 0 || rdram == nullptr || now_ms() - last_aim_ms.load() > 250) {
        return;
    }
    if (goemon_vr::billboard_camera(rdram, static_cast<uint32_t>(ctx->r4), lock_state, static_cast<float>(percent) / 100.0f)) {
        ctx->r2 = 1;
    }
}

// The aimed camera's viewport (patches/anime.c g_vr_viewport): the full screen
// instead of the game's 1.2x-zoomed, raised TV framing, which drew the world 20% too
// big for the headset's view. Live: `adb shell setprop debug.goemon.viewport game`
// brings the game's back (default: full).
extern "C" void recomp_vr_viewport(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
    static uint32_t reads = 0;
    static bool full = true;
    if ((reads++ % 120u) == 0u) {
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("debug.goemon.viewport", value);
        const bool want = std::strcmp(value, "game") != 0;
        if (want != full || reads == 1u) {
            vr_log("3D viewport: %s", want ? "the full screen (matches the headset's view)" : "the game's own (1.2x, raised)");
        }
        full = want;
    }
    if (!full || rdram == nullptr) {
        return;
    }
    int16_t game[8] = {};
    if (goemon_vr::full_screen_viewport(rdram, static_cast<uint32_t>(ctx->r4), static_cast<uint32_t>(ctx->r5), game)) {
        static int logs = 0;
        static int16_t last[2] = {};
        if (logs < 10 && (game[0] != last[0] || game[1] != last[1])) {
            ++logs;
            last[0] = game[0];
            last[1] = game[1];
            vr_log("3D viewport: the game's is scale %.2f x %.2f, centre %.2f, %.2f; drawn full screen",
                game[0] / 4.0f, game[1] / 4.0f, game[4] / 4.0f, game[5] / 4.0f);
        }
        ctx->r2 = 1;
    }
}

// The older flat sky grid (debug.goemon.vrsky 1): drawn around the headset's
// view from the grid filled here (goemon_vr::fill_background). Same arguments
// as recomp_vr_sky, with the patch's VrBackground table.
extern "C" void recomp_vr_background(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
    if (sky_mode() != 1 || rdram == nullptr || now_ms() - last_aim_ms.load() > 250) {
        return;
    }
    HeadState head;
    {
        std::lock_guard<std::mutex> lock(head_mu);
        head = head_live;
    }
    if (!head.fov_ready) {
        return;
    }
    goemon_vr::SkyView view;
    // Lined up with the camera the game made the sky scroll for (the frame before).
    view.yaw_offset = goemon_vr::wrap_angle(bits_float(move_yaw_bits.load()) + lock_state.last_yaw - lock_state.sky_game_yaw);
    view.pitch = lock_state.view_pitch;
    view.game_pitch = lock_state.sky_game_pitch;
    view.tan_right = 1.0f / std::max(head.proj_x_scale, 0.05f);
    view.tan_up = 1.0f / std::max(head.proj_y_scale, 0.05f);
    const uint32_t node = static_cast<uint32_t>(ctx->r4);
    const uint32_t out = static_cast<uint32_t>(ctx->r5);
    const bool panorama = static_cast<uint32_t>(ctx->r6) != 0u;
    if (goemon_vr::fill_background(rdram, node, out, panorama, view)) {
        ctx->r2 = 1;
        static int logs = 0;
        if (logs < 3) {
            ++logs;
            vr_log("sky drawn around the headset's view (%s mode)", panorama ? "panorama" : "backdrop");
        }
    }
}

// PC desk tests only (the game's level select); never on the Quest.
extern "C" void recomp_desk_level_select(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r2 = 0;
}

bool goemon_vr_display_list(RT64::Application* app, const OSTask* task) {
    return on_display_list(app, task);
}

extern "C" void goemon_vr_overlay_n64_input(int player_index, uint16_t* buttons, float* x, float* y) {
    overlay_input(player_index, buttons, x, y);
}
