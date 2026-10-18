// PC stand-in for the Quest's camera hook. Without GOEMON_VR_TEST it does
// nothing: the game draws as on the PC release. With it, every level camera is
// aimed by the same code the Quest uses (src/quest/goemon_vr_camera.h), so the
// hook can be checked in a window:
//   GOEMON_VR_TEST="yaw pitch fovy pull"  degrees (yaw +left, pitch +up), fovy 0 =
//                                         the game's, pull 1 = the game's distance
//   GOEMON_VR_SWEEP="amp period"          adds amp*sin(2 pi t / period) to the yaw
//                                         (a head turning back and forth)
//   GOEMON_PRESS_FILE=<file>              keys held from a text file (see below)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "recomp.h"
#include "rhi/rt64_render_hooks.h"
#include "../quest/goemon_vr_camera.h"

namespace {

struct DeskTest {
    bool on = false;
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    float fovy_deg = 0.0f;
    float pull = 1.0f;
    float sweep_amp = 0.0f;
    float sweep_period = 0.0f;
    bool lock = true;             // world lock (GOEMON_VR_LOCK=0 turns it off)
    goemon_vr::LockState lock_state;
    float total_yaw = 0.0f;       // last aim, for turning the stick
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last = {};
    std::chrono::steady_clock::time_point last_aim = {};
    uint32_t last_proj = 0;
    // Keys from the press file.
    uint16_t held = 0;
    float sx = 0.0f;
    float sy = 0.0f;
    float turn = 0.0f;            // -1 left, +1 right
    float pull_rate = 0.0f;       // -1 in, +1 out
    bool realign = false;         // key "behind"
    bool first_person = false;    // key "fp" toggles (the Quest's right stick click)
};

DeskTest& test() {
    static DeskTest t = [] {
        DeskTest d;
        if (const char* env = std::getenv("GOEMON_VR_TEST")) {
            d.on = std::sscanf(env, "%f %f %f %f", &d.yaw_deg, &d.pitch_deg, &d.fovy_deg, &d.pull) >= 2;
        }
        if (const char* env = std::getenv("GOEMON_VR_LOCK")) {
            d.lock = std::atoi(env) != 0;
        }
        if (const char* env = std::getenv("GOEMON_VR_SWEEP")) {
            std::sscanf(env, "%f %f", &d.sweep_amp, &d.sweep_period);
        }
        if (d.on) {
            std::printf("[vrtest] aiming level cameras: yaw %.1f pitch %.1f fovy %.1f pull %.2f sweep %.0f/%.1fs\n",
                d.yaw_deg, d.pitch_deg, d.fovy_deg, d.pull, d.sweep_amp, d.sweep_period);
            std::fflush(stdout);
        }
        return d;
    }();
    return t;
}

constexpr float kPi = 3.14159265f;

float read_mtx_entry(const uint8_t* mtx, int row, int col) {
    const int stored = col ^ 1;
    int16_t integer = 0;
    uint16_t frac = 0;
    std::memcpy(&integer, mtx + (row * 4 + stored) * 2, sizeof(integer));
    std::memcpy(&frac, mtx + 32 + (row * 4 + stored) * 2, sizeof(frac));
    return static_cast<float>((static_cast<int32_t>(integer) << 16) | static_cast<int32_t>(frac)) / 65536.0f;
}

} // namespace

// The view's final heading (degrees) for traces: the game camera's plus our turn.
static float wrap_heading_deg(const goemon_vr::LockState& lock, float move_yaw) {
    return goemon_vr::wrap_angle(lock.last_yaw + move_yaw) * (180.0f / 3.14159265f);
}

// Order trace (GOEMON_VR_TRACE + trace_on.txt): aims, sky draws and display lists in order.
static bool order_trace_on() {
    static const bool trace = std::getenv("GOEMON_VR_TRACE") != nullptr;
    static int checks = 0;
    static bool on = false;
    if (!trace) {
        return false;
    }
    if ((checks++ % 20) == 0) {
        on = static_cast<bool>(std::ifstream("trace_on.txt"));
    }
    return on;
}
static int order_lines = 0;
static uint32_t aim_count = 0;

extern "C" void recomp_vr_aim_camera(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
    DeskTest& t = test();
    if (!t.on) {
        return;
    }
    const uint32_t cam = static_cast<uint32_t>(ctx->r4);
    const auto now = std::chrono::steady_clock::now();
    // Live changes: vrtest.txt in the working folder ("yaw pitch fovy pull"),
    // read twice a second, so several settings can be compared in one spot.
    static auto last_file = std::chrono::steady_clock::time_point{};
    if (now - last_file > std::chrono::milliseconds(500)) {
        last_file = now;
        std::ifstream live("vrtest.txt");
        float y = 0.0f, p = 0.0f, f = 0.0f, l = 1.0f;
        int lk = 1;
        if (live >> y >> p >> f) {
            if (!(live >> l)) {
                l = 1.0f;
            }
            if (!(live >> lk)) {
                lk = 1;
            }
            t.lock = lk != 0;
            if (y != t.yaw_deg || p != t.pitch_deg || f != t.fovy_deg || l != t.pull) {
                t.yaw_deg = y;
                t.pitch_deg = p;
                t.fovy_deg = f;
                t.pull = l;
                std::printf("[vrtest] live settings: yaw %.1f pitch %.1f fovy %.1f pull %.2f\n", y, p, f, l);
                std::fflush(stdout);
            }
        }
    }
    float dt = t.last.time_since_epoch().count() == 0 ? 0.0f : std::chrono::duration<float>(now - t.last).count();
    t.last = now;
    dt = std::min(dt, 0.1f);

    // Game state from g_system (SYS_W at 0x8008CCC0, lib/mnsg/include/common.h):
    // stepw +0x3ADD4 (u8), camera_type +0x3AE40 (u16), player_exists +0xCF88C (u8).
    {
        static uint32_t last_state = 0xFFFFFFFFu;
        const uint32_t stepw = goemon_vr::read_u8(rdram, 0x8008CCC0u + 0x3ADD4u);
        const uint32_t camera_type = goemon_vr::read_u16(rdram, 0x8008CCC0u + 0x3AE40u);
        const uint32_t player_exists = goemon_vr::read_u8(rdram, 0x8008CCC0u + 0xCF88Cu);
        const uint32_t state = stepw | (camera_type << 8) | (player_exists << 24);
        if (state != last_state) {
            last_state = state;
            std::printf("[vrtest] state: stepw %u camera_type %u player_exists %u (camera %s, fovy %.1f)%c", stepw, camera_type, player_exists,
                goemon_vr::is_level_camera(rdram, cam) ? "level" : "other", goemon_vr::read_u16(rdram, cam + goemon_vr::kCamFovy) * 360.0f / 32768.0f, 10);
            std::fflush(stdout);
        }
    }
    // Desk research: "dump_sys.txt" in the working folder writes the game's
    // system block (SYS_W 0x3ADC0..0x3B200 and 0xCF880..0xCF910) to dump_<name>.bin,
    // named by the file's content, then deletes the trigger. Also logs the player.
    {
        std::ifstream trig("dump_sys.txt");
        if (trig) {
            std::string name;
            trig >> name;
            trig.close();
            std::remove("dump_sys.txt");
            std::ofstream out("dump_" + name + ".bin", std::ios::binary);
            for (uint32_t off = 0x3ADC0u; off < 0x3B200u; ++off) {
                const char b = static_cast<char>(goemon_vr::read_u8(rdram, 0x8008CCC0u + off));
                out.write(&b, 1);
            }
            for (uint32_t off = 0xCF880u; off < 0xCF910u; ++off) {
                const char b = static_cast<char>(goemon_vr::read_u8(rdram, 0x8008CCC0u + off));
                out.write(&b, 1);
            }
            const uint32_t player = goemon_vr::read_u32(rdram, 0x801FC60Cu);
            const uint32_t root = goemon_vr::read_u32(rdram, 0x801FC614u);
            auto log_object = [&](const char* what, uint32_t obj) {
                if (!goemon_vr::valid_vram(obj)) {
                    std::printf("[dump] %s: %08X (none)%c", what, obj, 10);
                    return;
                }
                std::printf("[dump] %s %08X pos %.1f %.1f %.1f rot %d %d %d%c", what, obj,
                    goemon_vr::read_f32(rdram, obj + 0x08u), goemon_vr::read_f32(rdram, obj + 0x0Cu), goemon_vr::read_f32(rdram, obj + 0x10u),
                    static_cast<int16_t>(goemon_vr::read_u16(rdram, obj + 0x14u)), static_cast<int16_t>(goemon_vr::read_u16(rdram, obj + 0x16u)),
                    static_cast<int16_t>(goemon_vr::read_u16(rdram, obj + 0x18u)), 10);
            };
            std::printf("[dump] %s written; camera at %.1f %.1f %.1f looking at %.1f %.1f %.1f%c", name.c_str(),
                goemon_vr::read_f32(rdram, cam), goemon_vr::read_f32(rdram, cam + 4u), goemon_vr::read_f32(rdram, cam + 8u),
                goemon_vr::read_f32(rdram, cam + 12u), goemon_vr::read_f32(rdram, cam + 16u), goemon_vr::read_f32(rdram, cam + 20u), 10);
            log_object("player object", player);
            log_object("player root", root);
            std::fflush(stdout);
        }
    }
    // Log when the kind of camera changes (level or not, the game's view).
    static uint32_t last_sig = 0xFFFFFFFFu;
    static int logs = 0;
    const bool level = goemon_vr::is_level_camera(rdram, cam);
    const goemon_vr::Scissor sc = goemon_vr::read_scissor(rdram, cam);
    const float game_fovy = goemon_vr::read_u16(rdram, cam + goemon_vr::kCamFovy) * 360.0f / 32768.0f;
    const uint32_t sig = (level ? 1u : 0u) + (static_cast<uint32_t>(game_fovy) << 1) + (static_cast<uint32_t>(sc.lry - sc.uly) << 10);
    if (sig != last_sig && logs < 400) {
        last_sig = sig;
        ++logs;
        std::printf("[vrtest] %s camera: fovy %.1f scissor (%d,%d)-(%d,%d) near %.1f far %.1f pos %.0f %.0f %.0f at %.0f %.0f %.0f\n",
            level ? "level" : "other", game_fovy, sc.ulx, sc.uly, sc.lrx, sc.lry,
            goemon_vr::read_f32(rdram, cam + 0x1Cu), goemon_vr::read_f32(rdram, cam + 0x20u),
            goemon_vr::read_f32(rdram, cam), goemon_vr::read_f32(rdram, cam + 4u), goemon_vr::read_f32(rdram, cam + 8u),
            goemon_vr::read_f32(rdram, cam + 12u), goemon_vr::read_f32(rdram, cam + 16u), goemon_vr::read_f32(rdram, cam + 20u));
        std::fflush(stdout);
    }

    // Camera distance from the keys, as the Quest's right stick does.
    t.pull = std::clamp(t.pull + t.pull_rate * 0.6f * dt, goemon_vr::kMinPull, 1.0f);

    float yaw = t.yaw_deg * (kPi / 180.0f);
    if (t.sweep_amp != 0.0f && t.sweep_period > 0.0f) {
        const float secs = std::chrono::duration<float>(now - t.start).count();
        yaw += t.sweep_amp * (kPi / 180.0f) * std::sin(2.0f * kPi * secs / t.sweep_period);
    }
    goemon_vr::AimInput in;
    in.head_yaw = yaw;
    in.turn = -t.turn * 120.0f * (kPi / 180.0f) * dt;
    in.lock = t.lock;
    static const int outdoor = [] {
        const char* env = std::getenv("GOEMON_VR_OUTDOOR");
        return env == nullptr ? 1 : std::atoi(env);
    }();
    in.outdoor_mode = outdoor;
    in.realign = t.realign;
    t.realign = false;
    in.first_person = t.first_person;
    if (const char* env = std::getenv("GOEMON_VR_FP")) {
        // "height forward near" for tuning
        float h = 0.0f, f = -1.0f, n = 0.0f;
        std::sscanf(env, "%f %f %f", &h, &f, &n);
        in.fp_eye_height = h;
        in.fp_forward = f;
        in.fp_near = n;
    }
    in.pitch = t.pitch_deg * (kPi / 180.0f);
    in.fovy_deg = t.fovy_deg;
    in.pull = t.pull;
    goemon_vr::AimResult out;
    if (!goemon_vr::aim(rdram, cam, in, t.lock_state, out)) {
        return;
    }
    // GOEMON_VR_FOG: 2 radial (default), 1 near-plane corrected only, 0 as RT64 computes it.
    static const int fog_mode = [] {
        const char* env = std::getenv("GOEMON_VR_FOG");
        return env == nullptr ? 2 : std::atoi(env);
    }();
    static goemon_vr::FogNearTable fog_table;
    static bool fog_set = false;
    if (fog_table.note(out.far_plane, out.game_near) || !fog_set) {
        fog_set = true;
        RT64::SetVrFog(fog_mode == 2, fog_table.fars, fog_table.nears, fog_mode == 0 ? 0 : fog_table.count);
        std::printf("[vrtest] fog mode %d: %d cameras, last far %.0f game near %.1f%c", fog_mode, fog_table.count,
            out.far_plane, out.game_near, 10);
        std::fflush(stdout);
    }
    if (out.cut) {
        static int cuts = 0;
        if (cuts++ < 40) {
            std::printf("[vrtest] camera cut %d (reason %d): heading changed %.1f deg, moved %.0f of distance %.0f\n", static_cast<int>(t.lock_state.cuts),
                t.lock_state.cut_reason, t.lock_state.cut_delta * (180.0f / kPi), t.lock_state.cut_moved, t.lock_state.cut_dist);
            t.lock_state.cut_delta = t.lock_state.cut_moved = t.lock_state.cut_dist = 0.0f;
            std::fflush(stdout);
        }
    }
    static uint32_t station_seen = 0;
    if (t.lock_state.station_moves != station_seen) {
        station_seen = t.lock_state.station_moves;
        std::printf("[vrtest] standing camera spot %u (reason %d)%c", station_seen, t.lock_state.station_reason, 10);
        std::fflush(stdout);
    }
    static uint32_t follows_seen = 0;
    if (t.lock_state.follows != follows_seen) {
        follows_seen = t.lock_state.follows;
        std::printf("[vrtest] follow mode (%u)%c", follows_seen, 10);
        std::fflush(stdout);
    }
    static uint32_t snaps_seen = 0;
    if (t.lock_state.snaps != snaps_seen) {
        snaps_seen = t.lock_state.snaps;
        std::printf("[vrtest] world lock snapped to the game's view (%u)" "%c", snaps_seen, 10);
        std::fflush(stdout);
    }
    static const bool trace = std::getenv("GOEMON_VR_TRACE") != nullptr;
    if (trace) {
        static int traced = 0;
        if (std::ifstream("trace_on.txt") && traced < 3000) {
            ++traced;
            const uint32_t player = goemon_vr::read_u32(rdram, goemon_vr::kPlayerObjectVram);
            float pp[3] = {0, 0, 0};
            if (goemon_vr::valid_vram(player)) {
                for (int i = 0; i < 3; ++i) {
                    pp[i] = goemon_vr::read_f32(rdram, player + goemon_vr::kObjectPos + 4u * static_cast<uint32_t>(i));
                }
            }
            std::printf("[trace] heading %.2f | ", wrap_heading_deg(t.lock_state, out.move_yaw));
            std::printf("[trace] game yaw %.1f follow %d az %.1f cut %d | game eye %.1f %.1f %.1f at %.1f %.1f %.1f | player %.1f %.1f %.1f | our eye %.1f %.1f %.1f" "%c",
                t.lock_state.last_yaw * (180.0f / kPi), t.lock_state.follow ? 1 : 0, t.lock_state.follow_az * (180.0f / kPi), out.cut ? 1 : 0,
                t.lock_state.last_eye[0], t.lock_state.last_eye[1], t.lock_state.last_eye[2],
                t.lock_state.game_at[0], t.lock_state.game_at[1], t.lock_state.game_at[2], pp[0], pp[1], pp[2],
                t.lock_state.eye_out[0], t.lock_state.eye_out[1], t.lock_state.eye_out[2], 10);
            std::fflush(stdout);
        }
    }
    t.total_yaw = out.move_yaw;
    ++aim_count;
    if (order_trace_on() && order_lines < 4000) {
        ++order_lines;
        std::printf("[order] aim %u: head yaw %.2f move yaw %.2f game yaw %.2f%c", aim_count, yaw * (180.0f / kPi),
            out.move_yaw * (180.0f / kPi), t.lock_state.last_yaw * (180.0f / kPi), 10);
        std::fflush(stdout);
    }
    t.last_aim = now;
    t.last_proj = out.proj_phys;
    ctx->r2 = 1;
}

// Called from send_dl: the projection the last aimed camera wrote.
void goemon_desk_test_check_proj(uint8_t* rdram) {
    if (order_trace_on() && order_lines < 4000) {
        ++order_lines;
        std::printf("[order] dl sent (aims so far %u)%c", aim_count, 10);
        std::fflush(stdout);
    }
    static int logs = 0;
    DeskTest& t = test();
    if (!t.on || t.last_proj == 0 || logs >= 40) {
        return;
    }
    const uint8_t* m = rdram + t.last_proj;
    if (read_mtx_entry(m, 2, 3) > -0.5f) {
        return;
    }
    ++logs;
    std::printf("[vrtest] projection at %08X: [0][0] %.3f [1][1] %.3f [2][2] %.3f [2][3] %.3f [3][2] %.3f | off-centre [2][0] %.4f [2][1] %.4f [1][0] %.4f [0][1] %.4f\n", t.last_proj,
        read_mtx_entry(m, 0, 0), read_mtx_entry(m, 1, 1), read_mtx_entry(m, 2, 2), read_mtx_entry(m, 2, 3), read_mtx_entry(m, 3, 2),
        read_mtx_entry(m, 2, 0), read_mtx_entry(m, 2, 1), read_mtx_entry(m, 1, 0), read_mtx_entry(m, 0, 1));
    std::fflush(stdout);
}

// Desk tests (PC): GOEMON_PRESS_FILE names a text file read every 100 ms; the
// words in it are held: a b z r l start, cu cd cl cr (C buttons), du dd dl dr
// (D-pad), up down left right (stick), turnleft turnright (smooth turning),
// pullin pullout (camera distance). An empty file lets go.
void goemon_desk_test_input(uint16_t* buttons, float* x, float* y) {
    static const char* path = std::getenv("GOEMON_PRESS_FILE");
    if (path == nullptr) {
        return;
    }
    DeskTest& t = test();
    static auto last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::milliseconds(100)) {
        last = now;
        std::ifstream in(path);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        text = " " + text + " ";
        for (char& c : text) {
            if (c == '\n' || c == '\r' || c == '\t') {
                c = ' ';
            }
        }
        auto has = [&text](const char* w) { return text.find(std::string(" ") + w + " ") != std::string::npos; };
        uint16_t b = 0;
        b |= has("a") ? 0x8000 : 0; b |= has("b") ? 0x4000 : 0; b |= has("z") ? 0x2000 : 0; b |= has("start") ? 0x1000 : 0;
        b |= has("du") ? 0x0800 : 0; b |= has("dd") ? 0x0400 : 0; b |= has("dl") ? 0x0200 : 0; b |= has("dr") ? 0x0100 : 0;
        b |= has("l") ? 0x0020 : 0; b |= has("r") ? 0x0010 : 0;
        b |= has("cu") ? 0x0008 : 0; b |= has("cd") ? 0x0004 : 0; b |= has("cl") ? 0x0002 : 0; b |= has("cr") ? 0x0001 : 0;
        t.held = b;
        t.sx = has("right") ? 1.0f : has("left") ? -1.0f : 0.0f;
        t.sy = has("up") ? 1.0f : has("down") ? -1.0f : 0.0f;
        t.turn = has("turnright") ? 1.0f : has("turnleft") ? -1.0f : 0.0f;
        t.pull_rate = has("pullout") ? 1.0f : has("pullin") ? -1.0f : 0.0f;
        static bool fp_was = false;
        const bool fp = has("fp");
        if (fp && !fp_was) {
            t.first_person = !t.first_person;
            std::printf("[vrtest] first person %s%c", t.first_person ? "on" : "off", 10);
            std::fflush(stdout);
        }
        fp_was = fp;
        static bool behind_was = false;
        const bool behind = has("behind");
        if (behind && !behind_was) {
            t.realign = true;
        }
        behind_was = behind;
    }
    *buttons |= t.held;
    if (t.sx != 0.0f || t.sy != 0.0f) {
        float sx = t.sx;
        float sy = t.sy;
        // Same as the Quest's overlay_input: in a level, forward walks where the view looks.
        if (t.on && now - t.last_aim < std::chrono::milliseconds(250)) {
            const float c = std::cos(t.total_yaw);
            const float s = std::sin(t.total_yaw);
            const float rx = sx * c - sy * s;
            const float ry = sx * s + sy * c;
            sx = rx;
            sy = ry;
        }
        constexpr float kN64StickMax = 85.0f / 127.0f;
        *x = sx * kN64StickMax;
        *y = sy * kN64StickMax;
    }
}

// GOEMON_LEVEL_SELECT: the game's own level select on L + Z + D-right (patches/main.c).
extern "C" void recomp_desk_level_select(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    static const bool on = std::getenv("GOEMON_LEVEL_SELECT") != nullptr;
    ctx->r2 = on ? 1 : 0;
}

// How the sky is drawn, like the Quest's debug.goemon.vrsky: GOEMON_VR_SKY=2
// (default) a dome in the 3D scene, 1 the older flat grid, 0 the game's flat sky.
static int desk_sky_mode() {
    static const int mode = [] {
        const char* env = std::getenv("GOEMON_VR_SKY");
        return env == nullptr ? 2 : std::atoi(env);
    }();
    return mode;
}

// The sky dome (patches/background.c), as on the Quest.
extern "C" void recomp_vr_sky(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
    DeskTest& t = test();
    if (!t.on || desk_sky_mode() != 2 || std::chrono::steady_clock::now() - t.last_aim > std::chrono::milliseconds(250)) {
        return;
    }
    static goemon_vr::SkyDomeState dome;
    const bool panorama = static_cast<uint32_t>(ctx->r6) != 0u;
    if (goemon_vr::fill_sky_dome(rdram, static_cast<uint32_t>(ctx->r4), static_cast<uint32_t>(ctx->r5), panorama, t.lock_state, dome)) {
        ctx->r2 = 1;
        static uint32_t logged_node = 0;
        static uint32_t logged_moves = 0;
        static int details = 0;
        if (details++ == 30) {
            const uint32_t out = static_cast<uint32_t>(ctx->r5);
            std::printf("[vrtest] sky dome table: far %.1f eye %.0f %.0f %.0f scale %.3f, cell 0 rect %d %d %d %d corner0 %d %d %d st %d %d, corner2 %d %d %d st %d %d%c",
                t.lock_state.far_plane, t.lock_state.eye_out[0], t.lock_state.eye_out[1], t.lock_state.eye_out[2],
                goemon_vr::read_f32(rdram, out + 0x10u),
                (int)goemon_vr::read_u32(rdram, out + 0x50u), (int)goemon_vr::read_u32(rdram, out + 0x54u), (int)goemon_vr::read_u32(rdram, out + 0x58u), (int)goemon_vr::read_u32(rdram, out + 0x5Cu),
                (int)goemon_vr::read_u32(rdram, out + 0x60u), (int)goemon_vr::read_u32(rdram, out + 0x64u), (int)goemon_vr::read_u32(rdram, out + 0x68u),
                (int)goemon_vr::read_u32(rdram, out + 0x6Cu), (int)goemon_vr::read_u32(rdram, out + 0x70u),
                (int)goemon_vr::read_u32(rdram, out + 0x88u), (int)goemon_vr::read_u32(rdram, out + 0x8Cu), (int)goemon_vr::read_u32(rdram, out + 0x90u),
                (int)goemon_vr::read_u32(rdram, out + 0x94u), (int)goemon_vr::read_u32(rdram, out + 0x98u), 10);
            const uint32_t node = static_cast<uint32_t>(ctx->r4);
            std::printf("[vrtest] sky node flags %04X tex %d x %d corner %.1f %.1f rect %.1f x %.1f%c", goemon_vr::read_u16(rdram, node + 0x0Eu),
                (int)(int16_t)goemon_vr::read_u16(rdram, node + 0x10u), (int)(int16_t)goemon_vr::read_u16(rdram, node + 0x12u),
                goemon_vr::read_f32(rdram, node + 0x1Cu), goemon_vr::read_f32(rdram, node + 0x20u), goemon_vr::read_f32(rdram, node + 0x24u), goemon_vr::read_f32(rdram, node + 0x28u), 10);
            std::fflush(stdout);
        }
        if (dome.node != logged_node || (dome.moves != logged_moves && dome.moves <= 40)) {
            logged_node = dome.node;
            logged_moves = dome.moves;
            std::printf("[vrtest] sky dome (%u moves): %s %dx%d, heading 0 at texel %.1f, level at row %.1f, %d cells%c", dome.moves,
                panorama ? "panorama" : "backdrop", dome.tex_w, dome.tex_h, dome.c, dome.t0,
                static_cast<int>(goemon_vr::read_u32(rdram, static_cast<uint32_t>(ctx->r5))), 10);
            std::fflush(stdout);
        }
    }
    if (order_trace_on() && order_lines < 4000) {
        ++order_lines;
        const uint32_t node = static_cast<uint32_t>(ctx->r4);
        std::printf("[order] dome after aim %u: start_x %.2f start_y %.2f sky game yaw %.2f pitch %.2f texel %.2f row %.2f%c", aim_count,
            goemon_vr::read_f32(rdram, node + 0x14u), goemon_vr::read_f32(rdram, node + 0x18u),
            t.lock_state.sky_game_yaw * (180.0f / kPi), t.lock_state.sky_game_pitch * (180.0f / kPi), dome.c, dome.t0, 10);
        std::fflush(stdout);
    }
}

// Sprites like on the Quest: GOEMON_VR_BILLBOARD=<percent per frame> (default 0;
// 0 = the base heading, 100 = the view at once, -1 = the game's own).
extern "C" void recomp_vr_billboard(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
    DeskTest& t = test();
    static const int percent = [] {
        const char* env = std::getenv("GOEMON_VR_BILLBOARD");
        return env == nullptr ? 0 : std::atoi(env);
    }();
    if (!t.on || percent < 0 || std::chrono::steady_clock::now() - t.last_aim > std::chrono::milliseconds(250)) {
        return;
    }
    if (goemon_vr::billboard_camera(rdram, static_cast<uint32_t>(ctx->r4), t.lock_state, static_cast<float>(percent) / 100.0f)) {
        ctx->r2 = 1;
    }
}

// The aimed camera's viewport like on the Quest: GOEMON_VR_VIEWPORT=game keeps the
// game's 1.2x-zoomed, raised one (default: full screen).
extern "C" void recomp_vr_viewport(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
    static const bool full = [] {
        const char* env = std::getenv("GOEMON_VR_VIEWPORT");
        return env == nullptr || std::strcmp(env, "game") != 0;
    }();
    if (!test().on || !full) {
        return;
    }
    int16_t game[8] = {};
    if (goemon_vr::full_screen_viewport(rdram, static_cast<uint32_t>(ctx->r4), static_cast<uint32_t>(ctx->r5), game)) {
        static int logs = 0;
        if (logs++ < 3) {
            std::printf("[vrtest] 3D viewport: the game's is scale %.2f x %.2f, centre %.2f, %.2f; drawn full screen%c",
                game[0] / 4.0f, game[1] / 4.0f, game[4] / 4.0f, game[5] / 4.0f, 10);
            std::fflush(stdout);
        }
        ctx->r2 = 1;
    }
}

// The older flat sky grid (GOEMON_VR_SKY=1), drawn around the test view. The picture
// shape for the grid is the GOEMON_VR_HUD aspect if given (the Quest's), otherwise
// the window's 4:3 layout.
extern "C" void recomp_vr_background(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
    DeskTest& t = test();
    const bool enabled = desk_sky_mode() == 1;
    static const float aspect = [] {
        float a = 0.0f, sx = 0.0f, sy = 0.0f;
        const char* env = std::getenv("GOEMON_VR_HUD");
        if (env != nullptr && std::sscanf(env, "%f %f %f", &a, &sx, &sy) >= 1 && a > 0.1f) {
            return a;
        }
        return 4.0f / 3.0f;
    }();
    if (!t.on || !enabled || t.fovy_deg <= 1.0f || std::chrono::steady_clock::now() - t.last_aim > std::chrono::milliseconds(250)) {
        return;
    }
    goemon_vr::SkyView view;
    view.yaw_offset = goemon_vr::wrap_angle(t.total_yaw + t.lock_state.last_yaw - t.lock_state.sky_game_yaw);
    view.pitch = t.lock_state.view_pitch;
    view.game_pitch = t.lock_state.sky_game_pitch;
    view.tan_up = std::tan(t.fovy_deg * 0.5f * (kPi / 180.0f));
    view.tan_right = view.tan_up * aspect;
    if (goemon_vr::fill_background(rdram, static_cast<uint32_t>(ctx->r4), static_cast<uint32_t>(ctx->r5), static_cast<uint32_t>(ctx->r6) != 0u, view)) {
        ctx->r2 = 1;
    }
    if (order_trace_on() && order_lines < 4000) {
        ++order_lines;
        const uint32_t node = static_cast<uint32_t>(ctx->r4);
        std::printf("[order] sky after aim %u: move yaw %.2f start_x %.2f start_y %.2f%c", aim_count, view.yaw_offset * (180.0f / kPi),
            goemon_vr::read_f32(rdram, node + 0x14u), goemon_vr::read_f32(rdram, node + 0x18u), 10);
        std::fflush(stdout);
    }
}
