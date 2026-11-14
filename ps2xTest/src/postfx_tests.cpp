#include "MiniTest.h"
#include "gpu/gpu_postfx.h"
#include "ps2_settings.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/gs_presentation.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Tests for the screen effects (task fx1): the table, presets and settings (always run), and the effect chain with the GPU renderer on a
// synthetic match field (skipped when no GPU device can be created). The scene has the shape of a match field: a clear, hundreds of
// triangle lists, then an overlay primitive with a Z of 65536 or more.

namespace
{
    namespace fs = std::filesystem;

    void setEnvVar(const char *name, const char *value)
    {
#if defined(_WIN32)
        _putenv_s(name, value ? value : "");
#else
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
#endif
    }

    void clearFxVariables()
    {
        for (const ps2x_settings::Definition &def : ps2x_settings::definitions())
            if (std::strncmp(def.key, "fx", 2) == 0)
                setEnvVar(def.envName, nullptr);
    }

    std::string readAll(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    // ---- the synthetic match field (progressive GPU renderer, scale 1: the picture is 640 x 448, a GS row is two picture rows)

    GSContext matchContext()
    {
        GSContext ctx{};
        ctx.frame = {0, 10, GS_PSM_CT32, 0};
        ctx.zbuf = {140, GS_PSM_Z24, false};
        ctx.scissor = {0, 639, 0, 223};
        ctx.xyoffset = {static_cast<uint16_t>(1728u * 16u), static_cast<uint16_t>(1936u * 16u)};
        ctx.test = (1ull << 16) | (2ull << 17); // ZTE on, GEQUAL
        return ctx;
    }

    void sprite(GSRasterBackend &b, float x0, float y0, float x1, float y1, double z, uint8_t r, uint8_t g, uint8_t bl, bool alwaysPass = false)
    {
        GSPrimitiveBatch p;
        p.vertexCount = 2;
        p.state.context = matchContext();
        if (alwaysPass)
            p.state.context.test = (1ull << 16) | (1ull << 17); // ZTE on, ZTST ALWAYS: what the game's clear uses
        p.state.prim.type = GS_PRIM_SPRITE;
        p.vertices[0].x = 1728.0f + x0;
        p.vertices[0].y = 1936.0f + y0;
        p.vertices[1].x = 1728.0f + x1;
        p.vertices[1].y = 1936.0f + y1;
        p.vertices[1].z = z;
        p.vertices[1].r = r;
        p.vertices[1].g = g;
        p.vertices[1].b = bl;
        p.vertices[1].a = 0x80;
        b.Submit(p);
    }

    void triangle(GSRasterBackend &b, float x0, float y0, float x1, float y1, float x2, float y2, double z, uint8_t r, uint8_t g, uint8_t bl)
    {
        GSPrimitiveBatch p;
        p.vertexCount = 3;
        p.state.context = matchContext();
        p.state.prim.type = GS_PRIM_TRIANGLE;
        p.state.prim.iip = false;
        const float xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
        for (size_t i = 0; i < 3; ++i)
        {
            p.vertices[i].x = 1728.0f + xs[i];
            p.vertices[i].y = 1936.0f + ys[i];
            p.vertices[i].z = z;
            p.vertices[i].r = r;
            p.vertices[i].g = g;
            p.vertices[i].b = bl;
            p.vertices[i].a = 0x80;
        }
        b.Submit(p);
    }

    constexpr double kOverlayZ = 65536.0;

    // A flat box drawn with a blend: alphaReg is the GS ALPHA register (A, B, C, D), alpha the vertex alpha (0x80 is 1.0).
    void blendedBox(GSRasterBackend &b, float x0, float y0, float x1, float y1, double z, uint8_t r, uint8_t g, uint8_t bl, uint8_t alpha, uint64_t alphaReg)
    {
        GSPrimitiveBatch p;
        p.vertexCount = 2;
        p.state.context = matchContext();
        p.state.context.alpha = alphaReg;
        p.state.prim.type = GS_PRIM_SPRITE;
        p.state.prim.abe = true;
        p.vertices[0].x = 1728.0f + x0;
        p.vertices[0].y = 1936.0f + y0;
        p.vertices[1].x = 1728.0f + x1;
        p.vertices[1].y = 1936.0f + y1;
        p.vertices[1].z = z;
        p.vertices[1].r = r;
        p.vertices[1].g = g;
        p.vertices[1].b = bl;
        p.vertices[1].a = alpha;
        b.Submit(p);
    }
    constexpr uint64_t kBlendOver = 0ull | (1ull << 2) | (0ull << 4) | (1ull << 6);     // (Cs - Cd) * As + Cd
    constexpr uint64_t kBlendAdd = 0ull | (2ull << 2) | (0ull << 4) | (1ull << 6);      // (Cs - 0) * As + Cd

    // A translucent black layer over the whole field, as the game's fades are (a sprite at Z 0xFFFF0 with the alpha blend (Cs - Cd) * As + Cd).
    void fade(GSRasterBackend &b, uint8_t alpha)
    {
        GSPrimitiveBatch p;
        p.vertexCount = 2;
        p.state.context = matchContext();
        p.state.context.alpha = 0ull | (1ull << 2) | (0ull << 4) | (1ull << 6);
        p.state.prim.type = GS_PRIM_SPRITE;
        p.state.prim.abe = true;
        p.vertices[0].x = 1728.0f;
        p.vertices[0].y = 1936.0f;
        p.vertices[1].x = 1728.0f + 640.0f;
        p.vertices[1].y = 1936.0f + 224.0f;
        p.vertices[1].z = 1048560.0;
        p.vertices[1].a = alpha;
        b.Submit(p);
    }

    // A match field: the clear, a grid of 480 triangles (the scene), thin stripes at the top, a bright blob, then (optionally) an opaque
    // overlay box. `triangles` false draws the same with sprites only (a 2D scene).
    void drawField(GSRasterBackend &b, bool overlay, bool triangles = true, float overlayDx = 0.0f, uint8_t fadeAlpha = 0, int extra = 0)
    {
        sprite(b, 0, 0, 640, 224, 0.0, 30, 40, 60, true); // the clear that starts a field: Z 0, always passes
        for (int row = 0; row < 12; ++row)
            for (int col = 0; col < 20; ++col)
            {
                const float x = col * 32.0f, y = row * 18.7f;
                const uint8_t r = static_cast<uint8_t>(col * 6), g = static_cast<uint8_t>(row * 10), bl = 0x50;
                if (triangles)
                {
                    triangle(b, x, y, x + 32, y, x, y + 18.7f, 1000.0, r, g, bl);
                    triangle(b, x + 32, y, x + 32, y + 18.7f, x, y + 18.7f, 1000.0, r, g, bl);
                }
                else
                    sprite(b, x, y, x + 32, y + 18.7f, 1000.0, r, g, bl);
            }
        for (int y = 0; y < 40; y += 2) // one GS row bright, one dark
            sprite(b, 0, static_cast<float>(y), 640, static_cast<float>(y + 1), 1500.0, 0xE0, 0xE0, 0xE0);
        triangle(b, 300, 100, 340, 100, 300, 130, 2000.0, 0xFF, 0xFF, 0xFF); // a white blob
        triangle(b, 340, 100, 340, 130, 300, 130, 2000.0, 0xFF, 0xFF, 0xFF);
        if (overlay)
            sprite(b, 500 + overlayDx, 190, 560 + overlayDx, 215, kOverlayZ, 0xFF, 0x10, 0x10); // opaque red, in front of everything
        if (fadeAlpha != 0)
            fade(b, fadeAlpha);
        if (extra == 1)
            blendedBox(b, 0, 0, 40, 30, kOverlayZ, 0xFF, 0xFF, 0xFF, 0x40, kBlendOver); // a translucent white box in the corner
        else if (extra == 2)
            blendedBox(b, 0, 0, 40, 30, kOverlayZ, 0x40, 0x40, 0x40, 0x80, kBlendAdd);  // an additive box in the corner
        else if (extra == 3)
            blendedBox(b, 0, 0, 100, 60, kOverlayZ, 0x20, 0x20, 0x20, 0x00, kBlendOver); // a box that is fully transparent (its alpha is 0)
    }

    struct Picture
    {
        std::vector<uint8_t> px; // 640 x 448 RGBA
        bool ok = false;
        const uint8_t *at(uint32_t x, uint32_t y) const { return &px[(static_cast<size_t>(y) * 640u + x) * 4u]; }
    };

    GSPresentationRequest makeRequest()
    {
        GSPresentationRequest r{};
        r.pmode = 0x8007;
        r.smode2 = 0x3;
        const uint64_t dispfb = 0ull | (10ull << 9);
        const uint64_t display = (636ull) | (32ull << 12) | (639ull << 32) | (223ull << 44);
        r.dispfb1 = r.dispfb2 = dispfb;
        r.display1 = r.display2 = display;
        return r;
    }

    // Draws the field twice (the backend reads the live parameters at each VBlank, so the field in which they changed is drawn without them),
    // marks the VBlank each time, and reads the picture of the second field.
    Picture picture(GSGpuBackend &gpu, bool overlay, bool triangles = true, uint8_t fadeAlpha = 0, int extra = 0)
    {
        const GSPresentationRequest request = makeRequest();
        drawField(gpu, overlay, triangles, 0.0f, fadeAlpha, extra);
        gpu.MarkPresentation(request);
        drawField(gpu, overlay, triangles, 0.0f, fadeAlpha, extra);
        gpu.MarkPresentation(request);
        Picture pic;
        PresentationFrame frame = gpu.Present(request);
        if (frame && frame.width == 640 && frame.height == 448)
        {
            pic.px = frame.pixels;
            pic.ok = true;
        }
        return pic;
    }

    // How many pixels of the box differ (any channel) between two pictures.
    uint32_t differing(const Picture &a, const Picture &b, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
    {
        uint32_t n = 0;
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = x0; x < x1; ++x)
                if (std::memcmp(a.at(x, y), b.at(x, y), 4) != 0)
                    ++n;
        return n;
    }

    // The variance of the green channel down a column, rows y0..y1.
    double columnVariance(const Picture &p, uint32_t x, uint32_t y0, uint32_t y1)
    {
        double sum = 0.0, sum2 = 0.0;
        for (uint32_t y = y0; y < y1; ++y)
        {
            const double v = p.at(x, y)[1];
            sum += v;
            sum2 += v * v;
        }
        const double n = static_cast<double>(y1 - y0);
        return sum2 / n - (sum / n) * (sum / n);
    }

    ps2x_postfx::Params neutral()
    {
        ps2x_postfx::Params p;
        p.enabled = true;
        p.bloomIntensity = 0.0f;
        p.tiltStrength = 0.0f;
        p.vignetteStrength = 0.0f;
        return p;
    }
}

void register_postfx_tests()
{
    MiniTest::Case("ScreenEffects", [](TestCase &tc)
    {

    tc.Run("the value table, the settings and the environment names agree", [](TestCase &t)
    {
        using namespace ps2x_postfx;
        t.IsTrue(values().size() == 17, "17 values");
        for (const Value &v : values())
        {
            const std::string key = std::string("fx_") + v.name;
            const ps2x_settings::Definition *def = ps2x_settings::find(key);
            t.IsTrue(def != nullptr, key + " is a setting");
            if (def)
            {
                t.Equals(std::string(def->envName), envNameOf(v.name), key + ": the environment variable");
                t.Equals(std::string(def->defaultValue), std::string(), key + ": no override by default");
            }
            t.IsTrue(v.min < v.max && v.step > 0.0f, key + ": a sensible range");
        }
        t.IsTrue(ps2x_settings::find("fx") && std::string(ps2x_settings::find("fx")->defaultValue) == "off", "off by default");
        t.IsTrue(ps2x_settings::find("fx_preset") && std::string(ps2x_settings::find("fx_preset")->envName) == "PS2X_FX_PRESET", "the preset");
    });

    tc.Run("presets: Off changes nothing, Subtle and Strong are within every range and Strong goes further", [](TestCase &t)
    {
        using namespace ps2x_postfx;
        const Params off = presetParams(Preset::Off), subtle = presetParams(Preset::Subtle), strong = presetParams(Preset::Strong);
        t.IsFalse(off.enabled, "Off is not enabled");
        t.IsTrue(subtle.enabled && strong.enabled, "Subtle and Strong are enabled");
        t.IsTrue(off.bloomIntensity == 0.0f && off.tiltStrength == 0.0f && off.vignetteStrength == 0.0f && off.exposure == 0.0f && off.contrast == 1.0f &&
                     off.saturation == 1.0f && off.temperature == 0.0f && off.lift == 0.0f && off.gamma == 1.0f && off.gain == 1.0f,
                 "Off has neutral numbers");
        for (const Value &v : values())
        {
            for (const Params &p : {off, subtle, strong})
            {
                const float x = p.*(v.member);
                t.IsTrue(x >= v.min && x <= v.max, std::string(v.name) + " is in range");
            }
        }
        t.IsTrue(strong.bloomIntensity > subtle.bloomIntensity && strong.tiltStrength > subtle.tiltStrength && strong.vignetteStrength > subtle.vignetteStrength,
                 "Strong is stronger");
        Preset parsed = Preset::Off;
        t.IsTrue(parsePreset("Strong", parsed) && parsed == Preset::Strong && parsePreset("off", parsed) && parsed == Preset::Off, "preset names parse");
        t.IsFalse(parsePreset("loud", parsed), "an unknown preset does not");
    });

    tc.Run("fromSettings reads the environment when the game has no settings store: on and off, the preset, a value, nonsense, a clamp", [](TestCase &t)
    {
        using namespace ps2x_postfx;
        ps2x_settings::resetForTest();
        clearFxVariables();
        t.IsFalse(fromSettings().enabled, "off by default");
        setEnvVar("PS2X_FX", "1");
        Params p = fromSettings();
        t.IsTrue(p.enabled, "PS2X_FX=1 turns it on");
        t.IsTrue(std::fabs(p.bloomIntensity - presetParams(Preset::Subtle).bloomIntensity) < 1e-6f, "with the Subtle values by default");
        setEnvVar("PS2X_FX_PRESET", "strong");
        t.IsTrue(std::fabs(fromSettings().bloomIntensity - presetParams(Preset::Strong).bloomIntensity) < 1e-6f, "PS2X_FX_PRESET=strong");
        setEnvVar("PS2X_FX_BLOOM_INTENSITY", "0.9");
        t.IsTrue(std::fabs(fromSettings().bloomIntensity - 0.9f) < 1e-6f, "one value from its own variable");
        setEnvVar("PS2X_FX_BLOOM_INTENSITY", "banana");
        t.IsTrue(std::fabs(fromSettings().bloomIntensity - presetParams(Preset::Strong).bloomIntensity) < 1e-6f, "nonsense is ignored");
        setEnvVar("PS2X_FX_BLOOM_INTENSITY", "99");
        t.IsTrue(std::fabs(fromSettings().bloomIntensity - 2.0f) < 1e-6f, "and a number out of range is clamped");
        setEnvVar("PS2X_FX_PRESET", "off");
        t.IsFalse(fromSettings().enabled, "the Off preset is off even with PS2X_FX=1");
        setEnvVar("PS2X_FX", "0");
        setEnvVar("PS2X_FX_PRESET", "subtle");
        t.IsFalse(fromSettings().enabled, "PS2X_FX=0 turns it off");
        clearFxVariables();
    });

    tc.Run("settings: the fx keys normalise, a blank override is not written, and the live parameters follow a change of a setting", [](TestCase &t)
    {
        using namespace ps2x_postfx;
        clearFxVariables();
        ps2x_settings::resetForTest();
        t.Equals(ps2x_settings::valueFromEnvironment("fx", "1").value_or("?"), std::string("on"), "PS2X_FX=1 means on");
        t.Equals(ps2x_settings::valueFromEnvironment("fx", "0").value_or("?"), std::string("off"), "PS2X_FX=0 means off");
        t.IsFalse(ps2x_settings::valueFromEnvironment("fx_preset", "loud").has_value(), "a preset name that does not exist");
        t.Equals(ps2x_settings::valueFromEnvironment("fx_gamma", " 1.250 ").value_or("?"), std::string("1.25"), "a number is written back short");
        t.IsFalse(ps2x_settings::valueFromEnvironment("fx_gamma", "nan").has_value(), "nan is no number");
        t.IsFalse(ps2x_settings::environmentFromValue("fx_gamma", "").has_value(), "a blank override has no environment value");
        t.Equals(ps2x_settings::environmentFromValue("fx", "on").value_or("?"), std::string("1"), "fx on is PS2X_FX=1");

        const fs::path dir = fs::temp_directory_path() / "ps2x_postfx_settings_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const std::string file = (dir / "settings.cfg").string();
        ps2x_settings::initialize(file);
        t.IsFalse(current().enabled, "off with an empty settings file");
        const uint64_t before = ps2x_settings::generation();
        t.IsTrue(ps2x_settings::set("fx", "on"), "the overlay turns it on");
        t.IsTrue(ps2x_settings::generation() != before, "that is a new generation");
        t.IsTrue(current().enabled, "and the live parameters follow it");
        Params live = current();
        live.bloomIntensity = 1.5f;
        setLive(live);
        t.IsTrue(std::fabs(current().bloomIntensity - 1.5f) < 1e-6f, "the debug panel's values stand until a setting changes");
        ps2x_settings::set("fx_preset", "strong");
        t.IsTrue(std::fabs(current().bloomIntensity - presetParams(Preset::Strong).bloomIntensity) < 1e-6f, "a change of a setting takes the settings back");

        // saving: only values that differ from the preset are written, the file stays tidy, and a reload gives the same look
        Params tuned = presetParams(Preset::Strong);
        tuned.bloomIntensity = 0.8f;
        tuned.vignetteStrength = 0.1f;
        tuned.enabled = true;
        t.Equals(saveToSettings(tuned), 2, "two values differ from the preset and are written");
        const std::string text = readAll(file);
        t.IsTrue(text.find("fx_bloom_intensity = 0.8") != std::string::npos && text.find("fx_vignette_strength = 0.1") != std::string::npos, "they are in the file");
        t.IsTrue(text.find("fx_exposure") == std::string::npos, "the others are not");
        t.IsTrue(text.find("fx = on") != std::string::npos && text.find("fx_preset = strong") != std::string::npos, "with the switch and the preset");
        t.IsTrue(std::fabs(fromSettings().bloomIntensity - 0.8f) < 1e-6f && std::fabs(fromSettings().vignetteStrength - 0.1f) < 1e-6f, "and fromSettings sees them");
        ps2x_settings::resetForTest();
        clearFxVariables();
        ps2x_settings::initialize(file);
        t.IsTrue(std::fabs(fromSettings().bloomIntensity - 0.8f) < 1e-6f && fromSettings().enabled, "a fresh start reads the file back");
        ps2x_settings::resetForTest();
        clearFxVariables();
        fs::remove_all(dir, ec);
    });

    tc.Run("the effect chain on a synthetic match field: neutral changes nothing, the overlay is kept, a 2D field is left alone", [](TestCase &t)
    {
        using namespace ps2x_postfx;
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
            if (!SDL_Init(SDL_INIT_VIDEO))
            {
                std::cout << "    (skipped: no SDL video driver)" << std::endl;
                return;
            }
        }
        const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
        SDL_GPUDevice *device = SDL_CreateGPUDevice(formats, false, nullptr);
        if (!device)
        {
            std::cout << "    (skipped: no GPU device: " << SDL_GetError() << ")" << std::endl;
            return;
        }
        {
            ps2x_settings::resetForTest();
            clearFxVariables();
            GSMem::InitLookupTables();
            std::vector<uint8_t> vram(GSMem::MEMORY_SIZE, 0);
            GSGpuBackend gpu(device, 1, GSGpuBackend::FieldMode::Progressive);
            t.IsTrue(gpu.Ready() && gpu.SetFieldPublishing(true), "the progressive GPU renderer sets up and publishes fields");
            if (gpu.Ready() && gpu.FieldPublishing())
            {
                gpu.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
                Params off;
                off.enabled = false;
                setLive(off);
                const Picture base = picture(gpu, true);
                t.IsTrue(base.ok, "the picture of a field without effects");
                if (base.ok)
                {
                    // the overlay box is in the picture as the game drew it
                    t.IsTrue(base.at(520, 400)[0] > 200 && base.at(520, 400)[1] < 60, "the overlay is opaque red");

                    setLive(neutral());
                    const Picture same = picture(gpu, true);
                    t.IsTrue(same.ok && differing(base, same, 0, 0, 640, 448) == 0, "neutral numbers through the chain give the same picture, byte for byte");

                    Params vig = neutral();
                    vig.vignetteStrength = 1.0f;
                    vig.vignetteRadius = 0.2f;
                    vig.vignetteSoftness = 0.4f;
                    setLive(vig);
                    const Picture v = picture(gpu, true);
                    t.IsTrue(v.ok && v.at(4, 4)[2] + 20 < base.at(4, 4)[2], "a vignette darkens the corner of the scene");
                    t.IsTrue(v.ok && std::memcmp(v.at(320, 224), base.at(320, 224), 4) == 0, "and leaves the centre");
                    t.IsTrue(v.ok && differing(base, v, 500, 380, 560, 430) == 0, "the overlay box is exactly what the game drew");
                    t.IsTrue(v.ok && differing(base, v, 0, 0, 640, 448) > 50000, "and the scene around it changed");

                    Params tilt = neutral();
                    tilt.tiltStrength = 3.0f;
                    setLive(tilt);
                    const Picture ts = picture(gpu, true);
                    t.IsTrue(ts.ok && columnVariance(ts, 100, 20, 60) < 0.5 * columnVariance(base, 100, 20, 60), "tilt-shift blurs the stripes far from the focus band");
                    t.IsTrue(ts.ok && differing(base, ts, 0, 200, 640, 290) == 0, "rows in the focus band are untouched");
                    t.IsTrue(ts.ok && differing(base, ts, 500, 380, 560, 430) == 0, "the overlay box is untouched");

                    Params bloom = neutral();
                    bloom.bloomThreshold = 0.5f;
                    bloom.bloomIntensity = 1.0f;
                    setLive(bloom);
                    const Picture bl = picture(gpu, true);
                    t.IsTrue(bl.ok && bl.at(346, 230)[1] > base.at(346, 230)[1] + 2, "bloom makes a glow next to the bright blob");
                    t.IsTrue(bl.ok && differing(base, bl, 500, 380, 560, 430) == 0, "the overlay box is untouched");

                    // a fade (a translucent black layer over everything) goes through the effects: it does not leave the picture unprocessed
                    setLive(off);
                    const Picture fadeOff = picture(gpu, false, true, 0x40);
                    setLive(vig);
                    const Picture faded = picture(gpu, false, true, 0x40);
                    t.IsTrue(faded.ok && fadeOff.ok && fadeOff.at(4, 4)[2] > 30 && faded.at(4, 4)[2] < 8, "a half fade still has the vignette's dark corner (the game's own pixel would be 40)");
                    t.IsTrue(faded.ok && fadeOff.ok && std::abs(int(faded.at(320, 224)[1]) - int(fadeOff.at(320, 224)[1])) <= 3, "and the same fade at the centre");

                    // The overlay is drawn over the processed scene with the game's own blend, whatever the blend is.
                    setLive(off);
                    const Picture glassOff = picture(gpu, false, true, 0, 1);
                    const Picture addOff = picture(gpu, false, true, 0, 2);
                    setLive(vig);
                    const Picture glass = picture(gpu, false, true, 0, 1);
                    const Picture added = picture(gpu, false, true, 0, 2);
                    t.IsTrue(glass.ok && glassOff.ok && glassOff.at(4, 4)[2] > 150 && std::abs(int(glass.at(4, 4)[2]) - 127) <= 3,
                             "a translucent white box over the vignette's black corner is half white (127), not the scene under it (the game's own pixel is 167)");
                    t.IsTrue(added.ok && addOff.ok && addOff.at(4, 4)[2] > 130 && std::abs(int(added.at(4, 4)[2]) - 64) <= 3,
                             "an additive box over the black corner adds its 64 to the processed scene (the game's own pixel is 144)");
                    // a box of fully transparent texels changes no colour, so it must leave no mark in the processed picture (fx1: it did, the mask saw the alpha)
                    setLive(vig);
                    const Picture ghost = picture(gpu, false, true, 0, 3);
                    setLive(off);
                    const Picture ghostOff = picture(gpu, false, true, 0, 3);
                    setLive(vig);
                    const Picture bare = picture(gpu, false, true, 0, 0);
                    t.IsTrue(ghost.ok && bare.ok && ghostOff.ok && differing(bare, ghost, 0, 0, 640, 448) == 0, "a transparent box leaves no rectangle: the picture equals the one without it");
                    t.IsTrue(ghost.ok && ghostOff.ok && differing(ghostOff, ghost, 0, 0, 640, 448) > 50000, "and the scene is processed everywhere, the box included");

                    // a field with no overlay is all scene: the effects reach the whole picture
                    setLive(vig);
                    const Picture noOverlayOff = [&]()
                    {
                        setLive(off);
                        return picture(gpu, false);
                    }();
                    setLive(vig);
                    const Picture noOverlay = picture(gpu, false);
                    t.IsTrue(noOverlay.ok && noOverlayOff.ok && differing(noOverlayOff, noOverlay, 500, 380, 560, 430) > 1000, "a field with no overlay is processed everywhere");

                    // a 2D field (no triangle lists) is not a match scene: copied as it always was, whatever the effects
                    Params strong = presetParams(Preset::Strong);
                    setLive(off);
                    const Picture flatOff = picture(gpu, true, false);
                    setLive(strong);
                    const Picture flat = picture(gpu, true, false);
                    t.IsTrue(flat.ok && flatOff.ok && differing(flatOff, flat, 0, 0, 640, 448) == 0, "a field made of sprites only is left alone");

                    // and the effects turn off again at once
                    setLive(off);
                    const Picture again = picture(gpu, true);
                    t.IsTrue(again.ok && differing(base, again, 0, 0, 640, 448) == 0, "off again gives the original picture");
                }
            }
            ps2x_postfx::Status &status = ps2x_postfx::status();
            t.IsTrue(status.fieldsWithEffects.load() > 0 && status.fieldsPlain.load() > 0, "the status counts both kinds of field");
            t.IsFalse(status.unavailable.load(), "and the effects were available");
        }
        SDL_DestroyGPUDevice(device);
        setLive(Params());
    });
    });
}
