// Screen effects for the GPU renderer (task fx1). See gpu_postfx.h for what they are and where the backend calls them.
//
// Reading guide:
//   the table, presets and the settings glue   first half: Params, values(), presetParams(), fromSettings(), current(), setLive()
//   the GPU passes                             second half: Chain::Impl (tilt-shift, bloom down and up, the final pass)
//
// The passes (a field of w x h texels; every texture is that size unless it says half; 8 = 8-bit display encoding, F = half-float
// linear light, task fx2):
//   1. tilt-shift, horizontal   the frame target (8)                                   ->  O (8)   taps mixed in linear light, result encoded again
//   2. tilt-shift, vertical     O                                                      ->  T (8)   (skipped without tilt-shift)
//   3. bloom, bright + down     T (8, made linear by the pass)                         ->  D1 (half, F)
//   4. bloom, down              D1 -> D2 -> D3 -> D4 -> D5 (each half the one before, F)
//   5. bloom, up                (D5, D4) -> U4, (U4, D3) -> U3, (U3, D2) -> U2, (U2, D1) -> U1     (F; skipped without bloom)
//   6. final                    T (8), U1, frame target (alpha only)  ->  the output texture (8): linear light, glow added, grade, the
//                               highlight shoulder, back to the display encoding, vignette.
// With the match lights (a job with a light buffer) a multiply pass comes first: the frame target times its light buffer. With
// wrestler shape (a job with a wrestler mask too, task lt3-T1) three passes come before that one: the mask blurred across, then
// down, then the light buffer copied with the wrestlers' pixels lit again (postfx_rim_blur.frag.hlsl, postfx_rim.frag.hlsl); the
// multiply pass reads that copy.
// The input of the first pass (and of the bright pass, without tilt-shift) is the frame target itself, at the moment its scene is
// complete. Nothing here writes into the game's frame targets: they are only read.

#include "gpu/gpu_postfx.h"

#include "gpu/gpu_shaders.h"
#include "ps2_settings.h"
#include "runtime/gs/gs_gpu_threads.h"
#include "shaders/compiled/postfx_down_frag.h"
#include "shaders/compiled/postfx_final_frag.h"
#include "shaders/compiled/postfx_light_frag.h"
#include "shaders/compiled/postfx_rim_blur_frag.h"
#include "shaders/compiled/postfx_rim_frag.h"
#include "shaders/compiled/postfx_rim_n_frag.h"
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
            {"highlight_start", "Highlight roll-off start (1 = none)", 0.3f, 1.0f, 0.01f, &Params::highlightStart},
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
            // Task fx2: the numbers of the linear-light chain that come closest to the look of the fx1b Subtle (bloom 0.36, contrast 1.085,
            // saturation 1.14, temperature 0.225 on display values; fitted on the recorded ring, introductions and time-over fields against
            // the old picture with its highlights rolled off instead of cut: docs/hd-lighting.md, "Highlights"). Only what is nearly white
            // glows (threshold 0.78). The tilt-shift and vignette are as they were.
            p.enabled = true;
            p.bloomThreshold = 0.78f;
            p.bloomKnee = 0.5f;
            p.bloomIntensity = 0.40f;
            p.exposure = 0.05f;
            p.contrast = 1.25f;
            p.saturation = 1.06f;
            p.temperature = 0.23f;
            p.gamma = 1.04f;
            p.gain = 1.0f;
            p.highlightStart = 0.75f;
            p.tiltCentre = 0.55f;
            p.tiltWidth = 0.40f;
            p.tiltFalloff = 0.30f;
            p.tiltStrength = 1.75f;
            p.vignetteStrength = 0.325f;
            p.vignetteRadius = 0.575f;
            p.vignetteSoftness = 0.65f;
            break;
        case Preset::Strong:
            // Task fx2: the same glow around the lights, warmth, vignette and tilt-shift as before, in linear light with the highlights rolled
            // off. The fx1b numbers (bloom 0.50 added to display values from a threshold of 0.60, exposure 0.05, contrast 1.12, saturation
            // 1.20, temperature 0.30, gamma 0.95, gain 1.02) cut 38.6% of the ring picture at 255 in some channel and 8.1% in all three.
            p.enabled = true;
            p.bloomThreshold = 0.80f;
            p.bloomKnee = 0.5f;
            p.bloomIntensity = 0.70f;
            p.exposure = 0.15f;
            p.contrast = 1.28f;
            p.saturation = 1.25f;
            p.temperature = 0.26f;
            p.gamma = 1.12f;
            p.gain = 1.0f;
            p.highlightStart = 0.70f;
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

        // An effect's switch is on unless it says otherwise (blank is the default, on).
        bool switchOn(const std::string &text)
        {
            return !(text == "0" || text == "off" || text == "false" || text == "no");
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
        p.bloomOn = switchOn(settingText("fx_bloom", "PS2X_FX_BLOOM"));
        p.gradingOn = switchOn(settingText("fx_grading", "PS2X_FX_GRADING"));
        p.tiltOn = switchOn(settingText("fx_tiltshift", "PS2X_FX_TILTSHIFT"));
        p.vignetteOn = switchOn(settingText("fx_vignette", "PS2X_FX_VIGNETTE"));
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
        // the single effects' switches (written whatever they are: they have no preset to differ from)
        ps2x_settings::set("fx_bloom", params.bloomOn ? "on" : "off");
        ps2x_settings::set("fx_grading", params.gradingOn ? "on" : "off");
        ps2x_settings::set("fx_tiltshift", params.tiltOn ? "on" : "off");
        ps2x_settings::set("fx_vignette", params.vignetteOn ? "on" : "off");
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

        // The display encoding (sRGB) of one channel to linear light: what postfx_common.hlsli does in the shaders.
        float srgbToLinear(float c)
        {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        // The white that maps to 1.0 on the highlight shoulder, in linear light (a white two stops over the picture's own white).
        constexpr float kShoulderWhite = 4.0f;
        // Match lights: where the shoulder starts when the look has none of its own (a lit pixel can be brighter than white).
        constexpr float kLightShoulderStart = 0.7f;
    }

    struct Chain::Impl
    {
        SDL_GPUDevice *dev = nullptr;
        SDL_GPUShader *vs = nullptr, *fsTilt = nullptr, *fsDown = nullptr, *fsUp = nullptr, *fsFinal = nullptr;
        SDL_GPUGraphicsPipeline *pipeTilt = nullptr, *pipeDown = nullptr, *pipeUp = nullptr, *pipeFinal = nullptr;
        SDL_GPUSampler *linear = nullptr, *nearest = nullptr;
        SDL_GPUTextureFormat bloomFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        // Match lights (task lt2-L1): the multiply pass, in its two forms, and the blur for a picture brighter than white. Ready only
        // where the device renders to half-float; without it a job's light buffer is ignored (logged once).
        SDL_GPUShader *fsLight = nullptr;
        SDL_GPUGraphicsPipeline *pipeLightHdr = nullptr, *pipeLightOut = nullptr, *pipeTiltHdr = nullptr;
        bool lightReady = false, lightWarned = false;
        uint32_t litW = 0, litH = 0;
        SDL_GPUTexture *lit = nullptr;        // the lit scene: half-float, the display encoding carried on above 1 (postfx_hdr.hlsli)
        SDL_GPUTexture *scratchHdr = nullptr; // the blur's two steps for such a picture
        SDL_GPUTexture *tiltOutHdr = nullptr;
        bool ensureLitTextures(uint32_t w, uint32_t h, bool blur);
        // Wrestler shape (task lt3-T1): the mask's blur and the pass that lights the wrestlers again. Made the first time a job asks.
        SDL_GPUShader *fsRimBlur = nullptr, *fsRim = nullptr;
        SDL_GPUGraphicsPipeline *pipeRimBlur = nullptr, *pipeRim = nullptr;
        bool rimTried = false, rimReady = false;
        uint32_t rimW = 0, rimH = 0, relitW = 0, relitH = 0;
        SDL_GPUTexture *rimA = nullptr, *rimB = nullptr; // the mask blurred across, then down too: half-float, the picture's size
        SDL_GPUTexture *relit = nullptr;                 // the light buffer with the wrestlers lit again: half-float, the frame target's size
        bool ensureRim(const Job &job);
        // Generated shape (task lt3-T3): the same pass reading the cards' normal maps as well. Made the first time a job asks.
        SDL_GPUShader *fsRimN = nullptr;
        SDL_GPUGraphicsPipeline *pipeRimN = nullptr;
        bool rimNTried = false;
        SDL_GPUTexture *recordRim(SDL_GPUCommandBuffer *cmd, const Job &job, Stats &stats);

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
        if (bloomFormat == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT)
        {
            fsLight = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::postfx_light_frag);
            if (fsLight)
            {
                pipeLightHdr = makePipeline(dev, vs, fsLight, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
                pipeLightOut = makePipeline(dev, vs, fsLight, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
                pipeTiltHdr = makePipeline(dev, vs, fsTilt, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
            }
            lightReady = pipeLightHdr && pipeLightOut && pipeTiltHdr;
        }

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
        drop(lit);
        drop(scratchHdr);
        drop(tiltOutHdr);
        drop(rimA);
        drop(rimB);
        drop(relit);
        rimW = rimH = relitW = relitH = 0;
        litW = litH = 0;
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

    // The textures of a lit scene, made when a job first brings a light buffer (and the two for the blur when the blur first runs on one).
    bool Chain::Impl::ensureLitTextures(uint32_t w, uint32_t h, bool blur)
    {
        if (lit && (w != litW || h != litH))
        {
            ps2x_gs::DefragGuard guard;
            for (SDL_GPUTexture **t : {&lit, &scratchHdr, &tiltOutHdr})
            {
                if (*t)
                {
                    SDL_ReleaseGPUTexture(dev, *t);
                    bytes -= static_cast<uint64_t>(litW) * litH * 8u;
                }
                *t = nullptr;
            }
        }
        if (!lit)
            lit = makeTexture(w, h, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
        litW = w;
        litH = h;
        if (blur && !scratchHdr)
            scratchHdr = makeTexture(w, h, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
        if (blur && !tiltOutHdr)
            tiltOutHdr = makeTexture(w, h, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
        return lit && (!blur || (scratchHdr && tiltOutHdr));
    }

    // Wrestler shape: the shaders and pipelines (once), and the textures for this job's sizes.
    bool Chain::Impl::ensureRim(const Job &job)
    {
        if (!rimTried)
        {
            rimTried = true;
            fsRimBlur = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::postfx_rim_blur_frag);
            fsRim = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::postfx_rim_frag);
            if (fsRimBlur && fsRim)
            {
                pipeRimBlur = makePipeline(dev, vs, fsRimBlur, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
                pipeRim = makePipeline(dev, vs, fsRim, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
            }
            rimReady = pipeRimBlur && pipeRim;
            if (!rimReady)
                std::fprintf(stderr, "[light] the wrestler shape pass could not be set up; the wrestlers stay tinted by position only\n");
        }
        if (!rimReady)
            return false;
        auto remake = [&](SDL_GPUTexture *&t, uint32_t oldW, uint32_t oldH, uint32_t w, uint32_t h)
        {
            if (t && (oldW != w || oldH != h))
            {
                ps2x_gs::DefragGuard guard;
                SDL_ReleaseGPUTexture(dev, t);
                bytes -= static_cast<uint64_t>(oldW) * oldH * 8u;
                t = nullptr;
            }
            if (!t)
                t = makeTexture(w, h, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
        };
        remake(rimA, rimW, rimH, job.w, job.h);
        remake(rimB, rimW, rimH, job.w, job.h);
        rimW = job.w;
        rimH = job.h;
        remake(relit, relitW, relitH, job.targetW, job.targetH);
        relitW = job.targetW;
        relitH = job.targetH;
        return rimA && rimB && relit;
    }

    // Wrestler shape: three passes; returns the texture the multiply pass reads in place of the light buffer (null: the light buffer).
    SDL_GPUTexture *Chain::Impl::recordRim(SDL_GPUCommandBuffer *cmd, const Job &job, Stats &stats)
    {
        if (!job.light || !job.rimMask || !job.rimWhere || job.targetW == 0 || job.targetH == 0 || !(job.rimShape > 0.0f || job.rimStrength > 0.0f) ||
            job.rimX1 < job.rimX0 || job.rimY1 < job.rimY0 || !ensureRim(job))
            return nullptr;
        const float w = static_cast<float>(job.w), h = static_cast<float>(job.h);
        const float sigma = std::clamp(job.rimSigma, 0.25f, 25.0f);
        const int reach = static_cast<int>(std::min(std::ceil(sigma * 2.5f), 64.0f));
        // The wrestlers' box in texels of the picture. The second blur is needed there (and one texel around it, for the slope); the
        // first, which the second reads up and down, in rows `reach` further out. Nothing outside those boxes is read by the last pass.
        const int px0 = job.rimX0 - static_cast<int>(job.x), py0 = job.rimY0 - static_cast<int>(job.y);
        const int px1 = job.rimX1 - static_cast<int>(job.x), py1 = job.rimY1 - static_cast<int>(job.y);
        const auto box = [&](int growX, int growY) {
            const int x0 = std::clamp(px0 - growX, 0, static_cast<int>(job.w)), y0 = std::clamp(py0 - growY, 0, static_cast<int>(job.h));
            const int x1 = std::clamp(px1 + 1 + growX, 0, static_cast<int>(job.w)), y1 = std::clamp(py1 + 1 + growY, 0, static_cast<int>(job.h));
            return SDL_Rect{x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0)};
        };
        const SDL_Rect across = box(2, reach + 2), down = box(2, 2);
        if (down.w <= 0 || down.h <= 0)
            return nullptr; // the wrestlers are outside the picture
        PassParams b = {};
        b.v[kSrc][0] = static_cast<float>(job.x); b.v[kSrc][1] = static_cast<float>(job.y); b.v[kSrc][2] = w; b.v[kSrc][3] = h;
        b.v[kAux][0] = 1.0f; b.v[kAux][1] = 0.0f;
        b.v[kTilt][2] = sigma;
        const auto blurPass = [&](SDL_GPUTexture *target, SDL_GPUTexture *source, const SDL_Rect &scissor) {
            SDL_GPUColorTargetInfo ct = {};
            ct.texture = target;
            ct.load_op = SDL_GPU_LOADOP_DONT_CARE; // only the box is written, and only the box is read
            ct.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(rp, pipeRimBlur);
            SDL_SetGPUScissor(rp, &scissor);
            const SDL_GPUTextureSamplerBinding binding = {source, nearest};
            SDL_BindGPUFragmentSamplers(rp, 0, &binding, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &b, sizeof(b));
            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
            SDL_EndGPURenderPass(rp);
            ++stats.passes;
        };
        blurPass(rimA, job.rimMask, across);
        b.v[kSrc][0] = 0.0f; b.v[kSrc][1] = 0.0f;
        b.v[kAux][0] = 0.0f; b.v[kAux][1] = 1.0f;
        blurPass(rimB, rimA, down);

        PassParams r = {};
        r.v[kSrc][0] = static_cast<float>(job.x); r.v[kSrc][1] = static_cast<float>(job.y); r.v[kSrc][2] = w; r.v[kSrc][3] = h;
        r.v[kAux][0] = job.rimEye[0]; r.v[kAux][1] = job.rimEye[1]; r.v[kAux][2] = job.rimEye[2]; r.v[kAux][3] = job.rimRightSign;
        r.v[kTilt][0] = job.rimShape;
        r.v[kTilt][1] = job.rimStrength;
        r.v[kTilt][3] = job.rimLean;
        // Generated shape: the pass with the cards' normal maps, when the job has them and asks for them (postfx_rim_n.frag.hlsl).
        bool withNormals = job.rimLeanMap && job.rimNormals > 0.0f && job.rimShape > 0.0f;
        if (withNormals && !rimNTried)
        {
            rimNTried = true;
            fsRimN = ps2x::gpu::createShader(dev, ps2x::gpu::shaders::postfx_rim_n_frag);
            if (fsRimN)
                pipeRimN = makePipeline(dev, vs, fsRimN, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
            if (!pipeRimN)
                std::fprintf(stderr, "[light] the pass for the wrestlers' generated shape could not be set up; they keep the shape of their outline alone\n");
        }
        withNormals = withNormals && pipeRimN;
        if (withNormals)
        {
            r.v[kBloom][0] = job.rimNormals;
            r.v[kBloom][1] = job.rimPaint;
            r.v[kBloom][2] = job.rimCrease;
            r.v[kBloom][3] = job.rimOutline;
            r.v[kTilt][2] = job.rimTexel; // (only the pass with the maps reads it: the tagged parts' crease rule)
        }
        {
            SDL_GPUColorTargetInfo ct = {};
            ct.texture = relit;
            ct.load_op = SDL_GPU_LOADOP_DONT_CARE; // the picture's rectangle is written whole, and the multiply pass reads nothing else
            ct.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(rp, withNormals ? pipeRimN : pipeRim);
            const SDL_GPUViewport viewport = {static_cast<float>(job.x), static_cast<float>(job.y), w, h, 0.0f, 1.0f};
            SDL_SetGPUViewport(rp, &viewport);
            const SDL_GPUTextureSamplerBinding bindings[4] = {{job.light, nearest}, {job.rimWhere, nearest}, {rimB, nearest}, {job.rimLeanMap, nearest}};
            SDL_BindGPUFragmentSamplers(rp, 0, bindings, withNormals ? 4 : 3);
            SDL_PushGPUFragmentUniformData(cmd, 0, &r, sizeof(r));
            SDL_PushGPUFragmentUniformData(cmd, 1, job.rimLights, sizeof(job.rimLights));
            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
            SDL_EndGPURenderPass(rp);
            ++stats.passes;
        }
        return relit;
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
        // An effect that is off does none of its work: no passes for the tilt-shift and the bloom, and no grading or vignette maths in the
        // final pass (the shader reads the two switches from Tilt.x and Tilt.y, which that pass does not otherwise use).
        const bool tilt = p.tiltOn && p.tiltStrength > 0.001f && p.tiltWidth < 0.999f;
        const bool bloom = p.bloomOn && p.bloomIntensity > 0.001f && levels >= 2;

        PassParams base = {};
        base.v[kTilt][0] = p.tiltCentre;
        base.v[kTilt][1] = p.tiltWidth * 0.5f;
        base.v[kTilt][2] = p.tiltFalloff;
        base.v[kTilt][3] = p.tiltStrength * job.scale;
        base.v[kBloom][0] = srgbToLinear(p.bloomThreshold); // the setting is a brightness as displayed; the bloom works in linear light
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
        // The shoulder belongs to the light: it runs when the glow adds light or the grading scales it, not for a vignette or a blur alone.
        base.v[kMode][2] = (p.gradingOn || bloom) ? p.highlightStart : 1.0f;

        // Match lights: the scene times its light buffer, in linear light, before anything else.
        bool hdr = false; // the picture the passes below read is a lit scene in a half-float texture
        SDL_GPUTexture *litScene = nullptr;
        if (job.light && !lightReady && !lightWarned)
        {
            lightWarned = true;
            std::fprintf(stderr, "[light] this device cannot render to half-float textures: the lights are not applied\n");
        }
        if (job.light && lightReady)
        {
            PassParams l = base;
            l.v[kSrc][0] = static_cast<float>(job.x); l.v[kSrc][1] = static_cast<float>(job.y); l.v[kSrc][2] = w; l.v[kSrc][3] = h;
            // Wrestler shape: the light buffer copied with the wrestlers lit again, when the job asks for it.
            SDL_GPUTexture *const shaped = recordRim(cmd, job, stats);
            const SDL_GPUTextureSamplerBinding bl[2] = {{job.field, nearest}, {shaped ? shaped : job.light, nearest}};
            if (!tilt && !bloom && !p.gradingOn && !p.vignetteOn)
            {
                // No effect is on: this is the only pass, and it writes the finished picture (postfx_light.frag.hlsl, form 1).
                l.v[kMode][0] = 1.0f;
                l.v[kMode][2] = kLightShoulderStart;
                l.v[kMode][3] = kShoulderWhite;
                if (job.output)
                {
                    l.v[kAux][2] = static_cast<float>(job.x); l.v[kAux][3] = static_cast<float>(job.y);
                    const SDL_GPUViewport viewport = {static_cast<float>(job.x), static_cast<float>(job.y), w, h, 0.0f, 1.0f};
                    pass(cmd, pipeLightOut, job.output, bl, 2, l, stats, &viewport);
                    return job.output;
                }
                pass(cmd, pipeLightOut, scratch, bl, 2, l, stats);
                return scratch;
            }
            if (ensureLitTextures(job.w, job.h, tilt))
            {
                pass(cmd, pipeLightHdr, lit, bl, 2, l, stats);
                litScene = lit;
                hdr = true;
                // Light can push a pixel over white: it needs the shoulder even when the look has none.
                if (base.v[kMode][2] >= 0.999f)
                    base.v[kMode][2] = kLightShoulderStart;
            }
        }
        base.v[kMode][3] = kShoulderWhite;

        SDL_GPUTexture *source = litScene ? litScene : job.field;
        SDL_GPUTexture *input = source; // what the later passes read as "the scene"
        float inX = litScene ? 0.0f : static_cast<float>(job.x), inY = litScene ? 0.0f : static_cast<float>(job.y);

        if (tilt)
        {
            PassParams a = base;
            a.v[kSrc][0] = inX; a.v[kSrc][1] = inY; a.v[kSrc][2] = w; a.v[kSrc][3] = h;
            a.v[kAux][0] = 1.0f; a.v[kAux][1] = 0.0f;
            a.v[kAux][2] = hdr ? 1.0f : 0.0f; // a lit scene stays brighter than white through the blur (half-float textures)
            SDL_GPUTexture *const blurA = hdr ? scratchHdr : scratch;
            SDL_GPUTexture *const blurB = hdr ? tiltOutHdr : tiltOut;
            SDL_GPUGraphicsPipeline *const pipeBlur = hdr ? pipeTiltHdr : pipeTilt;
            SDL_GPUTextureSamplerBinding b0 = {source, linear};
            pass(cmd, pipeBlur, blurA, &b0, 1, a, stats);
            PassParams b = base;
            b.v[kSrc][0] = 0.0f; b.v[kSrc][1] = 0.0f; b.v[kSrc][2] = w; b.v[kSrc][3] = h;
            b.v[kAux][0] = 0.0f; b.v[kAux][1] = 1.0f;
            b.v[kAux][2] = hdr ? 1.0f : 0.0f;
            SDL_GPUTextureSamplerBinding b1 = {blurA, linear};
            pass(cmd, pipeBlur, blurB, &b1, 1, b, stats);
            input = blurB;
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
        f.v[kTilt][0] = p.gradingOn ? 1.0f : 0.0f;
        f.v[kTilt][1] = p.vignetteOn ? 1.0f : 0.0f;
        // Without bloom the glow slot still needs a texture, and it must not be one that this pass uses otherwise (the picture, the field, or
        // `scratch`, which is the target of the last pass when job.output is null): the first bloom level is free then. With the tilt-shift on
        // and the bloom off the old placeholder was the picture itself, and one texture bound twice in a pass was left in the wrong layout
        // under Vulkan's validation (found by the switches).
        SDL_GPUTextureSamplerBinding bf[3] = {{input, nearest}, {glow ? glow : down[1], linear}, {job.field, nearest}};
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
        for (SDL_GPUGraphicsPipeline *p : {pipeTilt, pipeDown, pipeUp, pipeFinal, pipeLightHdr, pipeLightOut, pipeTiltHdr, pipeRimBlur, pipeRim, pipeRimN})
            if (p)
                SDL_ReleaseGPUGraphicsPipeline(dev, p);
        for (SDL_GPUShader *s : {vs, fsTilt, fsDown, fsUp, fsFinal, fsLight, fsRimBlur, fsRim, fsRimN})
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
        const uint64_t passesBefore = m_stats.passes;
        SDL_GPUTexture *result = m_impl->record(commandBuffer, job, m_stats);
        if (result)
        {
            status().lastPasses.store(static_cast<uint32_t>(m_stats.passes - passesBefore), std::memory_order_relaxed);
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
