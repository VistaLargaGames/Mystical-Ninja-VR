#ifndef __PATCH_GRAPHICS_H__
#define __PATCH_GRAPHICS_H__

#include "patch_helpers.h"

DECLARE_FUNC(void, recomp_get_window_resolution, u32*, u32*);
DECLARE_FUNC(float, recomp_get_target_aspect_ratio, float);
DECLARE_FUNC(s32, recomp_get_target_framerate, s32);
DECLARE_FUNC(s32, recomp_high_precision_fb_enabled);
DECLARE_FUNC(float, recomp_get_resolution_scale);
// Quest VR: aims a copy of the camera where the headset looks (wider view, full scissor).
// Returns 1 when it changed the copy. Always 0 on PC unless a desk test asks for it.
DECLARE_FUNC(s32, recomp_vr_aim_camera, void*);
DECLARE_FUNC(s32, recomp_vr_billboard, void*);
DECLARE_FUNC(s32, recomp_vr_viewport, void*, void*);

#endif
