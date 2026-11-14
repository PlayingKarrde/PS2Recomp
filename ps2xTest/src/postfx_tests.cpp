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
    constexpr uint8_t kBarRed[8] = {150, 170, 190, 210, 225, 238, 248, 255}; // drawField(extra 4): the red of each bar

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
        else if (extra == 4)
            for (int i = 0; i < 8; ++i) // eight warm bars of rising brightness, the top ones as bright as a lit shoulder or a highlight on the mat
            {
                const uint8_t r = kBarRed[i];
                sprite(b, 120.0f + 20.0f * i, 110.0f, 140.0f + 20.0f * i, 150.0f, 1500.0, r, static_cast<uint8_t>(r * 0.82f), static_cast<uint8_t>(r * 0.60f));
            }
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

    // The red, green and blue of the pixel at the centre of bar i (drawField with extra 4: the bars are 20 wide from x = 120, at GS row 130).
    const uint8_t *barPixel(const Picture &p, int i)
    {
        return p.at(130u + 20u * static_cast<uint32_t>(i), 260);
    }

    double toLinearLevel(int v)
    {
        const double c = v / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
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
        t.IsTrue(values().size() == 18, "18 values");
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
        for (const char *name : {"bloom", "grading", "tiltshift", "vignette"})
        {
            const std::string key = std::string("fx_") + name;
            const ps2x_settings::Definition *def = ps2x_settings::find(key);
            t.IsTrue(def != nullptr, key + " is a setting (the switch of one effect)");
            if (def)
            {
                t.Equals(std::string(def->envName), envNameOf(name), key + ": the environment variable");
                t.Equals(std::string(def->defaultValue), std::string("on"), key + ": on by default");
            }
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
                     off.saturation == 1.0f && off.temperature == 0.0f && off.lift == 0.0f && off.gamma == 1.0f && off.gain == 1.0f &&
                     off.highlightStart == 1.0f,
                 "Off has neutral numbers");
        for (const Params &p : {off, subtle, strong})
            t.IsTrue(p.bloomOn && p.gradingOn && p.tiltOn && p.vignetteOn, "a preset never sets which effects are on: all four stay on");
        t.IsTrue(subtle.highlightStart < 1.0f && strong.highlightStart < 1.0f && strong.highlightStart <= subtle.highlightStart,
                 "Subtle and Strong roll their highlights off, Strong from lower down");
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
        t.IsTrue(p.bloomOn && p.gradingOn && p.tiltOn && p.vignetteOn && anyEffectOn(p), "and all four effects on");
        setEnvVar("PS2X_FX_BLOOM", "0");
        p = fromSettings();
        t.IsTrue(!p.bloomOn && p.gradingOn && p.tiltOn && p.vignetteOn, "PS2X_FX_BLOOM=0 turns the bloom off and nothing else");
        setEnvVar("PS2X_FX_BLOOM", "1");
        setEnvVar("PS2X_FX_TILTSHIFT", "off");
        setEnvVar("PS2X_FX_GRADING", "no");
        setEnvVar("PS2X_FX_VIGNETTE", "false");
        p = fromSettings();
        t.IsTrue(p.bloomOn && !p.gradingOn && !p.tiltOn && !p.vignetteOn && anyEffectOn(p), "the others turn off by their own variables; the bloom is on again with 1");
        setEnvVar("PS2X_FX_BLOOM", "0");
        t.IsFalse(anyEffectOn(fromSettings()), "with all four off no effect is on");
        t.IsTrue(fromSettings().enabled, "while the master switch is still on");
        setEnvVar("PS2X_FX_BLOOM", nullptr);
        setEnvVar("PS2X_FX_TILTSHIFT", nullptr);
        setEnvVar("PS2X_FX_GRADING", nullptr);
        setEnvVar("PS2X_FX_VIGNETTE", nullptr);
        setEnvVar("PS2X_FX_BLOOM", "banana");
        t.IsTrue(fromSettings().bloomOn, "nonsense leaves a switch on");
        setEnvVar("PS2X_FX_BLOOM", nullptr);
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
        t.Equals(ps2x_settings::valueFromEnvironment("fx_bloom", "0").value_or("?"), std::string("off"), "PS2X_FX_BLOOM=0 means off");
        t.Equals(ps2x_settings::valueFromEnvironment("fx_vignette", "1").value_or("?"), std::string("on"), "PS2X_FX_VIGNETTE=1 means on");
        t.IsFalse(ps2x_settings::valueFromEnvironment("fx_grading", "maybe").has_value(), "a switch that is neither on nor off is refused");
        t.Equals(ps2x_settings::environmentFromValue("fx_tiltshift", "off").value_or("?"), std::string("0"), "a switch that is off is PS2X_FX_TILTSHIFT=0");
        t.IsFalse(ps2x_settings::environmentFromValue("fx_tiltshift", "on").has_value(), "and one that is on is the default: no variable");
        t.Equals(ps2x_settings::valueFromEnvironment("fx_bloom_threshold", " 0.5 ").value_or("?"), std::string("0.5"), "the bloom's number is still a number, not a switch");
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
        tuned.tiltOn = false; // a switch, saved with the values
        t.Equals(saveToSettings(tuned), 2, "two values differ from the preset and are written");
        const std::string text = readAll(file);
        t.IsTrue(text.find("fx_bloom_intensity = 0.8") != std::string::npos && text.find("fx_vignette_strength = 0.1") != std::string::npos, "they are in the file");
        t.IsTrue(text.find("fx_exposure") == std::string::npos, "the others are not");
        t.IsTrue(text.find("fx = on") != std::string::npos && text.find("fx_preset = strong") != std::string::npos, "with the switch and the preset");
        t.IsTrue(text.find("fx_tiltshift = off") != std::string::npos && text.find("fx_bloom = on") != std::string::npos, "and the switches of the single effects");
        t.IsTrue(!fromSettings().tiltOn && fromSettings().bloomOn, "which fromSettings reads back");
        t.IsTrue(std::fabs(fromSettings().bloomIntensity - 0.8f) < 1e-6f && std::fabs(fromSettings().vignetteStrength - 0.1f) < 1e-6f, "and fromSettings sees them");
        ps2x_settings::resetForTest();
        clearFxVariables();
        ps2x_settings::initialize(file);
        t.IsTrue(std::fabs(fromSettings().bloomIntensity - 0.8f) < 1e-6f && fromSettings().enabled && !fromSettings().tiltOn, "a fresh start reads the file back");
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

                    // Highlights (fx2). Light that adds up past white used to be cut: warm bars that are already bright come out as one flat
                    // colour. With the shoulder the bars stay apart and keep their hue; below its start nothing changes.
                    setLive(off);
                    const Picture barsOff = picture(gpu, false, true, 0, 4);
                    Params hot = neutral();
                    hot.bloomThreshold = 0.5f;
                    hot.bloomIntensity = 0.4f;
                    hot.exposure = 0.25f; // more light than the picture can show
                    setLive(hot);
                    const Picture barsCut = picture(gpu, false, true, 0, 4);
                    hot.highlightStart = 0.7f;
                    setLive(hot);
                    const Picture barsSoft = picture(gpu, false, true, 0, 4);
                    Params shoulderOnly = neutral();
                    shoulderOnly.highlightStart = 0.7f;
                    setLive(shoulderOnly);
                    const Picture barsShoulder = picture(gpu, false, true, 0, 4);
                    t.IsTrue(barsOff.ok && barsCut.ok && barsSoft.ok && barsShoulder.ok, "the pictures of the bars");
                    if (barsOff.ok && barsCut.ok && barsSoft.ok && barsShoulder.ok)
                    {
                        t.IsTrue(barPixel(barsOff, 7)[0] == 255 && barPixel(barsOff, 3)[0] == 210, "the bars are what the game drew");
                        t.IsTrue(barPixel(barsCut, 5)[0] == 255 && barPixel(barsCut, 6)[0] == 255 && barPixel(barsCut, 7)[0] == 255,
                                 "cut at white (no shoulder): the three brightest bars are the same flat red");
                        const double ratioOff = toLinearLevel(barPixel(barsOff, 7)[1]) / toLinearLevel(barPixel(barsOff, 7)[0]);
                        const double ratioCut = toLinearLevel(barPixel(barsCut, 7)[1]) / toLinearLevel(barPixel(barsCut, 7)[0]);
                        const double ratioSoft = toLinearLevel(barPixel(barsSoft, 7)[1]) / toLinearLevel(barPixel(barsSoft, 7)[0]);
                        t.IsTrue(std::fabs(ratioCut - ratioOff) > 0.2 * ratioOff, "and the cut shifts the colour (green catches up with red)");
                        // The cut turns bars 2 to 7 into one red; the shoulder keeps them apart (it squeezes the brightest together, so some neighbours
                        // are one level apart, but the order is kept and the whole run is not one value).
                        auto distinct = [](const Picture &pic)
                        {
                            std::vector<int> v;
                            for (int i = 2; i < 8; ++i)
                                v.push_back(barPixel(pic, i)[0]);
                            std::sort(v.begin(), v.end());
                            return static_cast<int>(std::unique(v.begin(), v.end()) - v.begin());
                        };
                        bool ordered = true;
                        for (int i = 1; i < 8; ++i)
                            ordered = ordered && barPixel(barsSoft, i)[0] >= barPixel(barsSoft, i - 1)[0];
                        const bool apart = distinct(barsCut) == 1 && distinct(barsSoft) >= 4 && ordered && barPixel(barsSoft, 7)[0] < 255;
                        if (!apart)
                        {
                            std::cout << "    bars, red of each (off / cut / soft):";
                            for (int i = 0; i < 8; ++i)
                                std::cout << " " << int(barPixel(barsOff, i)[0]) << "/" << int(barPixel(barsCut, i)[0]) << "/" << int(barPixel(barsSoft, i)[0]);
                            std::cout << std::endl;
                        }
                        t.IsTrue(apart, "with the shoulder the bars are still told apart, in order, and none is flat white");
                        t.IsTrue(std::fabs(ratioSoft - ratioOff) < 0.1 * ratioOff, "and the colour keeps its hue (green over red in linear light is what it was)");
                        bool lowBarsSame = true;
                        for (int i = 0; i < 4; ++i)
                            lowBarsSame = lowBarsSame && std::memcmp(barPixel(barsOff, i), barPixel(barsShoulder, i), 3) == 0;
                        t.IsTrue(lowBarsSame, "below the start of the shoulder nothing changes, byte for byte");
                        t.IsTrue(barPixel(barsShoulder, 7)[0] < 250 && barPixel(barsShoulder, 7)[0] > 220, "and white itself goes a little down: it is the top of the roll-off");
                    }

                    // The single effects (fx2): an effect that is off does none of its work. Checked three ways: the render passes the chain
                    // recorded (the bloom is 2 x levels - 1 of them, the tilt-shift 2, the final pass 1), the picture against the same look with that
                    // effect's strength at zero, and the picture with all four off against the effects-off picture.
                    {
                        Status &status = ps2x_postfx::status();
                        auto run = [&](const Params &p)
                        {
                            setLive(p);
                            Picture pic = picture(gpu, true);
                            return std::make_pair(std::move(pic), status.lastPasses.load());
                        };
                        const Params all = presetParams(Preset::Strong);
                        const auto [picAll, passesAll] = run(all);
                        t.IsTrue(picAll.ok && passesAll >= 6, "Strong with all four on: the chain recorded its passes (" + std::to_string(passesAll) + ")");

                        Params noBloom = all;
                        noBloom.bloomOn = false;
                        Params zeroBloom = all;
                        zeroBloom.bloomIntensity = 0.0f;
                        const auto [picNoBloom, passesNoBloom] = run(noBloom);
                        const auto [picZeroBloom, passesZeroBloom] = run(zeroBloom);
                        t.IsTrue(passesNoBloom < passesAll && passesNoBloom == passesZeroBloom && differing(picNoBloom, picAll, 0, 0, 640, 448) > 1000,
                                 "bloom off: its down and up passes are not recorded, and the picture has no glow");
                        t.IsTrue(differing(picNoBloom, picZeroBloom, 0, 0, 640, 448) == 0, "and is the same picture as the look with the bloom's strength at zero");

                        Params noTilt = all;
                        noTilt.tiltOn = false;
                        Params zeroTilt = all;
                        zeroTilt.tiltStrength = 0.0f;
                        const auto [picNoTilt, passesNoTilt] = run(noTilt);
                        const auto [picZeroTilt, passesZeroTilt] = run(zeroTilt);
                        t.IsTrue(passesNoTilt + 2 == passesAll && passesNoTilt == passesZeroTilt, "tilt-shift off: its two passes are not recorded");
                        t.IsTrue(differing(picNoTilt, picZeroTilt, 0, 0, 640, 448) == 0 && differing(picNoTilt, picAll, 0, 0, 640, 448) > 1000,
                                 "and the picture is the one with the blur's strength at zero");

                        Params noVig = all;
                        noVig.vignetteOn = false;
                        Params zeroVig = all;
                        zeroVig.vignetteStrength = 0.0f;
                        const auto [picNoVig, passesNoVig] = run(noVig);
                        const auto [picZeroVig, passesZeroVig] = run(zeroVig);
                        (void)passesZeroVig;
                        t.IsTrue(passesNoVig == passesAll && differing(picNoVig, picZeroVig, 0, 0, 640, 448) == 0 && differing(picNoVig, picAll, 0, 0, 640, 448) > 1000,
                                 "vignette off: same passes (it is a step of the final pass), the picture of a vignette of strength zero");

                        Params noGrade = all;
                        noGrade.gradingOn = false;
                        Params neutralGrade = all; // the bloom is on in both, so the highlight roll-off is too
                        neutralGrade.exposure = 0.0f;
                        neutralGrade.contrast = 1.0f;
                        neutralGrade.saturation = 1.0f;
                        neutralGrade.temperature = 0.0f;
                        neutralGrade.lift = 0.0f;
                        neutralGrade.gamma = 1.0f;
                        neutralGrade.gain = 1.0f;
                        const auto [picNoGrade, passesNoGrade] = run(noGrade);
                        const auto [picNeutralGrade, passesNeutralGrade] = run(neutralGrade);
                        t.IsTrue(passesNoGrade == passesAll && passesNeutralGrade == passesAll && differing(picNoGrade, picNeutralGrade, 0, 0, 640, 448) == 0 &&
                                     differing(picNoGrade, picAll, 0, 0, 640, 448) > 1000,
                                 "grading off: the picture of neutral grading numbers (the glow and its roll-off stay), the same passes");

                        // each effect alone
                        Params only = all;
                        only.bloomOn = only.gradingOn = only.tiltOn = only.vignetteOn = false;
                        Params bloomOnly = only, tiltOnly = only, vigOnly = only, gradeOnly = only;
                        bloomOnly.bloomOn = true;
                        tiltOnly.tiltOn = true;
                        vigOnly.vignetteOn = true;
                        gradeOnly.gradingOn = true;
                        const auto [picBloomOnly, passesBloomOnly] = run(bloomOnly);
                        const auto [picTiltOnly, passesTiltOnly] = run(tiltOnly);
                        const auto [picVigOnly, passesVigOnly] = run(vigOnly);
                        const auto [picGradeOnly, passesGradeOnly] = run(gradeOnly);
                        t.IsTrue(passesBloomOnly + 2 == passesAll && passesTiltOnly == 3 && passesVigOnly == 1 && passesGradeOnly == 1,
                                 "each effect alone records only its own passes (and the final one): bloom, tilt-shift 2 + 1, vignette 1, grading 1");
                        setLive(off);
                        const Picture plain = picture(gpu, true);
                        t.IsTrue(picVigOnly.ok && plain.ok && std::memcmp(picVigOnly.at(320, 224), plain.at(320, 224), 4) == 0 && picVigOnly.at(4, 4)[2] + 20 < plain.at(4, 4)[2],
                                 "the vignette alone darkens the corner and leaves the centre byte for byte (no grading, no roll-off)");
                        t.IsTrue(differing(picVigOnly, plain, 0, 0, 640, 448) > 1000 && differing(picVigOnly, plain, 150, 100, 490, 350) == 0,
                                 "and leaves the middle of the picture alone");
                        t.IsTrue(differing(picTiltOnly, plain, 0, 200, 640, 290) == 0 && differing(picTiltOnly, plain, 0, 0, 640, 100) > 1000,
                                 "the tilt-shift alone blurs far from the focus band and does nothing else (the focus band is byte for byte the plain picture)");
                        t.IsTrue(differing(picGradeOnly, plain, 0, 0, 640, 448) > 50000 && picGradeOnly.at(4, 4)[2] > picAll.at(4, 4)[2],
                                 "the grading alone changes the picture and does not darken the corner");
                        t.IsTrue(differing(picBloomOnly, plain, 0, 0, 640, 448) > 100 && differing(picBloomOnly, plain, 0, 380, 100, 448) == 0,
                                 "the bloom alone adds only its glow near the bright parts (the far corner is the plain picture)");

                        // all four off with the master switch on: the field takes the plain path, byte for byte the effects-off picture
                        Params none = all;
                        none.bloomOn = none.gradingOn = none.tiltOn = none.vignetteOn = false;
                        t.IsFalse(anyEffectOn(none), "no effect is on");
                        const uint64_t chainFieldsBefore = status.fieldsWithEffects.load();
                        setLive(none);
                        const Picture picNone = picture(gpu, true);
                        t.IsTrue(picNone.ok && differing(picNone, plain, 0, 0, 640, 448) == 0, "all four off, master on: the picture is the effects-off picture, byte for byte");
                        t.IsTrue(status.fieldsWithEffects.load() == chainFieldsBefore, "because the chain did not run at all: the field takes the plain path (nothing tracked, nothing drawn twice)");
                    }

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
