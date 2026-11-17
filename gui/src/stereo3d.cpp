// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// See stereo3d.h for credits. The depth heuristics below are ported from P5M's
// kDepthShader (AGPL-3.0); comments are translated/condensed from the original.

#include "stereo3d.h"

#include <QLoggingCategory>
#include <algorithm>
#include <cmath>

extern "C" {
#include <libplacebo/colorspace.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/shaders.h>
}

Q_DECLARE_LOGGING_CATEGORY(chiakiGui);

namespace {

// P5M clamps the synthesized disparity to half of an IPD-derived ceiling of 2% of the
// image width. Strength 100 maps to that ceiling, the default (50) to P5M's default.
constexpr float kMaxDisparity = 0.02f;

// Depth map at 1/4 resolution: cheaper, and P5M found that a blurry depth map
// gives fewer halos around edges than a sharp one.
constexpr int kDepthDivisor = 4;

const char *kDepthHeader = R"(
vec4 st3d_depth_main(vec2 p)
{
    vec2 uv = st3d_src_off + p * st3d_src_scale;
    vec2 dx = vec2(st3d_texel.x, 0.0);
    vec2 dy = vec2(0.0, st3d_texel.y);

    vec3 c = st3d_norm * textureLod(st3d_src, uv, 0.0).rgb;

    // Wide samples carry the scene cues (texture vs haze, vivid vs washed-out
    // colour). Spread on purpose: a hard-edged depth map halos everything.
    vec3 n = st3d_norm * textureLod(st3d_src, uv - dy * 8.0, 0.0).rgb;
    vec3 s = st3d_norm * textureLod(st3d_src, uv + dy * 8.0, 0.0).rgb;
    vec3 w = st3d_norm * textureLod(st3d_src, uv - dx * 8.0, 0.0).rgb;
    vec3 e = st3d_norm * textureLod(st3d_src, uv + dx * 8.0, 0.0).rgb;

    const vec3 avg = vec3(0.3333);
    float detail = clamp((abs(dot(n - c, avg)) + abs(dot(s - c, avg))
            + abs(dot(w - c, avg)) + abs(dot(e - c, avg))) * 2.0, 0.0, 1.0);

    float mx = max(max(c.r, c.g), c.b);
    float mn = min(min(c.r, c.g), c.b);
    float sat = (mx - mn) / max(mx, 1e-4);

    // Ground ramp: what is at the bottom of the screen is near (p.y = 1 at the bottom).
    float ground = clamp(p.y, 0.0, 1.0);

    // The ground ramp only gets full authority where there is a vertical gradient
    // (a receding floor). On walls, close-ups and full-screen menus it backs off,
    // otherwise the whole picture tilts backwards for no reason.
    float grad_v = abs(dot(s - n, avg));
    float w_ground = mix(0.25, 0.60, smoothstep(0.010, 0.075, grad_v));
    float rest = 1.0 - w_ground;
    float d = clamp(w_ground * ground + rest * (0.375 * detail + 0.625 * sat), 0.0, 1.0);

    // One-texel contrast separates text (hard pixel edges) from texture.
    vec3 fn = st3d_norm * textureLod(st3d_src, uv - dy, 0.0).rgb;
    vec3 fs = st3d_norm * textureLod(st3d_src, uv + dy, 0.0).rgb;
    vec3 fw = st3d_norm * textureLod(st3d_src, uv - dx, 0.0).rgb;
    vec3 fe = st3d_norm * textureLod(st3d_src, uv + dx, 0.0).rgb;
    float fine = (abs(dot(fn - c, avg)) + abs(dot(fs - c, avg))
            + abs(dot(fw - c, avg)) + abs(dot(fe - c, avg))) * 0.25;

    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    float still = 1.0;
    float change = 0.0;
    vec4 prev = vec4(0.0);
    if (st3d_has_prev > 0.5) {
        prev = textureLod(st3d_prev, p, 0.0);
        change = abs(luma - prev.g);
        // In a game the world always moves a little; what stays exactly the same
        // from frame to frame is UI drawn on top.
        still = 1.0 - smoothstep(0.004, 0.03, change);
    }

    // UI = hard edge AND motionless (either alone also matches fine texture or sky).
    // Keep it at screen depth so subtitles and HUDs don't float.
    float ui = smoothstep(0.05, 0.16, fine) * still;
    d = mix(d, st3d_conv, ui * 0.9);

    if (st3d_has_prev > 0.5) {
        // Exponential average with the previous frame against depth flicker.
        // Strong when the scene is calm, loosens on cuts so the old framing
        // isn't dragged along.
        float alpha = mix(0.12, 0.85, smoothstep(0.02, 0.20, change));
        d = mix(prev.r, d, alpha);
    }

    return vec4(clamp(d, 0.0, 1.0), luma, 0.0, 1.0);
}
)";

const char *kDepthBody = "color = st3d_depth_main(st3d_pos);\n";

const char *kStereoHeader = R"(
vec3 st3d_eye(vec2 q, float eye)
{
    float d = textureLod(st3d_depth, q, 0.0).r;
    // Half the disparity per eye, so the picture doesn't slide sideways when
    // the effect is turned on and the warp error is shared between both eyes.
    q.x = clamp(q.x + eye * 0.5 * st3d_disp * (d - st3d_conv), 0.0, 1.0);
    return textureLod(st3d_src, st3d_src_off + q * st3d_src_scale, 0.0).rgb;
}
)";

// Near content (d > conv) gets crossed disparity: the left eye samples to the left,
// shifting content right, and vice versa, so it appears in front of the screen.
const char *kSbsBody = R"(
{
    vec2 p = st3d_pos;
    bool left = p.x < 0.5;
    vec2 q = vec2(left ? p.x * 2.0 : (p.x - 0.5) * 2.0, p.y);
    float eye = (left ? -1.0 : 1.0) * st3d_swap;
    color = vec4(st3d_eye(q, eye), 1.0);
}
)";

const char *kAnaglyphBody = R"(
{
    vec3 l = st3d_eye(st3d_pos, -1.0 * st3d_swap);
    vec3 r = st3d_eye(st3d_pos, 1.0 * st3d_swap);
    color = vec4(r.r, l.g, r.b, 1.0);
}
)";

struct pl_shader_va pos_attrib(pl_gpu gpu, float (&verts)[4][2])
{
    struct pl_shader_va va = {};
    va.attr.name = "st3d_pos";
    va.attr.fmt = pl_find_vertex_fmt(gpu, PL_FMT_FLOAT, 2);
    verts[0][0] = 0.0f; verts[0][1] = 0.0f;
    verts[1][0] = 1.0f; verts[1][1] = 0.0f;
    verts[2][0] = 0.0f; verts[2][1] = 1.0f;
    verts[3][0] = 1.0f; verts[3][1] = 1.0f;
    for (int i = 0; i < 4; i++)
        va.data[i] = verts[i];
    return va;
}

struct pl_shader_desc tex_desc(const char *name, pl_tex tex)
{
    struct pl_shader_desc sd = {};
    sd.desc.name = name;
    sd.desc.type = PL_DESC_SAMPLED_TEX;
    sd.binding.object = tex;
    sd.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
    sd.binding.sample_mode = (tex->params.format->caps & PL_FMT_CAP_LINEAR)
        ? PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;
    return sd;
}

struct pl_shader_var float_var(const char *name, const float *data)
{
    struct pl_shader_var sv = {};
    sv.var = pl_var_float(name);
    sv.data = data;
    sv.dynamic = true;
    return sv;
}

struct pl_shader_var vec2_var(const char *name, const float *data)
{
    struct pl_shader_var sv = {};
    sv.var = pl_var_vec2(name);
    sv.data = data;
    sv.dynamic = true;
    return sv;
}

} // namespace

Stereo3D::Stereo3D()
{
    hook_.stages = PL_HOOK_RGB;
    hook_.input = PL_HOOK_SIG_TEX;
    hook_.priv = this;
    hook_.hook = &Stereo3D::hookCb;
    hook_.signature = 0x6368696b69334430ull; // "chiki3D0"
}

Stereo3D::~Stereo3D()
{
    // Textures must already have been released via releaseGpu()
}

void Stereo3D::setParams(Stereo3DMode mode, int strength, int convergence, bool swap_eyes)
{
    if (mode != mode_ && mode_ == Stereo3DMode::Off)
        has_cur_ = false; // stale history from a previous session
    mode_ = mode;
    disparity_ = kMaxDisparity * std::clamp(strength, 0, 100) / 100.0f;
    convergence_ = std::clamp(convergence, 0, 100) / 100.0f;
    swap_eyes_ = swap_eyes;
}

void Stereo3D::reset()
{
    has_cur_ = false;
}

void Stereo3D::releaseGpu(pl_gpu gpu)
{
    if (!gpu)
        gpu = gpu_;
    if (gpu) {
        pl_tex_destroy(gpu, &depth_[0]);
        pl_tex_destroy(gpu, &depth_[1]);
    }
    depth_[0] = depth_[1] = nullptr;
    depth_w_ = depth_h_ = 0;
    has_cur_ = false;
    gpu_ = nullptr;
}

struct pl_hook_res Stereo3D::hookCb(void *priv, const struct pl_hook_params *params)
{
    return static_cast<Stereo3D *>(priv)->run(params);
}

bool Stereo3D::ensureDepthTextures(pl_gpu gpu, int w, int h)
{
    if (gpu_ && gpu_ != gpu)
        releaseGpu(gpu_);
    gpu_ = gpu;
    if (depth_[0] && depth_[1] && depth_w_ == w && depth_h_ == h)
        return true;

    const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(
        PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_LINEAR);
    pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 2, 16, 0, caps);
    if (!fmt)
        fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 2, 16, 0, caps);
    if (!fmt)
        fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 2, 8, 0, caps);
    if (!fmt)
        fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 0, caps);
    if (!fmt)
        return false;

    struct pl_tex_params tp = {};
    tp.w = w;
    tp.h = h;
    tp.format = fmt;
    tp.sampleable = true;
    tp.renderable = true;
    for (int i = 0; i < 2; i++) {
        if (!pl_tex_recreate(gpu, &depth_[i], &tp))
            return false;
    }
    depth_w_ = w;
    depth_h_ = h;
    has_cur_ = false;
    return true;
}

bool Stereo3D::depthPass(const struct pl_hook_params *params, pl_tex prev, pl_tex target,
                         float norm, const float src_off[2], const float src_scale[2], const float texel[2])
{
    pl_shader sh = pl_dispatch_begin(params->dispatch);
    if (!sh)
        return false;

    const float has_prev = prev ? 1.0f : 0.0f;
    const float conv = convergence_;
    float verts[4][2];

    struct pl_shader_desc descs[2];
    int num_descs = 0;
    descs[num_descs++] = tex_desc("st3d_src", params->tex);
    // Bind something valid even without history; it is never sampled then.
    descs[num_descs++] = tex_desc("st3d_prev", prev ? prev : target == depth_[0] ? depth_[1] : depth_[0]);

    const struct pl_shader_var vars[] = {
        vec2_var("st3d_src_off", src_off),
        vec2_var("st3d_src_scale", src_scale),
        vec2_var("st3d_texel", texel),
        float_var("st3d_norm", &norm),
        float_var("st3d_has_prev", &has_prev),
        float_var("st3d_conv", &conv),
    };
    const struct pl_shader_va va = pos_attrib(params->gpu, verts);

    struct pl_custom_shader cs = {};
    cs.description = "chiaki 3D depth";
    cs.header = kDepthHeader;
    cs.body = kDepthBody;
    cs.input = PL_SHADER_SIG_NONE;
    cs.output = PL_SHADER_SIG_COLOR;
    cs.descriptors = descs;
    cs.num_descriptors = num_descs;
    cs.variables = vars;
    cs.num_variables = static_cast<int>(sizeof(vars) / sizeof(vars[0]));
    cs.vertex_attribs = &va;
    cs.num_vertex_attribs = 1;
    if (!pl_shader_custom(sh, &cs)) {
        pl_dispatch_abort(params->dispatch, &sh);
        return false;
    }

    struct pl_dispatch_params dp = {};
    dp.shader = &sh;
    dp.target = target;
    return pl_dispatch_finish(params->dispatch, &dp);
}

bool Stereo3D::stereoPass(const struct pl_hook_params *params, pl_tex depth, pl_tex target,
                          const float src_off[2], const float src_scale[2])
{
    pl_shader sh = pl_dispatch_begin(params->dispatch);
    if (!sh)
        return false;

    const float disp = disparity_;
    const float conv = convergence_;
    const float swap = swap_eyes_ ? -1.0f : 1.0f;
    float verts[4][2];

    const struct pl_shader_desc descs[] = {
        tex_desc("st3d_src", params->tex),
        tex_desc("st3d_depth", depth),
    };
    const struct pl_shader_var vars[] = {
        vec2_var("st3d_src_off", src_off),
        vec2_var("st3d_src_scale", src_scale),
        float_var("st3d_disp", &disp),
        float_var("st3d_conv", &conv),
        float_var("st3d_swap", &swap),
    };
    const struct pl_shader_va va = pos_attrib(params->gpu, verts);

    struct pl_custom_shader cs = {};
    cs.description = "chiaki 3D stereo";
    cs.header = kStereoHeader;
    cs.body = mode_ == Stereo3DMode::Anaglyph ? kAnaglyphBody : kSbsBody;
    cs.input = PL_SHADER_SIG_NONE;
    cs.output = PL_SHADER_SIG_COLOR;
    cs.descriptors = descs;
    cs.num_descriptors = static_cast<int>(sizeof(descs) / sizeof(descs[0]));
    cs.variables = vars;
    cs.num_variables = static_cast<int>(sizeof(vars) / sizeof(vars[0]));
    cs.vertex_attribs = &va;
    cs.num_vertex_attribs = 1;
    if (!pl_shader_custom(sh, &cs)) {
        pl_dispatch_abort(params->dispatch, &sh);
        return false;
    }

    struct pl_dispatch_params dp = {};
    dp.shader = &sh;
    dp.target = target;
    return pl_dispatch_finish(params->dispatch, &dp);
}

struct pl_hook_res Stereo3D::run(const struct pl_hook_params *params)
{
    struct pl_hook_res res = {};
    res.output = PL_HOOK_SIG_NONE;
    if (mode_ == Stereo3DMode::Off || !params->tex)
        return res;

    auto fail = [&](const char *what) {
        if (!logged_failure_) {
            logged_failure_ = true;
            qCWarning(chiakiGui) << "3D:" << what << "- passing the frame through unchanged";
        }
        struct pl_hook_res none = {};
        none.output = PL_HOOK_SIG_NONE;
        return none;
    };

    const pl_tex src = params->tex;
    const float tex_w = static_cast<float>(src->params.w);
    const float tex_h = static_cast<float>(src->params.h);
    const pl_rect2df rc = params->rect;
    const int w = std::max(1, static_cast<int>(std::lround(std::fabs(pl_rect_w(rc)))));
    const int h = std::max(1, static_cast<int>(std::lround(std::fabs(pl_rect_h(rc)))));

    const float src_off[2] = { rc.x0 / tex_w, rc.y0 / tex_h };
    const float src_scale[2] = { (rc.x1 - rc.x0) / tex_w, (rc.y1 - rc.y0) / tex_h };
    const float texel[2] = { 1.0f / tex_w, 1.0f / tex_h };

    // The hooked texture may hold e.g. 10-bit values in a 16-bit container; the
    // depth heuristics expect 0..1, so normalize for the depth pass only.
    struct pl_color_repr repr = params->repr;
    const float norm = pl_color_repr_normalize(&repr);

    if (!ensureDepthTextures(params->gpu, std::max(1, w / kDepthDivisor), std::max(1, h / kDepthDivisor)))
        return fail("could not allocate depth textures");

    if (!has_cur_ || serial_ != last_serial_) {
        const int next = has_cur_ ? (cur_ ^ 1) : cur_;
        pl_tex prev = has_cur_ ? depth_[cur_] : nullptr;
        if (!depthPass(params, prev, depth_[next], norm, src_off, src_scale, texel))
            return fail("depth pass failed");
        cur_ = next;
        has_cur_ = true;
        last_serial_ = serial_;
    }

    pl_tex out = params->get_tex(params->priv, w, h);
    if (!out)
        return fail("no output texture");
    if (!stereoPass(params, depth_[cur_], out, src_off, src_scale))
        return fail("stereo pass failed");

    res.output = PL_HOOK_SIG_TEX;
    res.tex = out;
    res.repr = params->repr;
    res.color = params->color;
    res.components = params->components;
    res.rect = { 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h) };
    return res;
}
