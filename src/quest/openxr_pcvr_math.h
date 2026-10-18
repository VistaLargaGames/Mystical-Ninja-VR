#pragma once

#include <cmath>
#include <cstring>

// guLookAtF memory layout (column-major float[16], translation in m[12..14]).
// m[0], m[4], m[8]  = right
// m[1], m[5], m[9]  = up
// m[2], m[6], m[10] = look (+Z, behind the camera; forward is -look)
// m[12], m[13], m[14] = -dot(eye, right/up/look)

inline void vr_look_at(float ex, float ey, float ez, float ax, float ay, float az, float ux, float uy, float uz, float mf[16]) {
    float xLook = ax - ex;
    float yLook = ay - ey;
    float zLook = az - ez;
    float len = -1.0f / std::sqrt(xLook * xLook + yLook * yLook + zLook * zLook);
    xLook *= len;
    yLook *= len;
    zLook *= len;

    float xRight = uy * zLook - uz * yLook;
    float yRight = uz * xLook - ux * zLook;
    float zRight = ux * yLook - uy * xLook;
    len = 1.0f / std::sqrt(xRight * xRight + yRight * yRight + zRight * zRight);
    xRight *= len;
    yRight *= len;
    zRight *= len;

    ux = yLook * zRight - zLook * yRight;
    uy = zLook * xRight - xLook * zRight;
    uz = xLook * yRight - yLook * xRight;
    len = 1.0f / std::sqrt(ux * ux + uy * uy + uz * uz);
    ux *= len;
    uy *= len;
    uz *= len;

    std::memset(mf, 0, sizeof(float) * 16);
    mf[0] = xRight;
    mf[1] = ux;
    mf[2] = xLook;
    mf[4] = yRight;
    mf[5] = uy;
    mf[6] = yLook;
    mf[8] = zRight;
    mf[9] = uz;
    mf[10] = zLook;
    mf[12] = -(ex * xRight + ey * yRight + ez * zRight);
    mf[13] = -(ex * ux + ey * uy + ez * uz);
    mf[14] = -(ex * xLook + ey * yLook + ez * zLook);
    mf[15] = 1.0f;
}

// Same cofactor invert the patch uses on the flat 16-float lookat.
inline bool vr_invert_matrix(const float m[16], float invOut[16]) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) {
        return false;
    }
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) {
        invOut[i] = inv[i] * det;
    }
    return true;
}

inline void vr_basis_from_lookat(const float m[16], float eye[3], float right[3], float up[3], float look[3]) {
    right[0] = m[0];
    right[1] = m[4];
    right[2] = m[8];
    up[0] = m[1];
    up[1] = m[5];
    up[2] = m[9];
    look[0] = m[2];
    look[1] = m[6];
    look[2] = m[10];
    eye[0] = -m[12] * right[0] - m[13] * up[0] - m[14] * look[0];
    eye[1] = -m[12] * right[1] - m[13] * up[1] - m[14] * look[1];
    eye[2] = -m[12] * right[2] - m[13] * up[2] - m[14] * look[2];
}

// Parallel shift: move eye and look-at by the same world offset so orientation stays put.
inline bool vr_shift_lookat(const float lookat[16], float dx, float dy, float dz, float shifted_inv[16]) {
    float eye[3], right[3], up[3], look[3];
    vr_basis_from_lookat(lookat, eye, right, up, look);
    float nex = eye[0] + dx;
    float ney = eye[1] + dy;
    float nez = eye[2] + dz;
    // look points behind the camera, so the look-at point is eye - look.
    float shifted[16];
    vr_look_at(nex, ney, nez, nex - look[0], ney - look[1], nez - look[2], up[0], up[1], up[2], shifted);
    return vr_invert_matrix(shifted, shifted_inv);
}

// OpenXR Y-up quaternion yaw. Positive yaw is a left turn. Identity (looking down -Z) is 0.
inline float vr_yaw_from_quat(float x, float y, float z, float w) {
    return std::atan2(2.0f * (w * y + x * z), 1.0f - 2.0f * (y * y + z * z));
}

// Positive pitch looks down (positive rotation about +X). Roll is ignored.
inline float vr_pitch_from_quat(float x, float y, float z, float w) {
    float sinp = 2.0f * (w * x - y * z);
    if (sinp > 1.0f) {
        sinp = 1.0f;
    } else if (sinp < -1.0f) {
        sinp = -1.0f;
    }
    return std::asin(sinp);
}
