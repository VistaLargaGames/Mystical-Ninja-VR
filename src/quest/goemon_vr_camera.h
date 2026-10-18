#pragma once
// Goemon's VR camera: which cameras are the level's, and how a copy of one is
// turned for the headset. Shared by the Quest (openxr_quest.cpp) and the PC
// desk test (src/game/vr_desk_test.cpp), so the PC checks exactly this code.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace goemon_vr {

// Goemon draws a level through a Camera object (patches/types.h): position at
// 0x00, look_at at 0x0C, the vertical field of view at 0x1A (u16, 32768 = 360
// degrees: func_80017D8C_1898C passes u16 * 360 / 32768 to guPerspective with
// aspect 4:3), near/far at 0x1C/0x20, bit 0 of byte 0x3E = orthographic, and the
// scissor (u16 ulx, uly, lrx, lry) at 0x48. The anime.c patch hands us a copy
// just before func_80017D8C_1898C builds the projection and view from it, so
// the game's own camera is never changed and nothing has to be put back.
constexpr uint32_t kCamPos = 0x00u;
constexpr uint32_t kCamAt = 0x0Cu;
constexpr uint32_t kCamFovy = 0x1Au;
constexpr uint32_t kCamFlags = 0x3Eu;
constexpr uint32_t kCamScissor = 0x48u;
// The display list being built (Gfx*, D_8015C5CC) and the matrix pool
// (D_80168504): func_80017D8C_1898C writes the projection at the pool's current
// address. Both tie a camera to the task that draws it.
constexpr uint32_t kGfxHeadVram = 0x8015C5CCu;
constexpr uint32_t kMtxPoolVram = 0x80168504u;
// g_player_1_task / g_player_1_object (patches/variables.h): zero on the title
// and menus (whose 3D backgrounds use the same Camera object), set while a
// player exists (gameplay and in-engine scenes).
constexpr uint32_t kPlayerTaskVram = 0x801FC604u;
constexpr uint32_t kPlayerObjectVram = 0x801FC60Cu;
// g_system (SYS_W at 0x8008CCC0) + 0x3AE26 (u16): 1 while the game is paused, 0 in
// play (found by diffing the block in play and in the pause menu on PC). The pause
// menu draws a 3D backdrop with the same Camera object (eye 0,0,100 looking at 0,0,0).
constexpr uint32_t kPausedVram = 0x8008CCC0u + 0x3AE26u;
// The player object (Object, patches/types.h): position Vec3f at 0x08, rotation
// Vec3s at 0x14; rotation.y at 0x16 is the facing, 1024 units a turn, measured like
// atan2(x, z) (checked on PC against the walking direction).
constexpr uint32_t kObjectPos = 0x08u;
constexpr uint32_t kObjectRotY = 0x16u;

// KSEG0 addresses only (the recomp has 512 MB; the camera copy is a patch global at 0x808xxxxx).
inline bool valid_vram(uint32_t vram) {
    return vram >= 0x80000000u && vram <= 0x9FFFFF00u;
}
inline uint8_t* ram(uint8_t* rdram, uint32_t vram) {
    return rdram + (vram - 0x80000000u);
}
inline float read_f32(uint8_t* rdram, uint32_t vram) {
    float v;
    std::memcpy(&v, ram(rdram, vram), 4);
    return v;
}
inline void write_f32(uint8_t* rdram, uint32_t vram, float v) {
    std::memcpy(ram(rdram, vram), &v, 4);
}
inline uint32_t read_u32(uint8_t* rdram, uint32_t vram) {
    uint32_t v;
    std::memcpy(&v, ram(rdram, vram), 4);
    return v;
}
// RDRAM holds native 32-bit words: a halfword lives at address ^ 2, a byte at ^ 3.
inline uint16_t read_u16(uint8_t* rdram, uint32_t vram) {
    uint16_t v;
    std::memcpy(&v, ram(rdram, vram ^ 2u), 2);
    return v;
}
inline void write_u16(uint8_t* rdram, uint32_t vram, uint16_t v) {
    std::memcpy(ram(rdram, vram ^ 2u), &v, 2);
}
inline uint8_t read_u8(uint8_t* rdram, uint32_t vram) {
    return *ram(rdram, vram ^ 3u);
}

struct Scissor {
    int ulx = 0, uly = 0, lrx = 0, lry = 0;
};

inline Scissor read_scissor(uint8_t* rdram, uint32_t cam) {
    return Scissor{read_u16(rdram, cam + kCamScissor + 0u), read_u16(rdram, cam + kCamScissor + 2u),
        read_u16(rdram, cam + kCamScissor + 4u), read_u16(rdram, cam + kCamScissor + 6u)};
}

// The level's camera: perspective, drawing (nearly) the whole screen, with a
// player in the game. Letterboxed cutscenes ((0,38)-(320,190)) fail the size
// test and stay on the floating screen, as do the title and menus.
inline bool is_level_camera(uint8_t* rdram, uint32_t cam) {
    if (rdram == nullptr || !valid_vram(cam)) {
        return false;
    }
    if ((read_u8(rdram, cam + kCamFlags) & 1u) != 0u) {
        return false;
    }
    const Scissor sc = read_scissor(rdram, cam);
    if (sc.lrx - sc.ulx < 280 || sc.lry - sc.uly < 180) {
        return false;
    }
    if (read_u16(rdram, kPausedVram) != 0u) {
        return false;
    }
    return read_u32(rdram, kPlayerTaskVram) != 0u && read_u32(rdram, kPlayerObjectVram) != 0u;
}

// Where Goemon stands (his feet), if known.
inline bool player_position(uint8_t* rdram, float pos[3]) {
    const uint32_t obj = read_u32(rdram, kPlayerObjectVram);
    if (!valid_vram(obj)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        pos[i] = read_f32(rdram, obj + kObjectPos + 4u * static_cast<uint32_t>(i));
        if (!std::isfinite(pos[i])) {
            return false;
        }
    }
    return true;
}

// The way Goemon faces (radians, atan2(x, z) like the camera headings), if known.
inline bool player_heading(uint8_t* rdram, float& heading) {
    const uint32_t obj = read_u32(rdram, kPlayerObjectVram);
    if (!valid_vram(obj)) {
        return false;
    }
    const int units = static_cast<int16_t>(read_u16(rdram, obj + kObjectRotY)) & 0x3FF;
    heading = static_cast<float>(units) * (2.0f * 3.14159265f / 1024.0f);
    return true;
}

struct AimInput {
    float head_yaw = 0.0f;      // radians, +left: the head's yaw (turns the view about the eye)
    float turn = 0.0f;          // radians, +left: smooth turning (right stick) since the last camera
    float pitch = 0.0f;         // radians, +up: the head's pitch from level
    float fovy_deg = 0.0f;      // vertical view to draw (0 = keep the game's)
    float pull = 1.0f;          // eye distance as a share of the game's (1 = unchanged)
    bool lock = true;           // keep the camera's direction in the world (see below)
    bool reset = false;         // back to the game's own camera direction
    bool realign = false;       // right stick click: the camera goes right behind Goemon
    // The head's movement since the headset position was locked (meters, headset
    // space: x right, y up, z back) and how many game units a meter is (0 = off).
    float head_offset[3] = {0.0f, 0.0f, 0.0f};
    float units_per_meter = 0.0f;
    // Outdoors (where the game's camera follows Goemon): 1 = the eye chases Goemon,
    // gliding after him (default, owner after 0.1.11: "the camera has to chase the
    // player"); 2 = a standing camera like the rooms', cut to a new spot when Goemon
    // leaves it; 0 = the rooms' logic everywhere.
    int outdoor_mode = 1;
    // First person (the right stick click toggles it): the eye in Goemon's head,
    // the view turned by the head and the right stick, Goemon facing the view.
    bool first_person = false;
    float fp_eye_height = 0.0f;   // 0 = kFirstPersonEyeHeight
    float fp_forward = -1.0f;     // < 0 = kFirstPersonForward
    float fp_near = 0.0f;         // 0 = kFirstPersonNear
};

struct AimResult {
    float pitch = 0.0f;         // pitch the camera now looks at (radians, +up)
    float move_yaw = 0.0f;      // how far the view is turned from the game's camera (radians, +left);
                                // the stick is turned by this so forward walks where you look
    bool cut = false;           // the game's camera jumped (new room, cutscene angle)
    uint32_t dl_phys = 0;       // display list head when the camera was drawn
    uint32_t proj_phys = 0;     // where its projection matrix goes
    float game_fovy_deg = 0.0f; // the game's own vertical view
    float game_near = 0.0f;     // the game's own near distance, if we moved it (else 0)
    float far_plane = 0.0f;     // ... and the camera's far distance (for the fog, RT64::SetVrFog)
    bool first_person = false;  // the eye is in Goemon's head
};

// Far distance -> the game's near distance, for the cameras seen lately (the game
// draws several per frame, with different planes). RT64 gives each projection the
// fog its own near plane gives (RT64::SetVrFog).
struct FogNearTable {
    float fars[4] = {};
    float nears[4] = {};
    int count = 0;
    // True if the table changed.
    bool note(float far_plane, float game_near) {
        if (!(game_near > 0.0f) || !(far_plane > game_near)) {
            return false;
        }
        for (int i = 0; i < count; i++) {
            if (std::fabs(fars[i] - far_plane) <= far_plane * 0.01f) {
                if (nears[i] == game_near) {
                    return false;
                }
                nears[i] = game_near;
                return true;
            }
        }
        if (count == 4) {
            for (int i = 1; i < 4; i++) {
                fars[i - 1] = fars[i];
                nears[i - 1] = nears[i];
            }
            count = 3;
        }
        fars[count] = far_plane;
        nears[count] = game_near;
        ++count;
        return true;
    }
};

// What the world lock remembers between cameras.
struct LockState {
    bool have_last = false;
    float last_yaw = 0.0f;      // the game camera's heading (radians)
    float last_pitch = 0.0f;    // the game camera's tilt (radians, +up)
    float last_eye[3] = {};
    float absorbed = 0.0f;      // the game's own turning we cancelled (radians, +left)
    float turned = 0.0f;        // the player's smooth turning (radians, +left)
    uint32_t snaps = 0;         // times the absorbed turning snapped back (for logs)
    // Follow mode: once the game's camera starts following Goemon (it moves with
    // him, outdoors), the eye keeps its own compass direction around him instead of
    // the game's, which swings behind him by itself ("auto realign"). Rooms whose
    // camera stands still stay in the game's view. A camera jump ends it.
    bool follow = false;
    float follow_az = 0.0f;     // direction of the eye from Goemon (radians, atan2(x, z))
    float follow_dist = 0.0f;   // the eye's distance from Goemon across the ground (follows the game's slowly)
    float follow_ground = 0.0f; // Goemon's ground height, smoothed: the eye sits a fixed height above it
    bool station_set = false;   // standing camera outdoors: where it stands
    float station_eye[3] = {};
    uint32_t station_moves = 0; // times it cut to a new spot (for logs)
    bool chase_set = false;     // chase camera outdoors: where the eye is now
    float chase_eye[3] = {};
    bool fp_active = false;     // first person: on
    float fp_heading = 0.0f;    // ... the body's heading (the head turns on top of it)
    uint32_t fp_toggles = 0;    // times it was switched (for logs)
    int station_reason = 0;     // why it last moved: 1 too far, 2 too close, 3 off to the side, 4 height
    float feet_vy = 0.0f;       // how far Goemon's feet moved up since the last level camera
    uint32_t feet_steady = 0;   // level cameras in a row his feet barely moved up or down
    uint32_t follows = 0;       // times follow mode started (for logs)
    // Why the last cut happened (for logs): heading change and eye move that frame.
    float cut_delta = 0.0f;
    float cut_moved = 0.0f;
    float cut_dist = 0.0f;
    uint32_t cuts = 0;          // camera cuts seen (for logs)
    uint32_t follow_ends = 0;   // times follow mode ended (for logs)
    int cut_reason = 0;         // last cut: 1 turn, 2 eye jump, 3 Goemon moved far, 4 no level camera for a while, 5 first camera
    uint32_t misses = 0;        // calls in a row that weren't a level camera
    bool have_feet = false;
    float last_feet[3] = {};    // where Goemon stood at the last level camera
    // For the sky (fill_sky_dome / fill_background), from the last aim. The game
    // scrolls its sky from the camera of the frame before (PC trace: the scroll
    // follows the heading one frame late), so the sky is lined up with that one.
    float sky_game_yaw = 0.0f;  // the game camera's heading the sky scroll was made for
    float sky_game_pitch = 0.0f;
    float view_pitch = 0.0f;    // the tilt the view was drawn with (radians, +up)
    float eye_out[3] = {};      // where the view was drawn from
    float game_at[3] = {};      // the game camera's own look-at point (for logs)
    float base_heading = 0.0f;  // the view's heading without the head (radians)
    float view_heading = 0.0f;  // the view's heading with the head (radians)
    float sprite_heading = 0.0f;// the heading sprites face: follows view_heading slowly
    bool have_sprite = false;
    float far_plane = 0.0f;     // the camera's far distance
    float game_near = 0.0f;     // the game's own near distance (we write kVrNearPlane)
};

constexpr float kMinPull = 0.15f;
// The game's near plane (20 units outdoors) cut the ground off in a straight line
// that stayed put in the view when the headset looked down near Goemon; the
// headset draws from this close instead (the depth buffer has precision to spare).
constexpr float kVrNearPlane = 4.0f;
// Follow mode: the eye's distance from Goemon follows the game camera's this much
// per frame (about 4 seconds at 30 frames a second), kept within these bounds.
constexpr float kFollowDistRate = 0.008f;
constexpr float kFollowMinDist = 25.0f;
constexpr float kFollowMaxDist = 90.0f;
// Follow mode ends only when Goemon (or the game's eye, beyond 3x its distance)
// moves this far in one frame: a new place, not walking (a few units a frame).
constexpr float kFollowCutMove = 60.0f;
// Follow mode height: the eye sits this far above Goemon's ground (his head,
// where the game's look-at point is when he stands). The ground estimate follows
// his feet down quickly (stepping off a ledge) but up only slowly, so a jump
// (15 units up and back in about a second) barely lifts the view. Before, the
// eye rode the game camera's height, which follows jumps and then sinks back over
// 1.5 s: the world bobbed by about a meter at every jump.
constexpr float kFollowEyeHeight = 30.0f;  // was 21 (his head): the owner's recording showed his hair
                                           // filling the view a few units from the eyes
constexpr float kFollowGroundDown = 0.2f;    // share of the gap closed per frame when he stands lower
constexpr float kFollowGroundUp = 0.1f;      // same when he stands higher (a step, a platform)
// The standing outdoor camera cuts to a new spot behind Goemon when he gets this
// far from it across the ground, this close, or this far off to the side of the
// view's base direction (the head can still look anywhere).
constexpr float kStationFar = 130.0f;
constexpr float kStationNear = 12.0f;
constexpr float kStationSideDeg = 70.0f;
// The chase camera glides this share of the way to its spot behind Goemon each game
// frame (30 a second: ~0.35 s to catch up), so his starts, stops and steps don't jerk
// the whole world; RT64's in-between pictures smooth it further.
constexpr float kChaseRate = 0.09f;
// First person: the eye this far above Goemon's feet and this far ahead of his
// centre (his eyes), and a near plane that cuts away his own head around it, so
// the body below and his arms (attacks) stay in view.
constexpr float kFirstPersonEyeHeight = 19.0f;
constexpr float kFirstPersonForward = 3.0f;
constexpr float kFirstPersonNear = 7.0f;
constexpr float kStationSideMinDist = 25.0f; // the side check only counts from this far (close up the angle swings fast)
constexpr float kStationRise = 50.0f;        // ... or when his ground is this far above/below the eye's
// Walking back toward the camera: the new spot is this far behind him (the game's
// own outdoor distance), so the next cut is a few seconds away, not 13 units.
constexpr float kStationBackMin = 45.0f;
constexpr float kStationBackMax = 80.0f;
constexpr float kFollowSteadyMove = 0.6f;    // feet moving less than this per frame (up or down) count as steady
constexpr uint32_t kFollowSteadyFrames = 6;  // the height follows only after this many steady frames (jump apex, falls: held)
constexpr float kSnapDeg = 70.0f;
constexpr float kPi = 3.14159265f;

inline float wrap_angle(float a) {
    return std::remainder(a, 2.0f * kPi);
}

// Turns the camera copy for the headset. Returns false (and changes nothing)
// for anything that isn't a usable level camera.
//
// World lock (in.lock): Goemon's camera turns by itself to keep him in view,
// mostly from a spot that stays put (it tracks him as he walks around) and
// sometimes swinging behind him. In a headset that turning spins the whole world
// around you, which makes people sick. With the lock the view keeps its compass
// direction while the eye still goes where the game's camera goes (pulled toward
// Goemon): you stand where the camera stands and follow Goemon with your head;
// only the head and the right stick turn the view. If the game has turned more
// than kSnapDeg away from the held direction (Goemon about to leave your view),
// the view snaps to the game's in one step, like a comfort snap turn. A jump of
// the game's camera (a new room or angle) starts again from the game's view, and
// so does a right stick click. Without the lock the game's own turning shows.
inline bool aim(uint8_t* rdram, uint32_t cam, const AimInput& in, LockState& lock, AimResult& out) {
    if (!is_level_camera(rdram, cam)) {
        // Other cameras (a 3D picture in the HUD or a menu, a fade) come and go
        // between level frames; only a long stretch without the level (a menu,
        // a scene change) forgets the lock. Each one used to: the next level frame
        // then counted as a camera cut, which ended follow mode and snapped the
        // view to the game's swinging camera.
        if (++lock.misses > 30u) {
            lock.have_last = false;
            lock.have_feet = false;
        }
        return false;
    }
    lock.misses = 0;
    float pos[3] = {read_f32(rdram, cam + kCamPos), read_f32(rdram, cam + kCamPos + 4u), read_f32(rdram, cam + kCamPos + 8u)};
    const float at[3] = {read_f32(rdram, cam + kCamAt), read_f32(rdram, cam + kCamAt + 4u), read_f32(rdram, cam + kCamAt + 8u)};
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(pos[i]) || !std::isfinite(at[i])) {
            return false;
        }
    }
    const float fx0 = at[0] - pos[0];
    const float fy0 = at[1] - pos[1];
    const float fz0 = at[2] - pos[2];
    const float dist = std::sqrt(fx0 * fx0 + fy0 * fy0 + fz0 * fz0);
    if (dist < 1.0f) {
        return false;
    }
    const float flat = std::sqrt(fx0 * fx0 + fz0 * fz0);

    // The game camera's heading, and whether it jumped.
    const float game_yaw = flat > 0.05f * dist ? std::atan2(fx0, fz0) : lock.last_yaw;
    bool cut = !lock.have_last || in.reset;
    if (cut) {
        lock.cut_reason = 5;
    }
    float feet_now[3];
    const bool have_feet_now = player_position(rdram, feet_now);
    float feet_moved = 0.0f;
    lock.feet_vy = 0.0f;
    if (have_feet_now && lock.have_feet) {
        const float dx = feet_now[0] - lock.last_feet[0];
        const float dy = feet_now[1] - lock.last_feet[1];
        const float dz = feet_now[2] - lock.last_feet[2];
        feet_moved = std::sqrt(dx * dx + dy * dy + dz * dz);
        lock.feet_vy = dy;
    }
    lock.feet_steady = std::fabs(lock.feet_vy) < kFollowSteadyMove ? lock.feet_steady + 1u : 0u;
    if (lock.have_last) {
        const float delta = wrap_angle(game_yaw - lock.last_yaw);
        const float mx = pos[0] - lock.last_eye[0];
        const float my = pos[1] - lock.last_eye[1];
        const float mz = pos[2] - lock.last_eye[2];
        const float moved = std::sqrt(mx * mx + my * my + mz * mz);
        // A new camera, not the game following Goemon: more than 20 degrees of
        // turn in one frame, or the eye jumped by half its distance. While
        // following Goemon the game camera's own swings don't matter (the view
        // hangs on Goemon), so then only a real jump counts: Goemon or the eye
        // moved far in one frame (a new place).
        int reason = 0;
        if (lock.follow) {
            if (feet_moved > kFollowCutMove) {
                reason = 3;
            } else if (moved > 3.0f * dist && moved > kFollowCutMove) {
                reason = 2;
            }
        } else if (std::fabs(delta) > 20.0f * (kPi / 180.0f)) {
            reason = 1;
        } else if (moved > 0.5f * dist) {
            reason = 2;
        }
        if (reason != 0) {
            cut = true;
            lock.cut_reason = reason;
            lock.cut_delta = delta;
            lock.cut_moved = reason == 3 ? feet_moved : moved;
            lock.cut_dist = dist;
        } else if (in.lock) {
            // Cancel the game's own turning, up to kSnapDeg.
            lock.absorbed = wrap_angle(lock.absorbed - delta);
            if (std::fabs(lock.absorbed) > kSnapDeg * (kPi / 180.0f)) {
                lock.absorbed = 0.0f;
                ++lock.snaps;
            }
        }
    }
    if (cut || !in.lock) {
        lock.absorbed = 0.0f;
        lock.turned = 0.0f;
    }
    // Follow mode starts when the game's eye moves (it follows Goemon) or on the
    // right stick click (straight behind Goemon); a camera jump ends it.
    bool eye_moving = false;
    if (lock.have_last && !cut) {
        const float mx = pos[0] - lock.last_eye[0];
        const float my = pos[1] - lock.last_eye[1];
        const float mz = pos[2] - lock.last_eye[2];
        eye_moving = (mx * mx + my * my + mz * mz) > 0.25f;
    }
    if (cut) {
        ++lock.cuts;
        if (lock.follow) {
            ++lock.follow_ends;
        }
        lock.follow = false;
    }
    if (have_feet_now) {
        lock.last_feet[0] = feet_now[0];
        lock.last_feet[1] = feet_now[1];
        lock.last_feet[2] = feet_now[2];
        lock.have_feet = true;
    }
    float facing = 0.0f;
    // The right stick click only works while following (outdoors): in rooms with
    // a standing camera it pulled the view behind Goemon, through walls (owner, test 4).
    const bool start_follow_behind = in.lock && in.realign && lock.follow && player_heading(rdram, facing);
    const bool start_follow = in.outdoor_mode != 0 && (start_follow_behind || (in.lock && !lock.follow && eye_moving && flat > 1.0f));
    if (start_follow) {
        // Keep looking the way the view looks now (or straight past Goemon for the
        // right stick click), from the game camera's distance at this moment.
        const float view_heading = game_yaw + lock.absorbed + lock.turned;
        lock.follow_az = wrap_angle((start_follow_behind ? facing : view_heading) + kPi);
        if (!lock.follow) {
            lock.follow_dist = std::clamp(std::sqrt((pos[0] - at[0]) * (pos[0] - at[0]) + (pos[2] - at[2]) * (pos[2] - at[2])),
                kFollowMinDist, kFollowMaxDist);
            float start_feet[3];
            lock.follow_ground = player_position(rdram, start_feet) ? start_feet[1] : at[1] - kFollowEyeHeight;
        }
        lock.follow = true;
        lock.absorbed = 0.0f;
        lock.turned = 0.0f;
        ++lock.follows;
    }
    if (!in.lock || in.outdoor_mode == 0) {
        lock.follow = false;
    }
    if (!lock.follow || start_follow) {
        lock.station_set = false;   // a new standing spot when following starts again
        lock.chase_set = false;
    }
    const float game_pitch = std::atan2(fy0, std::max(flat, 1.0e-3f));
    const bool steady = lock.have_last && !cut;
    lock.sky_game_yaw = steady ? lock.last_yaw : game_yaw;
    lock.sky_game_pitch = steady ? lock.last_pitch : game_pitch;
    lock.have_last = true;
    lock.last_yaw = game_yaw;
    lock.last_pitch = game_pitch;
    lock.last_eye[0] = pos[0];
    lock.last_eye[1] = pos[1];
    lock.last_eye[2] = pos[2];
    if (lock.follow) {
        // The stick orbits the eye around Goemon.
        lock.follow_az = wrap_angle(lock.follow_az + in.turn);
    } else {
        lock.turned = wrap_angle(lock.turned + in.turn);
    }

    // The eye: where the game's camera is, pulled toward its look-at point
    // (the game films from far away with a narrow lens, which looks tiny
    // through the headset's wide view).
    const float pull = std::clamp(in.pull, kMinPull, 1.0f);
    float center[3] = {at[0], at[1], at[2]};
    float ex = pos[0] - at[0];
    float ey = pos[1] - at[1];
    float ez = pos[2] - at[2];
    float feet[3];
    if (lock.follow && player_position(rdram, feet)) {
        // Around Goemon himself. The game's look-at point wanders up to 15 units
        // ahead of or behind him and its distance stretches (45 to 64 units) as it
        // lags, which slid and zoomed the whole view as he moved ("the world warps
        // outside"). The distance follows the game's over a few seconds (no
        // frame-to-frame zoom); the height stays the game's (its look-at height
        // doesn't jump with him), so a jump doesn't bounce the view.
        const float game_dist = std::sqrt(ex * ex + ez * ez);
        lock.follow_dist += (std::clamp(game_dist, kFollowMinDist, kFollowMaxDist) - lock.follow_dist) * kFollowDistRate;
        // Hold the height while he is in the air; otherwise follow his ground.
        if (lock.feet_steady >= kFollowSteadyFrames) {
            const float gap = feet[1] - lock.follow_ground;
            lock.follow_ground += gap * (gap < 0.0f ? kFollowGroundDown : kFollowGroundUp);
        }
        center[0] = feet[0];
        center[1] = lock.follow_ground + kFollowEyeHeight;
        center[2] = feet[2];
        ex = std::sin(lock.follow_az) * lock.follow_dist;
        ey = 0.0f;
        ez = std::cos(lock.follow_az) * lock.follow_dist;
    } else if (lock.follow) {
        // No player position: the game's distance and height, at our direction.
        const float dh = std::sqrt(ex * ex + ez * ez);
        ex = std::sin(lock.follow_az) * dh;
        ez = std::cos(lock.follow_az) * dh;
    }
    float pull_xz = pull;
    if (lock.follow && lock.follow_dist > 1.0f) {
        // Never closer than kFollowMinDist across the ground (the stick's
        // distance otherwise put the eye right in Goemon's hair).
        pull_xz = std::max(pull, kFollowMinDist / lock.follow_dist);
    }
    pos[0] = center[0] + ex * pull_xz;
    pos[1] = center[1] + ey * pull;
    pos[2] = center[2] + ez * pull_xz;
    bool station_jump = false;
    if (lock.follow && in.outdoor_mode == 2 && player_position(rdram, feet)) {
        // A standing camera, like the rooms' (which never warp): the eye stays
        // put while Goemon walks; when he leaves its range it cuts to the spot
        // behind him the riding eye would have (pos), like the game's room cuts.
        // The view's direction never changes at a cut (only the right stick turns
        // it): the stick moves Goemon relative to the view, so a turned view made
        // him turn round and walk back into the last spot, over and over (PC test).
        int reason = lock.station_set ? 0 : 5;
        if (lock.station_set) {
            const float dx = feet[0] - lock.station_eye[0];
            const float dz = feet[2] - lock.station_eye[2];
            const float d = std::sqrt(dx * dx + dz * dz);
            const float base = wrap_angle(lock.follow_az + kPi);
            const float off = d > 1.0f ? std::fabs(wrap_angle(std::atan2(dx, dz) - base)) : 0.0f;
            const float rise = (lock.follow_ground + kFollowEyeHeight) - lock.station_eye[1];
            if (d > kStationFar) {
                reason = 1;
            } else if (d < kStationNear) {
                reason = 2;
            } else if (d > kStationSideMinDist && off > kStationSideDeg * (kPi / 180.0f)) {
                reason = 3;
            } else if (std::fabs(rise) > kStationRise) {
                reason = 4;
            }
        }
        if (reason != 0) {
            station_jump = lock.station_set;
            float spot[3] = {pos[0], pos[1], pos[2]};
            if (reason == 2) {
                // Walking back toward the camera: further back on the same line.
                const float back = std::clamp(lock.follow_dist, kStationBackMin, kStationBackMax);
                spot[0] = feet[0] + std::sin(lock.follow_az) * back;
                spot[2] = feet[2] + std::cos(lock.follow_az) * back;
            }
            lock.station_reason = reason;
            lock.station_eye[0] = spot[0];
            lock.station_eye[1] = spot[1];
            lock.station_eye[2] = spot[2];
            lock.station_set = true;
            ++lock.station_moves;
        }
        pos[0] = lock.station_eye[0];
        pos[1] = lock.station_eye[1];
        pos[2] = lock.station_eye[2];
    } else if (lock.follow && in.outdoor_mode == 1) {
        // Chase: the eye glides after its spot behind Goemon (the view's direction
        // stays where the head and the right stick put it).
        if (!lock.chase_set) {
            lock.chase_eye[0] = pos[0];
            lock.chase_eye[1] = pos[1];
            lock.chase_eye[2] = pos[2];
            lock.chase_set = true;
        } else {
            for (int i = 0; i < 3; ++i) {
                lock.chase_eye[i] += (pos[i] - lock.chase_eye[i]) * kChaseRate;
            }
        }
        pos[0] = lock.chase_eye[0];
        pos[1] = lock.chase_eye[1];
        pos[2] = lock.chase_eye[2];
    }
    // First person: the eye in Goemon's head.
    bool fp_switch = false;
    float fp_feet[3];
    const bool fp_now = in.first_person && player_position(rdram, fp_feet);
    if (fp_now != lock.fp_active) {
        fp_switch = true;
        ++lock.fp_toggles;
        float facing = 0.0f;
        const bool have_facing = player_heading(rdram, facing);
        if (fp_now) {
            // Look the way Goemon faces.
            lock.fp_heading = have_facing ? facing : wrap_angle(lock.follow_az + kPi);
        } else {
            // Back to the chase camera, straight behind the way he faces now.
            lock.follow_az = wrap_angle((have_facing ? facing : lock.fp_heading) + kPi);
            lock.chase_set = false;
            lock.station_set = false;
        }
        lock.fp_active = fp_now;
    }
    if (lock.fp_active) {
        lock.fp_heading = wrap_angle(lock.fp_heading + in.turn);
        const float height = in.fp_eye_height > 0.0f ? in.fp_eye_height : kFirstPersonEyeHeight;
        const float ahead = in.fp_forward >= 0.0f ? in.fp_forward : kFirstPersonForward;
        pos[0] = fp_feet[0] + std::sin(lock.fp_heading) * ahead;
        pos[1] = fp_feet[1] + height;
        pos[2] = fp_feet[2] + std::cos(lock.fp_heading) * ahead;
    }
    write_f32(rdram, cam + kCamPos, pos[0]);
    write_f32(rdram, cam + kCamPos + 4u, pos[1]);
    write_f32(rdram, cam + kCamPos + 8u, pos[2]);
    lock.game_at[0] = at[0];
    lock.game_at[1] = at[1];
    lock.game_at[2] = at[2];
    lock.eye_out[0] = pos[0];
    lock.eye_out[1] = pos[1];
    lock.eye_out[2] = pos[2];
    lock.far_plane = read_f32(rdram, cam + 0x20u);
    const float near_plane = read_f32(rdram, cam + 0x1Cu);
    const float vr_near = lock.fp_active ? (in.fp_near > 0.0f ? in.fp_near : kFirstPersonNear) : kVrNearPlane;
    if (std::isfinite(near_plane) && near_plane > vr_near) {
        write_f32(rdram, cam + 0x1Cu, vr_near);
        lock.game_near = near_plane;
    }
    // N64 fog follows z/w, which depends on the near plane: near 4 instead of the
    // game's 15-35 made the fog start 8 times closer. RT64 puts the game's back.
    // Only when this camera's own near was read (once written it reads 4, and the
    // last one seen may belong to another camera).
    out.game_near = std::isfinite(near_plane) && near_plane > kVrNearPlane ? near_plane : 0.0f;
    out.far_plane = std::isfinite(lock.far_plane) ? lock.far_plane : 0.0f;

    // The view: the game's heading, turned by the lock, the stick and the head
    // about the eye; the game's tilt replaced with the head's so level means
    // level. If the view points straight down the heading is undefined, so the
    // last good one is kept.
    // Base heading: the game camera's (turned by the lock), or in follow mode
    // straight at Goemon from our eye.
    const float base_heading = lock.fp_active ? lock.fp_heading
        : lock.follow ? wrap_angle(lock.follow_az + kPi) : wrap_angle(game_yaw + lock.absorbed + lock.turned);
    // The head's own movement moves the eye (6DOF, as in South Park). Without it the
    // world was drawn from a point that ignored the few centimeters the eyes move
    // when the head turns or leans; close up (outdoors, the camera 1-2 m from Goemon
    // and the walls) that made the near world swim and bend ("the world warps
    // outside"; rooms film from far away). Headset forward (-z) is the base heading;
    // right is forward x up = (-cos, 0, sin) in Goemon's atan2(x, z) headings.
    if (in.units_per_meter > 0.0f) {
        const float u = in.units_per_meter;
        const float right_x = -std::cos(base_heading);
        const float right_z = std::sin(base_heading);
        const float fwd_x = std::sin(base_heading);
        const float fwd_z = std::cos(base_heading);
        const float ox = in.head_offset[0] * u;
        const float oy = in.head_offset[1] * u;
        const float back = in.head_offset[2] * u;
        pos[0] += right_x * ox - fwd_x * back;
        pos[1] += oy;
        pos[2] += right_z * ox - fwd_z * back;
        write_f32(rdram, cam + kCamPos, pos[0]);
        write_f32(rdram, cam + kCamPos + 4u, pos[1]);
        write_f32(rdram, cam + kCamPos + 8u, pos[2]);
        lock.eye_out[0] = pos[0];
        lock.eye_out[1] = pos[1];
        lock.eye_out[2] = pos[2];
    }
    const float yaw = wrap_angle(base_heading - game_yaw + in.head_yaw);
    lock.base_heading = base_heading;
    lock.view_heading = wrap_angle(base_heading + in.head_yaw);
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    const float nx = c * fx0 + s * fz0;
    const float nz = -s * fx0 + c * fz0;
    static float flat_x = 0.0f;
    static float flat_z = -1.0f;
    const float hlen = std::sqrt(nx * nx + nz * nz);
    if (hlen > 0.05f * dist) {
        flat_x = nx / hlen;
        flat_z = nz / hlen;
    }
    const float vx = std::cos(in.pitch) * flat_x;
    const float vy = std::sin(in.pitch);
    const float vz = std::cos(in.pitch) * flat_z;
    const float aim_dist = std::max(dist * pull, 1.0f);
    write_f32(rdram, cam + kCamAt, pos[0] + vx * aim_dist);
    write_f32(rdram, cam + kCamAt + 4u, pos[1] + vy * aim_dist);
    write_f32(rdram, cam + kCamAt + 8u, pos[2] + vz * aim_dist);

    out.game_fovy_deg = read_u16(rdram, cam + kCamFovy) * 360.0f / 32768.0f;
    if (in.fovy_deg > 1.0f) {
        const float fovy = std::clamp(in.fovy_deg, 30.0f, 170.0f);
        write_u16(rdram, cam + kCamFovy, static_cast<uint16_t>(std::lround(fovy / 360.0f * 32768.0f)));
    }
    // Draw the whole frame, not the TV's bordered box (8,16)-(312,224).
    write_u16(rdram, cam + kCamScissor + 0u, 0);
    write_u16(rdram, cam + kCamScissor + 2u, 0);
    write_u16(rdram, cam + kCamScissor + 4u, 320);
    write_u16(rdram, cam + kCamScissor + 6u, 240);

    out.pitch = std::asin(std::clamp(vy, -1.0f, 1.0f));
    lock.view_pitch = out.pitch;
    out.move_yaw = wrap_angle(yaw);
    out.cut = cut || station_jump || fp_switch;
    out.first_person = lock.fp_active;
    if (lock.fp_active) {
        // Goemon faces where you look, so attacks and throws go there.
        const uint32_t obj = read_u32(rdram, kPlayerObjectVram);
        if (valid_vram(obj)) {
            const uint16_t old = read_u16(rdram, obj + kObjectRotY);
            const int units = static_cast<int>(std::lround(wrap_angle(lock.view_heading) * (1024.0f / (2.0f * kPi)))) & 0x3FF;
            write_u16(rdram, obj + kObjectRotY, static_cast<uint16_t>((old & ~0x3FFu) | static_cast<uint16_t>(units)));
        }
    }
    out.dl_phys = read_u32(rdram, kGfxHeadVram) & 0x1FFFFFFFu;
    out.proj_phys = read_u32(rdram, kMtxPoolVram) & 0x1FFFFFFFu;
    return true;
}

// ---- The 3D viewport ----------------------------------------------------------
//
// The game's level viewport (Camera::vp, copied into the display list by
// func_80017D8C) is scale 192.5 x 144.5, centre (160, 95.5): its 3D fills a box 1.2x
// the 320x240 screen, 24.5 rows above the middle (measured in RT64 on PC, the same
// with and without the VR camera). The headset's layer says the picture's rows and
// columns cover the projection's own view, so out goes a full-screen viewport
// (scale and centre 160, 120; the game's depth terms). Vp_t: s16 vscale[4], vtrans[4].
// Returns false (keep the game's) for a bad address. game[8] gets the game's values.
inline bool full_screen_viewport(uint8_t* rdram, uint32_t game_vp, uint32_t out_vp, int16_t game[8]) {
    if (!valid_vram(game_vp) || !valid_vram(out_vp) || (game_vp & 1u) != 0u || (out_vp & 1u) != 0u) {
        return false;
    }
    for (uint32_t i = 0; i < 8u; ++i) {
        game[i] = static_cast<int16_t>(read_u16(rdram, game_vp + i * 2u));
    }
    const uint16_t values[8] = {640, 480, static_cast<uint16_t>(game[2]), 0, 640, 480, static_cast<uint16_t>(game[6]), 0};
    for (uint32_t i = 0; i < 8u; ++i) {
        write_u16(rdram, out_vp + i * 2u, values[i]);
    }
    return true;
}

// ---- The sky as a panorama around the headset's view -------------------------
//
// The game's sky backdrop (patches/background.c) is a strip of a 360 degree
// panorama, 640 texels around (1.78 texels a degree: measured on PC, the scroll
// moves 1.78 texels per degree of camera turn and wraps at 640). The default
// mode shows it 1 texel per pixel (180 degrees across the TV), the panorama mode
// (flag bit 15) 2 pixels per texel (90 degrees). Neither matches the headset's
// view, and a flat strip can't follow a wide perspective anyway, so in VR the
// patch draws a grid: each column edge shows the panorama at the direction that
// screen column looks, each row edge the elevation that row looks at.
constexpr float kSkyTexelsPerDegree = 640.0f / 360.0f;
constexpr int kSkyColumns = 28;  // plus at most one seam edge, within the patch's 32
constexpr int kSkyRows = 24;

struct SkyView {
    float yaw_offset = 0.0f;    // the view's heading minus the game camera's (radians, +left)
    float pitch = 0.0f;         // the view's tilt (radians, +up)
    float game_pitch = 0.0f;    // the game camera's tilt (radians, +up)
    float tan_right = 1.0f;     // the picture's right edge, tan of the half view
    float tan_up = 1.0f;        // the picture's top edge
};

// Fills the patch's VrBackground (out_vram) for the node (node_vram). Layout:
// columns, rows, texture_width, texture_height (s32), then col_x[33], col_s[33],
// row_y[25], row_t[25] (f32).
inline bool fill_background(uint8_t* rdram, uint32_t node_vram, uint32_t out_vram, bool panorama, const SkyView& view) {
    if (!valid_vram(node_vram) || !valid_vram(out_vram)) {
        return false;
    }
    // BackgroundGraphicsNode: texture_width s16 0x10, texture_height s16 0x12,
    // start_x f32 0x14, start_y f32 0x18.
    const int tex_w = panorama ? 640 : static_cast<int16_t>(read_u16(rdram, node_vram + 0x10u));
    const int tex_h = panorama ? 240 : static_cast<int16_t>(read_u16(rdram, node_vram + 0x12u));
    const float start_x = read_f32(rdram, node_vram + 0x14u);
    const float start_y = read_f32(rdram, node_vram + 0x18u);
    if (tex_w < 64 || tex_h < 8 || !std::isfinite(start_x) || !std::isfinite(start_y)) {
        return false;
    }
    // The texel at the middle of the TV picture, where the game camera looks.
    float center_s;
    float center_t;
    if (panorama) {
        float sx = std::fmod(start_x, 1280.0f);
        if (sx < 0.0f) {
            sx += 1280.0f;
        }
        center_s = sx * 0.5f + 80.0f;
        center_t = std::clamp(start_y, 0.0f, 240.0f) * 0.5f + 60.0f;
    } else {
        center_s = start_x + 160.0f;
        center_t = start_y + 120.0f;
    }
    // Turned from the game camera by the head (and the lock): turning left
    // shows texels to the left (smaller s), looking up shows rows above.
    center_s -= kSkyTexelsPerDegree * view.yaw_offset * (180.0f / kPi);
    center_t -= kSkyTexelsPerDegree * (view.pitch - view.game_pitch) * (180.0f / kPi);

    // Columns over the 320-pixel layout (RT64 spreads a full row of rects over
    // the whole picture), with an extra edge wherever the panorama's seam falls.
    const float fx = 160.0f / std::max(view.tan_right, 0.05f);
    const float fy = 120.0f / std::max(view.tan_up, 0.05f);
    float col_x[kSkyColumns + 8];
    float col_s[kSkyColumns + 8];
    int columns = 0;
    auto texel_at_x = [&](float x) {
        return center_s + kSkyTexelsPerDegree * std::atan((x - 160.0f) / fx) * (180.0f / kPi);
    };
    float prev_x = 0.0f;
    float prev_s = texel_at_x(0.0f);
    col_x[0] = prev_x;
    col_s[0] = prev_s;
    for (int i = 1; i <= kSkyColumns && columns < kSkyColumns + 6; ++i) {
        const float x = 320.0f * static_cast<float>(i) / kSkyColumns;
        const float s = texel_at_x(x);
        // A seam (multiple of the texture width) between prev_s and s: split there.
        const float seam = std::floor(s / tex_w) * tex_w;
        if (seam > prev_s + 0.01f && seam < s - 0.01f && columns < kSkyColumns + 6) {
            // Find the x of the seam by bisection on the monotonic mapping.
            float lo = prev_x;
            float hi = x;
            for (int k = 0; k < 20; ++k) {
                const float mid = 0.5f * (lo + hi);
                (texel_at_x(mid) < seam ? lo : hi) = mid;
            }
            ++columns;
            col_x[columns] = hi;
            col_s[columns] = seam;
        }
        ++columns;
        col_x[columns] = x;
        col_s[columns] = s;
        prev_x = x;
        prev_s = s;
    }
    if (columns > 32) {
        columns = 32;  // the patch's table holds 32 columns
    }

    // Rows over the 240-pixel layout.
    float row_y[kSkyRows + 1];
    float row_t[kSkyRows + 1];
    for (int j = 0; j <= kSkyRows; ++j) {
        const float y = 240.0f * static_cast<float>(j) / kSkyRows;
        const float t = center_t - kSkyTexelsPerDegree * std::atan((120.0f - y) / fy) * (180.0f / kPi);
        row_y[j] = y;
        row_t[j] = std::clamp(t, 0.0f, static_cast<float>(tex_h - 1));
    }

    // Write the table (s32/f32 words, native word order like the rest of RDRAM).
    auto put_s32 = [&](uint32_t off, int32_t v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        std::memcpy(ram(rdram, out_vram + off), &bits, 4);
    };
    put_s32(0x0u, columns);
    put_s32(0x4u, kSkyRows);
    put_s32(0x8u, tex_w);
    put_s32(0xCu, tex_h);
    constexpr uint32_t kColX = 0x10u;
    constexpr uint32_t kColS = kColX + 4u * 33u;
    constexpr uint32_t kRowY = kColS + 4u * 33u;
    constexpr uint32_t kRowT = kRowY + 4u * 25u;
    for (int i = 0; i <= columns; ++i) {
        write_f32(rdram, out_vram + kColX + 4u * i, col_x[i]);
        write_f32(rdram, out_vram + kColS + 4u * i, col_s[i]);
    }
    for (int j = 0; j <= kSkyRows; ++j) {
        write_f32(rdram, out_vram + kRowY + 4u * j, row_y[j]);
        write_f32(rdram, out_vram + kRowT + 4u * j, row_t[j]);
    }
    return true;
}

// ---- The sky as a dome around the camera ----------------------------------------
//
// The grid above is flat 2D, drawn for one head pose. RT64's in-between pictures
// (60 a second from the game's 30) move the 3D world halfway to the next frame,
// but 2D stays where the next frame has it, so outdoors the sky jumped against
// the world twice a frame whenever the head turned ("the world still warps").
// The dome draws the same panorama as textured triangles on a big sphere around
// the eye instead: part of the 3D scene, so every picture shows it where it
// belongs. The panorama is fixed in the world: texel column s looks along
// heading (c - s) / kSkyTexelsPerDegree and texel row t at elevation
// (t0 - t) / kSkyTexelsPerDegree, both measured from the game's own scroll.
constexpr int kSkyDomeCellTexels = 32;     // a cell's texels each way (fits TMEM with a 1-texel margin)
constexpr int kSkyDomeMaxCells = 320;      // the patch's table size
constexpr float kSkyDomeUnit = 1000.0f;    // vertex radius in model units (the matrix scales it)
constexpr float kSkyDomeHold = 12.0f;      // texels the measured scroll may wobble before the dome follows
                                           // (Quest: up to 4 while turning; a new sky jumps far more)
// The game scrolls the sky up and down only 0.38 texels per degree its camera
// tilts (start_y falls as the camera looks up; PC trace of a level's opening
// camera move, tilt 14 to -1 degrees), not the 1.78 a degree of the panorama
// itself. So start_y taken back to a level camera gives the row at the horizon,
// the same for any tilt.
constexpr float kSkyGameTiltRate = 0.38f;
// The patch's VrSky table: cells, texture_width, texture_height, pad (s32), the
// modelview matrix f32[4][4] at 0x10, then per cell at 0x50 + 96 * i: the texel
// rectangle to load (uls, ult, lrs, lrt) and four corners (x, y, z, s, t), s32,
// s and t in 1/32 texels.
constexpr uint32_t kSkyDomeCellBase = 0x50u;
constexpr uint32_t kSkyDomeCellSize = 96u;

struct SkyDomeState {
    bool have = false;
    uint32_t node = 0;
    int tex_w = 0;
    int tex_h = 0;
    bool panorama = false;
    float c = 0.0f;             // the texel column looking along heading 0
    float t0 = 0.0f;            // the texel row at elevation 0
    uint32_t moves = 0;         // times the held scroll was moved (for logs)
};

inline float wrap_texels(float v, float period) {
    float r = std::fmod(v, period);
    return r < 0.0f ? r + period : r;
}

inline bool fill_sky_dome(uint8_t* rdram, uint32_t node_vram, uint32_t out_vram, bool panorama, const LockState& lock, SkyDomeState& st) {
    if (!valid_vram(node_vram) || !valid_vram(out_vram) || !lock.have_last) {
        return false;
    }
    const int tex_w = panorama ? 640 : static_cast<int16_t>(read_u16(rdram, node_vram + 0x10u));
    const int tex_h = panorama ? 240 : static_cast<int16_t>(read_u16(rdram, node_vram + 0x12u));
    const float start_x = read_f32(rdram, node_vram + 0x14u);
    const float start_y = read_f32(rdram, node_vram + 0x18u);
    if (tex_w < 64 || tex_h < 8 || tex_h > 1024 || !std::isfinite(start_x) || !std::isfinite(start_y)) {
        return false;
    }
    // The texel column at the middle of the TV, where the game camera looked (see
    // fill_background), and the row there for a level camera (the horizon).
    const float deg = 180.0f / kPi;
    const float level_y = start_y + kSkyGameTiltRate * lock.sky_game_pitch * deg;
    float center_s;
    float t0;
    if (panorama) {
        center_s = wrap_texels(start_x, 1280.0f) * 0.5f + 80.0f;
        t0 = std::clamp(level_y, 0.0f, 240.0f) * 0.5f + 60.0f;
    } else {
        center_s = start_x + 160.0f;
        t0 = level_y + 120.0f;
    }
    const float c = wrap_texels(center_s + kSkyTexelsPerDegree * lock.sky_game_yaw * deg, 640.0f);
    // Hold the scroll still unless it really moved: the game steps it in 0.625
    // texel units, and every wobble would shake the sky.
    const bool same = st.have && st.node == node_vram && st.tex_w == tex_w && st.tex_h == tex_h && st.panorama == panorama;
    const float dc = std::remainder(c - st.c, 640.0f);
    if (!same || std::fabs(dc) > kSkyDomeHold || std::fabs(t0 - st.t0) > kSkyDomeHold) {
        if (same) {
            ++st.moves;
        }
        st.have = true;
        st.node = node_vram;
        st.tex_w = tex_w;
        st.tex_h = tex_h;
        st.panorama = panorama;
        st.c = c;
        st.t0 = t0;
    }

    // Columns: every kSkyDomeCellTexels around the 640 texels of a full turn,
    // split where the texture repeats (a column never crosses the texture's edge).
    float col_u[64];
    int cols = 0;
    col_u[cols++] = 0.0f;
    for (int u = 0; u < 640 && cols < 64; ) {
        const int next = std::min(std::min((u / kSkyDomeCellTexels + 1) * kSkyDomeCellTexels, (u / tex_w + 1) * tex_w), 640);
        col_u[cols++] = static_cast<float>(next);
        u = next;
    }
    // Rows: the texture's rows every kSkyDomeCellTexels between the elevations
    // +/-89, plus a cap above and below it that repeats the edge row.
    const float t_zenith = st.t0 - kSkyTexelsPerDegree * 89.0f;
    const float t_nadir = st.t0 + kSkyTexelsPerDegree * 89.0f;
    float row_t[48];        // texel row of each edge (clamped into the texture)
    float row_el[48];       // elevation of each edge (degrees)
    int rows = 0;
    auto add_row = [&](float t, float el) {
        if (rows < 47) {
            row_t[rows] = t;
            row_el[rows] = el;
            ++rows;
        }
    };
    const float t_first = std::max(0.0f, t_zenith);
    const float t_last = std::min(static_cast<float>(tex_h), t_nadir);
    if (t_last - t_first < 1.0f) {
        return false;
    }
    if (t_zenith < 0.0f) {
        add_row(0.0f, 89.0f);   // top cap
    }
    for (float t = t_first; ; ) {
        add_row(t, (st.t0 - t) / kSkyTexelsPerDegree);
        if (t >= t_last) {
            break;
        }
        const float next = (std::floor(t / kSkyDomeCellTexels) + 1.0f) * kSkyDomeCellTexels;
        t = std::min(next, t_last);
    }
    if (t_nadir > static_cast<float>(tex_h)) {
        add_row(static_cast<float>(tex_h), -89.0f);   // bottom cap
    }

    auto put_s32 = [&](uint32_t off, int32_t v) {
        std::memcpy(ram(rdram, out_vram + off), &v, 4);
    };
    int cells = 0;
    for (int r = 0; r + 1 < rows; ++r) {
        const float ta = row_t[r];
        const float tb = row_t[r + 1];
        const float ea = row_el[r] * (kPi / 180.0f);
        const float eb = row_el[r + 1] * (kPi / 180.0f);
        if (std::fabs(ea - eb) < 1.0e-4f) {
            continue;
        }
        // Texels to load: the cell's rows plus one each side (bilinear filtering), in the texture.
        const int ult = std::clamp(static_cast<int>(std::floor(std::min(ta, tb))) - 1, 0, tex_h - 1);
        const int lrt = std::clamp(static_cast<int>(std::ceil(std::max(ta, tb))) + 1, 0, tex_h - 1);
        const float ta_tex = std::clamp(ta, 0.0f, static_cast<float>(tex_h - 1));
        const float tb_tex = std::clamp(tb, 0.0f, static_cast<float>(tex_h - 1));
        for (int k = 0; k + 1 < cols && cells < kSkyDomeMaxCells; ++k) {
            const float u0 = col_u[k];
            const float u1 = col_u[k + 1];
            const float s0 = wrap_texels(u0, static_cast<float>(tex_w));
            const float s1 = s0 + (u1 - u0);
            const int uls = std::clamp(static_cast<int>(std::floor(s0)) - 1, 0, tex_w - 1);
            const int lrs = std::clamp(static_cast<int>(std::ceil(s1)) + 1, 0, tex_w - 1);
            // Headings of the column's edges: texel u looks along (c - u) / rate.
            const float h0 = (st.c - u0) / kSkyTexelsPerDegree * (kPi / 180.0f);
            const float h1 = (st.c - u1) / kSkyTexelsPerDegree * (kPi / 180.0f);
            const uint32_t base = kSkyDomeCellBase + kSkyDomeCellSize * static_cast<uint32_t>(cells);
            put_s32(base + 0u, uls);
            put_s32(base + 4u, ult);
            put_s32(base + 8u, lrs);
            put_s32(base + 12u, lrt);
            const float hs[4] = {h0, h1, h1, h0};
            const float es[4] = {ea, ea, eb, eb};
            const float ss[4] = {s0, s1, s1, s0};
            const float ts[4] = {ta_tex, ta_tex, tb_tex, tb_tex};
            for (int v = 0; v < 4; ++v) {
                const float ce = std::cos(es[v]);
                const uint32_t vo = base + 16u + 20u * static_cast<uint32_t>(v);
                put_s32(vo + 0u, static_cast<int32_t>(std::lround(std::sin(hs[v]) * ce * kSkyDomeUnit)));
                put_s32(vo + 4u, static_cast<int32_t>(std::lround(std::sin(es[v]) * kSkyDomeUnit)));
                put_s32(vo + 8u, static_cast<int32_t>(std::lround(std::cos(hs[v]) * ce * kSkyDomeUnit)));
                put_s32(vo + 12u, static_cast<int32_t>(std::lround(ss[v] * 32.0f)));
                put_s32(vo + 16u, static_cast<int32_t>(std::lround(ts[v] * 32.0f)));
            }
            ++cells;
        }
    }
    if (cells == 0) {
        return false;
    }
    // Model to world: scaled out to half the far distance, centered on the eye
    // the view is drawn from (N64 row-vector order: translation in the last row).
    const float far_dist = std::isfinite(lock.far_plane) && lock.far_plane > 10.0f ? lock.far_plane : 10000.0f;
    const float scale = std::clamp(0.5f * far_dist, 200.0f, 60000.0f) / kSkyDomeUnit;
    float m[16] = {scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0, lock.eye_out[0], lock.eye_out[1], lock.eye_out[2], 1.0f};
    for (int i = 0; i < 16; ++i) {
        write_f32(rdram, out_vram + 0x10u + 4u * static_cast<uint32_t>(i), m[i]);
    }
    put_s32(0x0u, cells);
    put_s32(0x4u, tex_w);
    put_s32(0x8u, tex_h);
    put_s32(0xCu, 0);
    return true;
}

// ---- Sprites that face the camera ----------------------------------------------
//
// func_8001B6D4 (called right after the camera's matrices) keeps eye - look-at of
// the camera at 0x80168980 and works angles out of it; Goemon's trees, bushes and
// other sprites turn to face that direction (PC: a tree stays parallel to the picture
// even looking 35 degrees down). Fed the head-aimed copy, every sprite swung with the
// head. This turns a copy of the aimed camera, level, to a heading that follows
// the view only slowly (rate = share of the gap per frame: 0 keeps the base
// heading, 1 follows the head at once like before), so a head turn doesn't swing
// the sprites while one held for a while still gets them facing you.
inline bool billboard_camera(uint8_t* rdram, uint32_t cam, LockState& lock, float rate) {
    if (!valid_vram(cam) || !lock.have_last) {
        return false;
    }
    if (!lock.have_sprite) {
        lock.sprite_heading = lock.view_heading;
        lock.have_sprite = true;
    }
    const float target = rate <= 0.0f ? lock.base_heading : lock.view_heading;
    const float step = rate <= 0.0f ? 1.0f : std::min(rate, 1.0f);
    lock.sprite_heading = wrap_angle(lock.sprite_heading + wrap_angle(target - lock.sprite_heading) * step);
    const float pos[3] = {read_f32(rdram, cam + kCamPos), read_f32(rdram, cam + kCamPos + 4u), read_f32(rdram, cam + kCamPos + 8u)};
    const float at[3] = {read_f32(rdram, cam + kCamAt), read_f32(rdram, cam + kCamAt + 4u), read_f32(rdram, cam + kCamAt + 8u)};
    const float dx = at[0] - pos[0];
    const float dy = at[1] - pos[1];
    const float dz = at[2] - pos[2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(len) || len < 1.0f) {
        return false;
    }
    write_f32(rdram, cam + kCamAt, pos[0] + std::sin(lock.sprite_heading) * len);
    write_f32(rdram, cam + kCamAt + 4u, pos[1]);
    write_f32(rdram, cam + kCamAt + 8u, pos[2] + std::cos(lock.sprite_heading) * len);
    return true;
}

} // namespace goemon_vr
