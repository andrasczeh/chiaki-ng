// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Synthetic (pseudo) stereo 3D for flat game streams.
//
// The depth estimation is a port of the synthetic stereo in P5M
// (https://github.com/beecrepaldi-afk/P5M, AGPL-3.0, app/src/main/cpp/tone_mapper.cpp),
// which itself builds on chiaki-ng. The idea of a cheap side-by-side / anaglyph
// pseudo-3D mode for Chiaki comes from pearce31's Chiaki 3D mods
// (https://github.com/pearce31/Chiaki-Pseudo-3D-and-Android-Phone-VR, GPL-3.0-or-later).
//
// Implemented as a libplacebo custom hook on PL_HOOK_RGB, so it works with both the
// Vulkan and OpenGL renderer backends:
//   1. depth pass  - quarter-resolution depth map from scene cues (ground ramp,
//                    detail, saturation), HUD/text held at screen depth, and an
//                    adaptive temporal average with the previous frame.
//   2. stereo pass - warps the image by half the disparity per eye and packs both
//                    eyes as half side-by-side (or a green/magenta anaglyph).

#ifndef CHIAKI_GUI_STEREO3D_H
#define CHIAKI_GUI_STEREO3D_H

#include <cstdint>

extern "C" {
#include <libplacebo/gpu.h>
#include <libplacebo/shaders/custom.h>
}

enum class Stereo3DMode : int {
    Off = 0,
    HalfSideBySide = 1,
    Anaglyph = 2, // green (left) / magenta (right)
};

class Stereo3D
{
public:
    Stereo3D();
    ~Stereo3D();
    Stereo3D(const Stereo3D &) = delete;
    Stereo3D &operator=(const Stereo3D &) = delete;

    // strength / convergence in 0..100 (settings units)
    void setParams(Stereo3DMode mode, int strength, int convergence, bool swap_eyes);
    Stereo3DMode mode() const { return mode_; }

    // Identifies the source frame about to be rendered. libplacebo may call the hook
    // several times for the same frame (re-presents, resizes); the depth history only
    // advances when this changes.
    void setFrameSerial(uint64_t serial) { serial_ = serial; }

    // nullptr when 3D is off
    const struct pl_hook *hook() const { return mode_ == Stereo3DMode::Off ? nullptr : &hook_; }

    // Drop the depth history (stream restart, scene reset)
    void reset();

    // Must be called before the GPU is destroyed
    void releaseGpu(pl_gpu gpu);

private:
    static struct pl_hook_res hookCb(void *priv, const struct pl_hook_params *params);
    struct pl_hook_res run(const struct pl_hook_params *params);
    bool ensureDepthTextures(pl_gpu gpu, int w, int h);
    bool depthPass(const struct pl_hook_params *params, pl_tex prev, pl_tex target,
                   float norm, const float src_off[2], const float src_scale[2], const float texel[2]);
    bool stereoPass(const struct pl_hook_params *params, pl_tex depth, pl_tex target,
                    const float src_off[2], const float src_scale[2]);

    struct pl_hook hook_ = {};
    Stereo3DMode mode_ = Stereo3DMode::Off;
    float disparity_ = 0.01f;   // fraction of the eye image width
    float convergence_ = 0.35f; // depth value that stays at screen distance
    bool swap_eyes_ = false;

    pl_gpu gpu_ = nullptr;
    pl_tex depth_[2] = {nullptr, nullptr};
    int depth_w_ = 0;
    int depth_h_ = 0;
    int cur_ = 0;
    bool has_cur_ = false;
    uint64_t serial_ = 0;
    uint64_t last_serial_ = 0;
    bool logged_failure_ = false;
};

#endif // CHIAKI_GUI_STEREO3D_H
