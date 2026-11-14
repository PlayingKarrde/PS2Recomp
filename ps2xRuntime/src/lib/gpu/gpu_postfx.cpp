// Screen effects for the GPU renderer (task fx1). See gpu_postfx.h for what they are and where the backend calls them.
//
// Reading guide:
//   the table, presets and the settings glue   first half: Params, values(), presetParams(), fromSettings(), current(), setLive()
//   the GPU passes                             second half: Chain::Impl (tilt-shift, bloom down and up, the final pass)
//
// The passes (a field of w x h texels; every texture is that size unless it says half):
//   1. tilt-shift, horizontal   the frame target                                      ->  O
//   2. tilt-shift, vertical     O                                                      ->  T       (skipped without tilt-shift)
//   3. bloom, bright + down     T                                                      ->  D1 (half)
//   4. bloom, down              D1 -> D2 -> D3 -> D4 -> D5 (each half the one before)
//   5. bloom, up                (D5, D4) -> U4, (U4, D3) -> U3, (U3, D2) -> U2, (U2, D1) -> U1     (skipped without bloom)
//   6. final                    T, U1, frame target (alpha only)  ->  the output texture: grade, bloom added, vignette.
// The input of the first pass (and of the bright pass, without tilt-shift) is the frame target itself, at the moment its scene is
// complete. Nothing here writes into the game's frame targets: they are only read.

#include "gpu/gpu_postfx.h"

#include "gpu/gpu_shaders.h"
#include "ps2_settings.h"
#include "runtime/gs/gs_gpu_threads.h"
#include "shaders/compiled/postfx_down_frag.h"
#include "shaders/compiled/postfx_final_frag.h"
#include "shaders/compiled/postfx_tilt_frag.h"
#include "shaders/compiled/postfx_up_frag.h"
#include "shaders/compiled/postfx_vert.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>

namespace ps2x_postfx
{
    // ------------------------------------------------------------------------------------------------
    // the table, the presets, the settings

    const std::vector<Value> &values()
    {
        static const std::vector<Value> table = {
            {"bloom_threshold", "Bloom threshold", 0.0f, 1.0f, 0.01f, &Params::bloomThreshold},
            {"bloom_knee", "Bloom soft knee", 0.0f, 1.0f, 0.01f, &Params::bloomKnee},
            {"bloom_intensity", "Bloom intensity", 0.0f, 2.0f, 0.01f, &Params::bloomIntensity},
            {"exposure", "Exposure (stops)", -2.0f, 2.0f, 0.01f, &Params::exposure},
            {"contrast", "Contrast", 0.5f, 1.5f, 0.01f, &Params::contrast},
            {"saturation", "Saturation", 0.0f, 2.0f, 0.01f, &Params::saturation},
            {"temperature", "Warm / cool", -1.0f, 1.0f, 0.01f, &Params::temperature},
            {"lift", "Lift (blacks)", -0.2f, 0.2f, 0.005f, &Params::lift},
            {"gamma", "Gamma (mids)", 0.5f, 2.0f, 0.01f, &Params::gamma},
            {"gain", "Gain (whites)", 0.5f, 1.5f, 0.01f, &Params::gain},
            {"tilt_centre", "Tilt-shift focus centre", 0.0f, 1.0f, 0.01f, &Params::tiltCentre},
            {"tilt_width", "Tilt-shift focus width", 0.0f, 1.0f, 0.01f, &Params::tiltWidth},
            {"tilt_falloff", "Tilt-shift fall-off", 0.02f, 1.0f, 0.01f, &Params::tiltFalloff},
            {"tilt_strength", "Tilt-shift blur (GS px)", 0.0f, 6.0f, 0.05f, &Params::tiltStrength},
            {"vignette_strength", "Vignette strength", 0.0f, 1.0f, 0.01f, &Params::vignetteStrength},
            {"vignette_radius", "Vignette radius", 0.0f, 1.2f, 0.01f, &Params::vignetteRadius},
            {"vignette_softness", "Vignette softness", 0.02f, 1.5f, 0.01f, &Params::vignetteSoftness},
        };
        return table;
    }

    const char *presetName(Preset preset)
    {
        switch (preset)
        {
        case Preset::Off: return "off";
        case Preset::Subtle: return "subtle";
        case Preset::Strong: return "strong";
        }
        return "off";
    }

    bool parsePreset(const std::string &text, Preset &out)
    {
        std::string t = text;
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (t == "off") { out = Preset::Off; return true; }
        if (t == "subtle") { out = Preset::Subtle; return true; }
        if (t == "strong") { out = Preset::Strong; return true; }
        return false;
    }

    Params presetParams(Preset preset)
    {
        Params p; // the neutral values: nothing changes the picture
        p.bloomIntensity = 0.0f;
        p.tiltStrength = 0.0f;
        p.vignetteStrength = 0.0f;
        switch (preset)
        {
        case Preset::Off:
            break;
        case Preset::Subtle:
            // Halfway between the first Subtle (task fx1: bloom 0.22, contrast 1.05, saturation 1.08, blur 1.0, vignette 0.20) and Strong.
            p.enabled = true;
            p.bloomThreshold = 0.675f;
            p.bloomKnee = 0.5f;
            p.bloomIntensity = 0.36f;
            p.exposure = 0.025f;
            p.contrast = 1.085f;
            p.saturation = 1.14f;
            p.temperature = 0.225f;
            p.gamma = 0.975f;
            p.gain = 1.01f;
            p.tiltCentre = 0.55f;
            p.tiltWidth = 0.40f;
            p.tiltFalloff = 0.30f;
            p.tiltStrength = 1.75f;
            p.vignetteStrength = 0.325f;
            p.vignetteRadius = 0.575f;
            p.vignetteSoftness = 0.65f;
            break;
        case Preset::Strong:
            p.enabled = true;
            p.bloomThreshold = 0.60f;
            p.bloomKnee = 0.5f;
            p.bloomIntensity = 0.50f;
            p.exposure = 0.05f;
            p.contrast = 1.12f;
            p.saturation = 1.20f;
            p.temperature = 0.30f;
            p.gamma = 0.95f;
            p.gain = 1.02f;
            p.tiltCentre = 0.55f;
            p.tiltWidth = 0.35f;
            p.tiltFalloff = 0.30f;
            p.tiltStrength = 2.5f;
            p.vignetteStrength = 0.45f;
            p.vignetteRadius = 0.55f;
            p.vignetteSoftness = 0.60f;
            break;
        }
        return p;
    }

    std::string envNameOf(const std::string &valueName)
    {
        std::string name = "PS2X_FX_";
        for (char c : valueName)
            name += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return name;
    }

    namespace
    {
        // The text of a setting: the settings store when the game has started it (it knows which environment variables fix a value and
        // which were only copied from the file), else the environment (the replayer and the tests have no store).
        std::string settingText(const std::string &key, const std::string &envName)
        {
            if (ps2x_settings::initialized())
                return ps2x_settings::get(key);
            const char *env = std::getenv(envName.c_str());
            return env ? std::string(env) : std::string();
        }

        bool isOn(const std::string &text)
        {
            return text == "1" || text == "on" || text == "true" || text == "yes";
        }

        float clampTo(float v, float lo, float hi)
        {
            return std::min(std::max(v, lo), hi);
        }
    }

    Params fromSettings()
    {
        Preset preset = Preset::Subtle;
        parsePreset(settingText("fx_preset", "PS2X_FX_PRESET"), preset);
        Params p = presetParams(preset == Preset::Off ? Preset::Subtle : preset); // the values the overrides sit on
        const bool wanted = isOn(settingText("fx", "PS2X_FX")) && preset != Preset::Off;
        for (const Value &value : values())
        {
            const std::string text = settingText(std::string("fx_") + value.name, envNameOf(value.name));
            if (text.empty())
                continue;
            char *end = nullptr;
            const float parsed = std::strtof(text.c_str(), &end);
            if (end != text.c_str() && std::isfinite(parsed))
                p.*(value.member) = clampTo(parsed, value.min, value.max);
        }
        p.enabled = wanted;
        return p;
    }

    namespace
    {
        struct Live
        {
            std::mutex mutex;
            Params params;
            uint64_t settingsGeneration = ~0ull; // the generation `params` was read at (nothing read yet)
        };
        Live &live()
        {
            static Live l;
            return l;
        }
    }

    Status &status()
    {
        static Status s;
        return s;
    }

    Params current()
    {
        Live &l = live();
        const uint64_t generation = ps2x_settings::generation();
        std::lock_guard<std::mutex> lock(l.mutex);
        if (generation != l.settingsGeneration)
        {
            l.params = fromSettings();
            l.settingsGeneration = generation;
        }
        return l.params;
    }

    void setLive(const Params &params)
    {
        Live &l = live();
        const uint64_t generation = ps2x_settings::generation();
        std::lock_guard<std::mutex> lock(l.mutex);
        l.params = params;
        l.settingsGeneration = generation; // the settings stay as they are until one of them changes
    }

    int saveToSettings(const Params &params)
    {
        Preset preset = Preset::Subtle;
        parsePreset(ps2x_settings::get("fx_preset"), preset);
        const Params base = presetParams(preset == Preset::Off ? Preset::Subtle : preset);
        int written = 0;
        for (const Value &value : values())
        {
            const float v = params.*(value.member);
            const bool same = std::fabs(v - base.*(value.member)) < value.step * 0.25f;
            char text[32] = "";
            if (!same)
                std::snprintf(text, sizeof(text), "%g", static_cast<double>(v));
            if (ps2x_settings::set(std::string("fx_") + value.name, text) && !same)
                ++written;
        }
        ps2x_settings::save();
        return written;
    }

    // ------------------------------------------------------------------------------------------------
    // the GPU passes

    namespace
    {
        // Must match the cbuffer Params in postfx_common.hlsli: eight float4.
        struct PassParams
        {
            float v[8][4];
        };
        static_assert(sizeof(PassParams) == 128, "postfx_common.hlsli: eight float4");
        enum Slot { kSrc, kAux, kTilt, kBloom, kGradeA, kGradeB, kVig, kMode };

        constexpr int kMaxLevels = 5;
    }

    struct Chain::Impl
    {
        SDL_GPUDevice *dev = nullptr;
        SDL_GPUShader *vs = nullptr, *fsTilt = nullptr, *fsDown = nullptr, *fsUp = nullptr, *fsFinal = nullptr;
        SDL_GPUGraphicsPipeline *pipeTilt = nullptr, *pipeDown = nullptr, *pipeUp = nullptr, *pipeFinal = nullptr;
        SDL_GPUSampler *linear = nullptr, *nearest = nullptr;
        SDL_GPUTextureFormat bloomFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

        // Sized for the picture (texW x texH); made again when the picture's size changes.
        uint32_t texW = 0, texH = 0;
        SDL_GPUTexture *tiltOut = nullptr; // T
        SDL_GPUTexture *scratch = nullptr; // O: the horizontal blur, then the finished picture
        SDL_GPUTexture *down[kMaxLevels + 1] = {};  // [1..levels]
        SDL_GPUTexture *up[kMaxLevels + 1] = {};    // [1..levels-1]
        uint32_t levelW[kMaxLevels + 1] = {}, levelH[kMaxLevels + 1] = {};
        int levels = 0;
        uint64_t bytes = 0;

        bool makeShadersAndPipelines();
        SDL_GPUTexture *makeTexture(uint32_t w, uint32_t h, SDL_GPUTextureFormat format);
        void releaseTextures();
        bool ensureTextures(uint32_t w, uint32_t h);
        void pass(SDL_GPUCommandBuffer *cmd, SDL_GPUGraphicsPipeline *pipeline, SDL_GPUTexture *target, const SDL_GPUTextureSamplerBinding *bindings,
                  uint32_t bindingCount, const PassParams &params, Stats &stats, const SDL_GPUViewport *viewport = nullptr);
        SDL_GPUTexture *record(SDL_GPUCommandBuffer *cmd, const Job &job, Stats &stats);
        ~Impl();
    };

    namespace
    {
        SDL_GPUGraphicsPipeline *makePipeline(SDL_GPUDevice *dev, SDL_GPUShader *vs, SDL_GPUShader *fs, SDL_GPUTextureFormat format)
        {
            SDL_GPUColorTargetDescription ct = {};
            ct.format = format;
            SDL_GPUGraphicsPipelineCreateInfo info = {};
            info.vertex_shader = vs;
            info.fragment_shader = fs;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
            info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
            info.target_info.color_target_descriptions = &ct;
            info.target_info.num_color_targets = 1;
            info.target_info.has_depth_stencil_target = false;
            SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &info);
            if (!p)
                std::fprintf(stderr, "[fx] SDL_CreateGPUGraphicsPipeline: %s\n", SDL_GetError());
            return p;
        }
    }

    bool Chain::Impl::makeShadersAndPipelines()
    {
        vs = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::postfx_vert);
        fsTilt = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::postfx_tilt_frag);
        fsDown = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::postfx_down_frag);
        fsUp = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::postfx_up_frag);
        fsFinal = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::postfx_final_frag);
        if (!vs || !fsTilt || !fsDown || !fsUp || !fsFinal)
            return false;
        // Half-float levels keep the glow smooth; where the device cannot render to them, 8 bits will do.
        if (SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))
            bloomFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        pipeTilt = makePipeline(dev, vs, fsTilt, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
        pipeDown = makePipeline(dev, vs, fsDown, bloomFormat);
        pipeUp = makePipeline(dev, vs, fsUp, bloomFormat);
        pipeFinal = makePipeline(dev, vs, fsFinal, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
        if (!pipeTilt || !pipeDown || !pipeUp || !pipeFinal)
            return false;

        SDL_GPUSamplerCreateInfo si = {};
        si.min_filter = SDL_GPU_FILTER_LINEAR;
        si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        linear = SDL_CreateGPUSampler(dev, &si);
        si.min_filter = SDL_GPU_FILTER_NEAREST;
        si.mag_filter = SDL_GPU_FILTER_NEAREST;
        nearest = SDL_CreateGPUSampler(dev, &si);
        return linear && nearest;
    }

    SDL_GPUTexture *Chain::Impl::makeTexture(uint32_t w, uint32_t h, SDL_GPUTextureFormat format)
    {
        SDL_GPUTextureCreateInfo ci = {};
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = format;
        ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ci.width = w;
        ci.height = h;
        ci.layer_count_or_depth = 1;
        ci.num_levels = 1;
        ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
        SDL_GPUTexture *t = SDL_CreateGPUTexture(dev, &ci);
        if (!t)
            std::fprintf(stderr, "[fx] cannot create a %ux%u texture: %s\n", w, h, SDL_GetError());
        else
            bytes += static_cast<uint64_t>(w) * h * (format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ? 8u : 4u);
        return t;
    }

    void Chain::Impl::releaseTextures()
    {
        ps2x_gs::DefragGuard guard;
        auto drop = [&](SDL_GPUTexture *&t)
        {
            if (t)
                SDL_ReleaseGPUTexture(dev, t);
            t = nullptr;
        };
        drop(tiltOut);
        drop(scratch);
        for (int i = 0; i <= kMaxLevels; ++i)
        {
            drop(down[i]);
            drop(up[i]);
        }
        bytes = 0;
        texW = texH = 0;
        levels = 0;
    }

    bool Chain::Impl::ensureTextures(uint32_t w, uint32_t h)
    {
        if (w == texW && h == texH && tiltOut)
            return true;
        releaseTextures();
        tiltOut = makeTexture(w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
        scratch = makeTexture(w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
        if (!tiltOut || !scratch)
        {
            releaseTextures();
            return false;
        }
        uint32_t lw = w, lh = h;
        levels = 0;
        for (int k = 1; k <= kMaxLevels; ++k)
        {
            lw = std::max<uint32_t>(1u, (lw + 1u) / 2u);
            lh = std::max<uint32_t>(1u, (lh + 1u) / 2u);
            if (std::min(lw, lh) < 8u && k > 2)
                break;
            levelW[k] = lw;
            levelH[k] = lh;
            down[k] = makeTexture(lw, lh, bloomFormat);
            if (!down[k])
            {
                releaseTextures();
                return false;
            }
            levels = k;
        }
        for (int k = 1; k < levels; ++k)
        {
            up[k] = makeTexture(levelW[k], levelH[k], bloomFormat);
            if (!up[k])
            {
                releaseTextures();
                return false;
            }
        }
        texW = w;
        texH = h;
        return true;
    }

    void Chain::Impl::pass(SDL_GPUCommandBuffer *cmd, SDL_GPUGraphicsPipeline *pipeline, SDL_GPUTexture *target,
                           const SDL_GPUTextureSamplerBinding *bindings, uint32_t bindingCount, const PassParams &params, Stats &stats,
                           const SDL_GPUViewport *viewport)
    {
        SDL_GPUColorTargetInfo ct = {};
        ct.texture = target;
        // Every pass writes the whole texture, except one that draws into a part of a bigger one (a viewport): the rest is kept.
        ct.load_op = viewport ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(rp, pipeline);
        if (viewport)
            SDL_SetGPUViewport(rp, viewport);
        SDL_BindGPUFragmentSamplers(rp, 0, bindings, bindingCount);
        SDL_PushGPUFragmentUniformData(cmd, 0, &params, sizeof(params));
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
        ++stats.passes;
    }

    SDL_GPUTexture *Chain::Impl::record(SDL_GPUCommandBuffer *cmd, const Job &job, Stats &stats)
    {
        const Params &p = job.params;
        if (!job.field || job.w == 0 || job.h == 0 || !ensureTextures(job.w, job.h))
            return nullptr;
        const float w = static_cast<float>(job.w), h = static_cast<float>(job.h);
        const bool tilt = p.tiltStrength > 0.001f && p.tiltWidth < 0.999f;
        const bool bloom = p.bloomIntensity > 0.001f && levels >= 2;

        PassParams base = {};
        base.v[kTilt][0] = p.tiltCentre;
        base.v[kTilt][1] = p.tiltWidth * 0.5f;
        base.v[kTilt][2] = p.tiltFalloff;
        base.v[kTilt][3] = p.tiltStrength * job.scale;
        base.v[kBloom][0] = p.bloomThreshold;
        base.v[kBloom][1] = p.bloomKnee;
        base.v[kBloom][2] = p.bloomIntensity;
        base.v[kBloom][3] = 0.75f;
        base.v[kGradeA][0] = p.exposure;
        base.v[kGradeA][1] = p.contrast;
        base.v[kGradeA][2] = p.saturation;
        base.v[kGradeA][3] = p.temperature;
        base.v[kGradeB][0] = p.lift;
        base.v[kGradeB][1] = p.gamma;
        base.v[kGradeB][2] = p.gain;
        base.v[kVig][0] = p.vignetteStrength;
        base.v[kVig][1] = p.vignetteRadius;
        base.v[kVig][2] = p.vignetteSoftness;
        base.v[kVig][3] = job.aspect;

        SDL_GPUTexture *source = job.field;
        SDL_GPUTexture *input = source; // what the later passes read as "the scene"
        float inX = static_cast<float>(job.x), inY = static_cast<float>(job.y);

        if (tilt)
        {
            PassParams a = base;
            a.v[kSrc][0] = inX; a.v[kSrc][1] = inY; a.v[kSrc][2] = w; a.v[kSrc][3] = h;
            a.v[kAux][0] = 1.0f; a.v[kAux][1] = 0.0f;
            SDL_GPUTextureSamplerBinding b0 = {source, linear};
            pass(cmd, pipeTilt, scratch, &b0, 1, a, stats);
            PassParams b = base;
            b.v[kSrc][0] = 0.0f; b.v[kSrc][1] = 0.0f; b.v[kSrc][2] = w; b.v[kSrc][3] = h;
            b.v[kAux][0] = 0.0f; b.v[kAux][1] = 1.0f;
            SDL_GPUTextureSamplerBinding b1 = {scratch, linear};
            pass(cmd, pipeTilt, tiltOut, &b1, 1, b, stats);
            input = tiltOut;
            inX = inY = 0.0f;
        }

        SDL_GPUTexture *glow = nullptr;
        if (bloom)
        {
            PassParams d = base;
            d.v[kSrc][0] = inX; d.v[kSrc][1] = inY; d.v[kSrc][2] = w; d.v[kSrc][3] = h;
            d.v[kMode][0] = 1.0f; // the first step applies the threshold
            SDL_GPUTextureSamplerBinding bd = {input, linear};
            pass(cmd, pipeDown, down[1], &bd, 1, d, stats);
            for (int k = 2; k <= levels; ++k)
            {
                PassParams e = base;
                e.v[kSrc][0] = 0.0f; e.v[kSrc][1] = 0.0f;
                e.v[kSrc][2] = static_cast<float>(levelW[k - 1]); e.v[kSrc][3] = static_cast<float>(levelH[k - 1]);
                e.v[kMode][0] = 0.0f;
                SDL_GPUTextureSamplerBinding be = {down[k - 1], linear};
                pass(cmd, pipeDown, down[k], &be, 1, e, stats);
            }
            SDL_GPUTexture *low = down[levels];
            for (int k = levels - 1; k >= 1; --k)
            {
                SDL_GPUTextureSamplerBinding bu[2] = {{low, linear}, {down[k], linear}};
                pass(cmd, pipeUp, up[k], bu, 2, base, stats);
                low = up[k];
            }
            glow = low;
        }

        PassParams f = base;
        f.v[kSrc][0] = inX; f.v[kSrc][1] = inY; f.v[kSrc][2] = w; f.v[kSrc][3] = h;
        f.v[kAux][0] = static_cast<float>(job.x); f.v[kAux][1] = static_cast<float>(job.y);
        f.v[kMode][1] = glow ? 1.0f : 0.0f;
        SDL_GPUTextureSamplerBinding bf[3] = {{input, nearest}, {glow ? glow : tiltOut, linear}, {job.field, nearest}};
        if (job.output)
        {
            // Into the frame target's own coordinates: the picture at (x, y) of a texture the size of the frame target.
            f.v[kAux][2] = static_cast<float>(job.x); f.v[kAux][3] = static_cast<float>(job.y);
            const SDL_GPUViewport viewport = {static_cast<float>(job.x), static_cast<float>(job.y), w, h, 0.0f, 1.0f};
            pass(cmd, pipeFinal, job.output, bf, 3, f, stats, &viewport);
            return job.output;
        }
        pass(cmd, pipeFinal, scratch, bf, 3, f, stats);
        return scratch;
    }

    Chain::Impl::~Impl()
    {
        releaseTextures();
        ps2x_gs::DefragGuard guard;
        for (SDL_GPUGraphicsPipeline *p : {pipeTilt, pipeDown, pipeUp, pipeFinal})
            if (p)
                SDL_ReleaseGPUGraphicsPipeline(dev, p);
        for (SDL_GPUShader *s : {vs, fsTilt, fsDown, fsUp, fsFinal})
            if (s)
                SDL_ReleaseGPUShader(dev, s);
        if (linear)
            SDL_ReleaseGPUSampler(dev, linear);
        if (nearest)
            SDL_ReleaseGPUSampler(dev, nearest);
    }

    Chain::Chain(SDL_GPUDevice *device) : m_impl(new Impl)
    {
        m_impl->dev = device;
        m_ready = device != nullptr && m_impl->makeShadersAndPipelines();
        if (!m_ready)
            std::fprintf(stderr, "[fx] the screen effects could not be set up; they stay off\n");
    }

    Chain::~Chain()
    {
        delete m_impl;
    }

    SDL_GPUTexture *Chain::record(SDL_GPUCommandBuffer *commandBuffer, const Job &job, bool passLockHeld)
    {
        if (!m_ready)
            return nullptr;
        std::optional<ps2x_gs::DebugPassLock> passLock; // validation only: see gs_gpu_threads.h
        if (!passLockHeld)
            passLock.emplace();
        SDL_GPUTexture *result = m_impl->record(commandBuffer, job, m_stats);
        if (result)
        {
            ++m_stats.fields;
            m_stats.pictureW = job.w;
            m_stats.pictureH = job.h;
            m_stats.textureBytes = m_impl->bytes;
        }
        return result;
    }

    double Chain::timeEmpty()
    {
        if (!m_ready)
            return -1.0;
        SDL_WaitForGPUIdle(m_impl->dev);
        const uint64_t t0 = SDL_GetTicksNS();
        SDL_GPUFence *fence = nullptr;
        {
            ps2x_gs::DefragGuard window;
            SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_impl->dev);
            fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        }
        if (!fence)
            return -1.0;
        SDL_WaitForGPUFences(m_impl->dev, true, &fence, 1);
        const uint64_t t1 = SDL_GetTicksNS();
        SDL_ReleaseGPUFence(m_impl->dev, fence);
        return static_cast<double>(t1 - t0) / 1000.0;
    }

    double Chain::timeOnce(const Job &job)
    {
        if (!m_ready || !job.field)
            return -1.0;
        SDL_WaitForGPUIdle(m_impl->dev);
        const uint64_t t0 = SDL_GetTicksNS();
        SDL_GPUFence *fence = nullptr;
        {
            ps2x_gs::DefragGuard window;
            SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_impl->dev);
            Stats scratchStats;
            Job own = job;
            own.output = nullptr; // never into the frame target's picture: a field may still be shown from it
            {
                ps2x_gs::DebugPassLock passLock;
                m_impl->record(cmd, own, scratchStats);
            }
            fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        }
        if (!fence)
            return -1.0;
        SDL_WaitForGPUFences(m_impl->dev, true, &fence, 1);
        const uint64_t t1 = SDL_GetTicksNS();
        SDL_ReleaseGPUFence(m_impl->dev, fence);
        return static_cast<double>(t1 - t0) / 1000.0;
    }
}
