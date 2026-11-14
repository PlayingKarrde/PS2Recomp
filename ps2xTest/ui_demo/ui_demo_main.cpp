// ps2x_ui_demo: the RmlUi overlay (ps2xRuntime/src/lib/ui) over a stand-in game picture, on SDL_GPU.
//
//   ps2x_ui_demo [--driver vulkan|direct3d12|metal] [--debug] [--size WxH] [--scale S] [--assets DIR]
//                [--backdrop FILE.png] [--aspect W:H] [--png FILE] [--png-dir DIR] [--script STEPS]
//                [--expect key=value,...] [--window | --headless] [--frames N]
//
// What it shows: a stand-in for the game's picture (a 4:3 box, pillarboxed in the window, as the host draws it) and,
// over it, the settings page (assets/pages/settings.rml) in the game-like theme (assets/theme/fpr.rcss). It uses the
// same two steps the host will: one render pass with the picture, then Overlay::record() as a second pass over it.
//
// Modes
//   (default)     a window, until it is closed. Mouse, keyboard and a game controller all work (D-pad or left stick
//                 to move, A to change a value or press a button, B for Back, LB/RB to switch tabs).
//   --headless    no window: the frame goes into an offscreen texture. With --png FILE one frame is written and the
//                 program exits. This is what runs without a person at the screen.
//   --script      scripted input, a list separated by commas, one step each, run through the same SDL event path as
//                 real input. After each step a few frames are drawn and, with --png-dir, a PNG is written
//                 (NN_step.png; 00_start.png first). Steps:
//                   up down left right a b lb rb       controller D-pad and buttons
//                   lsup lsdown lsleft lsright         left stick pushed and released
//                   key:NAME                           a keyboard key by its SDL name (Tab, Return, Escape, ...)
//                   move:X,Y   click:X,Y               mouse, in window pixels
//                   move:@SEL  click:@SEL              mouse, at the middle of the first element that matches an RCSS selector
//                                                      (click:@#quit, click:@.tab.selected; "panel" is the content box showing;
//                                                      "SEL|3" is the third match, click:@.mod|3)
//                   drag:X0,Y0,X1,Y1                   press, move and release the left button (sliders)
//                   wait:N                             N more frames
//                 Real controllers are ignored while a script runs.
//   --expect      after the script, compare setting values (aspect, resolution, brightness...) and exit 1 if any differ. A value
//                 that starts with ~ only has to be contained in what the menu says (a help line or status line).
//   --validation-selftest   draws one deliberately invalid scissor rectangle (x = -8) each frame. With --debug the
//                 validation layer must report it, which shows that a clean --debug run means something.
//   --virtual-pad drive the script's controller steps through an SDL virtual gamepad (SDL_AttachVirtualJoystick), so
//                 the events come out of SDL's own gamepad layer, not pushed by this program.
//   --page FILE   show another page of the asset folder (default pages/settings.rml) without the settings behaviour.
//   --menu        show the game's settings menu (pages/game_settings.rml, ui_game_menu.h) with a stand-in for the game side,
//                 instead of the demo page. --set k=v,... gives starting values, --startup k=v,... what the "running game"
//                 started with (a different current value then shows RESTART), --env-lock k=NAME,... settings fixed by an
//                 environment variable. --expect then checks the stand-in's values. --quit-blocked TEXT dims Quit Game with
//                 TEXT as the reason (the host does this while the game writes the memory card). --card-line TEXT sets the Mods tab's
//                 memory card line, and --card-buttons keep,back shows the Keep Card and Back to My Card buttons,
//                 --dev-line TEXT the Mods tab's dev pack line (the modder's buttons show with it). --expect also understands mods-card
//                 (its text), back-card and keep-card (shown or hidden) and back-asked (1 once the Back to My Card question was answered Yes). Besides the setting
//                 values, --expect understands focus (the id of the focused element), confirm (shown or hidden: the
//                 Quit Game question), quit (asked, once Quit was confirmed), quit-locked (1 or empty), help (the
//                 help line's text) and tab (the selected tab). Task u3: rect:SELECTOR (the element's box in window pixels, "x y w h",
//                 or (none); for example rect:#header, rect:panel) and scale (the overlay's UI scale).
//   --mods-dir DIR [--mods-state FILE]   with --menu: give the menu a mod manager on that mods folder (src/lib/mods), so the
//                 Mods tab is there. The state file (default DIR/../mods.toml) decides which mods are on; a theme mod that is on in
//                 it is applied, as at a game start. --expect then also understands mods-on, mods-active, mods-order (comma
//                 lists of mod ids), mods-restart (1 or 0), mods-status, selected-mod, mod-title, mod-problem, mod-confirm,
//                 restart-note and mods-count. `drop:PATH` as a script step installs a file as a drop on the window would.
//                 Task s4b: --card-buttons import shows the Import a Save... button (--expect import-save=shown, import-asked=1 once it
//                 was pressed); the script step `pick:LABEL|LABEL|LABEL` shows the "Which save?" question with those buttons, and
//                 --expect picked=N says which one was answered (-1 before an answer).
//                 Task h1: --load-now none|waiting|pending|pending-unsaved|pending-wrongplace|note:TEXT sets what the stand-in host offers on
//                 the Mods tab's Load Now button (waiting: the game has not paused yet; pending: enabled; pending-unsaved: enabled and the
//                 game holds unsaved changes; pending-wrongplace: dimmed, "Go to the title screen or the main menu."; note:TEXT only the line),
//                 --restart-blocked TEXT dims Restart Now, --no-restart removes the button (a platform that cannot start the game again).
//                 --expect understands load-now and restart-now (shown or hidden), load-now-locked and restart-now-locked (1 or empty),
//                 mods-apply (shown or hidden), mods-load-note (its text), load-asked and restart-asked (how many times the host was asked),
//                 mod-confirm-title and mod-confirm-text (the Mods tab's question).
//   --window      with --script or --png: open a window anyway, so the frames go through the swapchain. The PNG is
//                 then taken from a second, offscreen draw of the same frame.
//
// Exit code: 0 ok, 1 a check failed or the UI logged an error, 2 setup failed.

#include "../common/png_write.h"

#if defined(PS2X_DEMO_MODS)
#include "mod_manager.h"
#endif
#include "ui_game_menu.h"
#include "ui_overlay.h"
#include "ui_render_sdlgpu.h"
#include "ui_settings.h"

#include <RmlUi/Core.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace
{
    using namespace ps2x::ui;
    namespace fs = std::filesystem;

    // The stand-in picture's size, the PS2's 640x448.
    constexpr int kPicW = 640;
    constexpr int kPicH = 448;
    constexpr float kSettleFrames = 4;

    struct Options
    {
        const char *driver = nullptr;
        bool debug = false;
        int width = 1280;
        int height = 720;
        float scale = 0.0f; // 0 = the overlay's automatic scale (height / 720)
        std::string assets;
        std::string backdrop;
        std::string aspect = "4:3";
        std::string png;
        std::string pngDir;
        std::string script;
        std::string expect;
        bool headless = false;
        bool forceWindow = false;
        bool breakScissor = false; // --validation-selftest
        bool virtualPad = false;   // --virtual-pad
        bool menu = false;         // --menu
        std::string menuSet, menuStartup, menuEnvLock, quitBlocked, cardLine, cardButtons, devLine, loadNow, restartBlocked;
        bool noRestart = false;
        std::string modsDir, modsState;
        std::string page = "pages/settings.rml";
        int frames = 0;
    };

    [[noreturn]] void die(const char *what)
    {
        std::fprintf(stderr, "FATAL: %s: %s\n", what, SDL_GetError());
        std::exit(2);
    }

    // ---- The stand-in picture: a golden lattice like the game's menu background, built here (no game data) -------------

    std::vector<uint8_t> makeStandInPicture()
    {
        std::vector<uint8_t> px(static_cast<size_t>(kPicW) * kPicH * 4);
        for (int y = 0; y < kPicH; ++y)
        {
            for (int x = 0; x < kPicW; ++x)
            {
                const float v = static_cast<float>(y) / kPicH;
                float r = 0.80f - 0.30f * v;
                float g = 0.62f - 0.28f * v;
                float b = 0.14f - 0.06f * v;
                // Diamond lattice, drawn as pale grey lines.
                const float d1 = std::fabs(std::fmod(static_cast<float>(x + y), 56.0f) - 28.0f);
                const float d2 = std::fabs(std::fmod(static_cast<float>(x - y + 4000), 56.0f) - 28.0f);
                const float line = std::min(d1, d2) < 3.0f ? 1.0f : 0.0f;
                r += (0.78f - r) * line * 0.85f;
                g += (0.76f - g) * line * 0.85f;
                b += (0.70f - b) * line * 0.85f;
                // A darker band at the bottom and a bright patch, so the overlay sits on uneven content.
                const float dx = (x - 420.0f) / 150.0f;
                const float dy = (y - 140.0f) / 90.0f;
                const float glow = std::max(0.0f, 1.0f - (dx * dx + dy * dy));
                r += glow * 0.25f;
                g += glow * 0.25f;
                b += glow * 0.25f;
                uint8_t *p = &px[(static_cast<size_t>(y) * kPicW + x) * 4];
                p[0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
                p[1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
                p[2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
                p[3] = 255;
            }
        }
        return px;
    }

    // Loads a PNG or BMP as RGBA8 (for --backdrop), scaled to the stand-in picture's size with nearest sampling.
    bool loadBackdrop(const std::string &path, std::vector<uint8_t> &out)
    {
        SDL_Surface *s = SDL_LoadSurface(path.c_str());
        if (!s)
            return false;
        SDL_Surface *rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(s);
        if (!rgba)
            return false;
        out.assign(static_cast<size_t>(kPicW) * kPicH * 4, 255);
        for (int y = 0; y < kPicH; ++y)
        {
            const int sy = std::min(rgba->h - 1, y * rgba->h / kPicH);
            for (int x = 0; x < kPicW; ++x)
            {
                const int sx = std::min(rgba->w - 1, x * rgba->w / kPicW);
                const uint8_t *src = static_cast<const uint8_t *>(rgba->pixels) + static_cast<size_t>(sy) * rgba->pitch + sx * 4;
                std::memcpy(&out[(static_cast<size_t>(y) * kPicW + x) * 4], src, 4);
                out[(static_cast<size_t>(y) * kPicW + x) * 4 + 3] = 255;
            }
        }
        SDL_DestroySurface(rgba);
        return true;
    }

    // ---- The picture, drawn through the same renderer the overlay uses -----------------------------------------------------

    struct Picture
    {
        Rml::TextureHandle texture = 0;
        Rml::CompiledGeometryHandle quad = 0;
        // The unit quad's vertices and indices stay alive as long as the geometry does (RmlUi's rule for geometry).
        Rml::Vertex vertices[4];
        int indices[6] = {0, 1, 2, 0, 2, 3};
        Rml::CompiledGeometryHandle shade = 0; // a black quad, alpha set by the brightness setting
        Rml::Vertex shadeVertices[4];
        float brightness = 1.0f;
        float aspectW = 4.0f;
        float aspectH = 3.0f;
    };

    void createPicture(SdlGpuRenderer &r, Picture &p, const std::vector<uint8_t> &rgba)
    {
        // RmlUi's texture bytes are premultiplied; the picture is opaque, so they are the same.
        p.texture = r.GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), Rml::Vector2i(kPicW, kPicH));
        const Rml::ColourbPremultiplied white(255, 255, 255, 255);
        const Rml::Vector2f corners[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int i = 0; i < 4; ++i)
        {
            p.vertices[i] = {corners[i], white, corners[i]};
            p.shadeVertices[i] = {corners[i], Rml::ColourbPremultiplied(0, 0, 0, 255), {0, 0}};
        }
        p.quad = r.CompileGeometry(Rml::Span<const Rml::Vertex>(p.vertices, 4), Rml::Span<const int>(p.indices, 6));
        p.shade = r.CompileGeometry(Rml::Span<const Rml::Vertex>(p.shadeVertices, 4), Rml::Span<const int>(p.indices, 6));
    }

    // Draws the picture in the largest box of the chosen shape that fits the target, centred, with black around it
    // (the host's presenter does the same for the game's 4:3 box).
    void drawPicture(SdlGpuRenderer &r, const Picture &p, uint32_t w, uint32_t h)
    {
        float bw = static_cast<float>(w);
        float bh = bw * p.aspectH / p.aspectW;
        if (bh > static_cast<float>(h))
        {
            bh = static_cast<float>(h);
            bw = bh * p.aspectW / p.aspectH;
        }
        const float x = std::floor((static_cast<float>(w) - bw) * 0.5f);
        const float y = std::floor((static_cast<float>(h) - bh) * 0.5f);
        const Rml::Matrix4f box = Rml::Matrix4f::Translate(x, y, 0.0f) * Rml::Matrix4f::Scale(bw, bh, 1.0f);
        r.EnableScissorRegion(false);
        r.SetTransform(&box);
        r.RenderGeometry(p.quad, Rml::Vector2f(0, 0), p.texture);
        if (p.brightness < 1.0f)
        {
            // A black layer over the picture: its colour is premultiplied, so alpha 255 is opaque black. Scaling the
            // geometry's colour is not possible after compiling, so the layer's strength comes from drawing it with a
            // vertex colour set when the setting changes (see updateBrightness).
            r.RenderGeometry(p.shade, Rml::Vector2f(0, 0), 0);
        }
        r.SetTransform(nullptr);
    }

    void updateBrightness(SdlGpuRenderer &r, Picture &p, float brightness)
    {
        p.brightness = brightness;
        const uint8_t a = static_cast<uint8_t>(std::clamp((1.0f - brightness) * 255.0f, 0.0f, 255.0f));
        for (int i = 0; i < 4; ++i)
            p.shadeVertices[i].colour = Rml::ColourbPremultiplied(0, 0, 0, a);
        // The geometry is immutable once compiled; make a new one from the same arrays.
        r.ReleaseGeometry(p.shade);
        p.shade = r.CompileGeometry(Rml::Span<const Rml::Vertex>(p.shadeVertices, 4), Rml::Span<const int>(p.indices, 6));
    }

    // ---- Offscreen target and read-back ---------------------------------------------------------------------------------------

    SDL_GPUTexture *createTarget(SDL_GPUDevice *device, SDL_GPUTextureFormat format, int w, int h)
    {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = format;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = static_cast<Uint32>(w);
        info.height = static_cast<Uint32>(h);
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        return SDL_CreateGPUTexture(device, &info);
    }

    // Copies a B8G8R8A8 or R8G8B8A8 target into `rgba` (tightly packed, RGBA order). Submits `cmd` itself.
    bool downloadAndSubmit(SDL_GPUDevice *device, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int w, int h, bool bgra,
                           std::vector<uint8_t> &rgba)
    {
        SDL_GPUTransferBufferCreateInfo tbi = {};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tbi.size = static_cast<Uint32>(w * h * 4);
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device, &tbi);
        if (!tb)
            return false;
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion region = {};
        region.texture = target;
        region.w = static_cast<Uint32>(w);
        region.h = static_cast<Uint32>(h);
        region.d = 1;
        SDL_GPUTextureTransferInfo dst = {tb, 0, static_cast<Uint32>(w), static_cast<Uint32>(h)};
        SDL_DownloadFromGPUTexture(copy, &region, &dst);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (!fence)
        {
            SDL_ReleaseGPUTransferBuffer(device, tb);
            return false;
        }
        SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
        const uint8_t *map = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(device, tb, false));
        if (!map)
        {
            SDL_ReleaseGPUTransferBuffer(device, tb);
            return false;
        }
        rgba.assign(map, map + tbi.size);
        SDL_UnmapGPUTransferBuffer(device, tb);
        SDL_ReleaseGPUTransferBuffer(device, tb);
        if (bgra)
            for (size_t i = 0; i < rgba.size(); i += 4)
                std::swap(rgba[i], rgba[i + 2]);
        return true;
    }

    // ---- Script ----------------------------------------------------------------------------------------------------------------------

    std::vector<std::string> splitCommas(const std::string &text)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= text.size())
        {
            size_t end = text.find(',', start);
            // "click:100,200" and "drag:..." carry commas of their own: glue the number parts back on.
            if (end == std::string::npos)
                end = text.size();
            out.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        // Re-join pieces that are only numbers onto the step before them (x,y pairs of mouse steps).
        std::vector<std::string> joined;
        for (const std::string &piece : out)
        {
            const bool numeric = !piece.empty() && piece.find_first_not_of("0123456789-") == std::string::npos;
            if (numeric && !joined.empty() && joined.back().find(':') != std::string::npos)
                joined.back() += "," + piece;
            else if (!piece.empty())
                joined.push_back(piece);
        }
        return joined;
    }

    void pushEvent(SDL_Event e)
    {
        SDL_PushEvent(&e);
    }

    // With --virtual-pad the script drives an SDL virtual gamepad instead of pushing events: the events then come out
    // of SDL's own joystick and gamepad layers (button mapping, axis scaling), like a real controller's.
    SDL_Joystick *g_virtualJoystick = nullptr;

    void padButton(SDL_GamepadButton button, bool down)
    {
        if (g_virtualJoystick)
        {
            SDL_SetJoystickVirtualButton(g_virtualJoystick, static_cast<int>(button), down);
            return;
        }
        SDL_Event e = {};
        e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = 1;
        e.gbutton.button = static_cast<Uint8>(button);
        e.gbutton.down = down;
        pushEvent(e);
    }

    void padAxis(SDL_GamepadAxis axis, int value)
    {
        if (g_virtualJoystick)
        {
            SDL_SetJoystickVirtualAxis(g_virtualJoystick, static_cast<int>(axis), static_cast<Sint16>(value));
            return;
        }
        SDL_Event e = {};
        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
        e.gaxis.which = 1;
        e.gaxis.axis = static_cast<Uint8>(axis);
        e.gaxis.value = static_cast<Sint16>(value);
        pushEvent(e);
    }

    void key(SDL_Keycode code, bool down)
    {
        SDL_Event e = {};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.key = code;
        e.key.down = down;
        pushEvent(e);
    }

    void mouseMove(float x, float y)
    {
        SDL_Event e = {};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.x = x;
        e.motion.y = y;
        pushEvent(e);
    }

    void mouseButton(bool down)
    {
        SDL_Event e = {};
        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = down;
        e.button.clicks = 1;
        pushEvent(e);
    }

    // A stand-in for the game side of the settings menu: values in a map.
    std::map<std::string, std::string> parsePairs(const std::string &text)
    {
        std::map<std::string, std::string> out;
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find(',', start);
            if (end == std::string::npos)
                end = text.size();
            const std::string item = text.substr(start, end - start);
            start = end + 1;
            const size_t eq = item.find('=');
            if (eq != std::string::npos)
                out[item.substr(0, eq)] = item.substr(eq + 1);
        }
        return out;
    }

    struct FakeHost final : MenuHost
    {
        std::map<std::string, std::string> values{{"renderer", "gpu"},   {"resolution", "1"}, {"aspect", "4:3"}, {"fullscreen", "off"},
                                                  {"vsync", "off"},      {"filter", "sharp"},  {"volume", "100"},  {"texture_tools", "off"},
                                                  {"skip_intros", "off"}, {"fx", "off"},    {"fx_preset", "subtle"}, {"strike_depth", "original"}};
        std::map<std::string, std::string> startup;
        std::map<std::string, std::string> env;
        bool closeAsked = false;
        bool quitAsked = false;
        std::string quitBlocked; // --quit-blocked: why Quit Game is dimmed

        std::string value(const std::string &key) override { return values[key]; }
        std::string startupValue(const std::string &key) override { return startup.count(key) ? startup[key] : values[key]; }
        bool fixedByEnvironment(const std::string &key, std::string &envName) override
        {
            if (!env.count(key))
                return false;
            envName = env[key];
            return true;
        }
        void change(const std::string &key, const std::string &value) override
        {
            values[key] = value;
            std::printf("change: %s = %s\n", key.c_str(), value.c_str());
        }
        void closeRequested() override
        {
            std::printf("back\n");
            closeAsked = true;
        }
        void quitRequested() override
        {
            std::printf("quit\n");
            quitAsked = true;
            values["quit"] = "asked";
        }
        std::string quitBlockedReason() override { return quitBlocked; }
        // Task s3: the memory card line and its two buttons (the host's answers are fixed by --card-line and --card-buttons).
        std::string cardLineText;
        bool keepShown = false, backShown = false;
        std::string cardLine() override { return cardLineText; }
        std::string devLineText;
        std::string devPackLine() override { return devLineText; }
        bool canKeepCard() override { return keepShown; }
        std::string keepCard() override { return "Copied the profile card's save to your own card."; }
        bool canBackToOwnCard() override { return backShown && !backDone; }
        std::string backToOwnCard() override
        {
            backDone = true;
            std::printf("back to my card\n");
            return "From the next start the game plays from your own card. The profile card stays on disk; turn a data pack on to use it again.";
        }
        bool backDone = false;
        // Task s4b: the Import a Save... button, and the answer to "Which save?".
        bool importShown = false, importAsked = false;
        int picked = -1;
        bool canImportSave() override { return importShown; }
        std::string importSave() override
        {
            importAsked = true;
            std::printf("import a save\n");
            return "Choose the save file in the window that opened.";
        }
        // Task h1: Load Now and Restart Now (--load-now, --restart-blocked, --no-restart).
        LoadOffer loadOfferValue;
        int loadAsked = 0, restartAsked = 0;
        bool restartOk = true;
        std::string restartBlockedText;
        LoadOffer loadOffer() override { return loadOfferValue; }
        std::string loadNow() override
        {
            ++loadAsked;
            std::printf("load now\n");
            loadOfferValue = LoadOffer();
            loadOfferValue.note = "Waiting for the game to load the memory card...";
            closeAsked = true; // the real host closes the menu so the game can run
            return "Loading the memory card. The game shows its own \"Load Completed.\" when it is done.";
        }
        bool canRestart() override { return restartOk; }
        std::string restartBlockedReason() override { return restartBlockedText; }
        void restartNow() override
        {
            ++restartAsked;
            std::printf("restart now\n");
        }
        std::vector<ControlLine> controls() override
        {
            return {{"Move", "Arrow keys, W A S D", "D-pad, left stick"}, {"Cross", "X, Space", "A (bottom button)"},
                    {"Circle", "C, Escape", "B (right button)"},          {"Square", "Z, Keypad 0", "X (left button)"},
                    {"Triangle", "V, Keypad 1", "Y (top button)"},        {"L1 / R1", "Q / E", "LB / RB"},
                    {"L2 / R2", "Left Shift / Right Shift", "LT / RT"},   {"Start", "Enter", "Start"},
                    {"Select", "Tab", "Back"},                            {"This menu", "F2", "Guide, or hold L3 + R3"},
                    {"Debug panel", "F1", "-"}};
        }
        std::string controllerName() override { return "Demo virtual gamepad"; }
        ps2x::mods::Manager *manager = nullptr;
        ps2x::mods::Manager *modManager() override { return manager; }
    };

    // The application: owns the device, the overlay, the picture, and draws frames.
    struct Demo
    {
        Options opt;
        SDL_GPUDevice *device = nullptr;
        SDL_Window *window = nullptr;
        SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        SDL_GPUTexture *offscreen = nullptr; // headless target, and the PNG copy in windowed mode
        Overlay ownOverlay;
        Overlay *overlayPtr = &ownOverlay; // the menu's overlay in --menu mode
        Overlay &overlay() { return *overlayPtr; }
        SettingsPage page;
        Picture picture;
        Rml::ElementDocument *document = nullptr;
        GameMenu menu;
        FakeHost fake;
#if defined(PS2X_DEMO_MODS)
        ps2x::mods::Manager mods;
#endif
        int width = 0;
        int height = 0;
        bool quit = false;
        bool backPressed = false;
        double lastDt = 1.0 / 60.0;
        std::vector<std::string> changes;
        // Per-frame cost counters (milliseconds on this thread)
        double updateMs = 0.0, recordMs = 0.0, pictureMs = 0.0;
        int measured = 0;

        void sizeFromWindow()
        {
            if (!window)
                return;
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            if (pw > 0 && ph > 0)
            {
                width = pw;
                height = ph;
            }
            overlay().setPixelDensity(SDL_GetWindowPixelDensity(window));
        }

        // Draws one frame (picture pass, then the overlay pass) onto `target`.
        void drawFrame(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target)
        {
            auto t0 = std::chrono::steady_clock::now();
            const SDL_FColor black = {0.0f, 0.0f, 0.0f, 1.0f};
            SdlGpuRenderer &r = overlay().renderer();
            r.beginFrame(cmd, target, static_cast<uint32_t>(width), static_cast<uint32_t>(height), &black);
            drawPicture(r, picture, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            r.endFrame();
            auto t1 = std::chrono::steady_clock::now();
            overlay().record(cmd, target);
            auto t2 = std::chrono::steady_clock::now();
            if (opt.breakScissor)
            {
                // A deliberate mistake, to show that the validation layer would report one (see --validation-selftest).
                SDL_GPUColorTargetInfo colour = {};
                colour.texture = target;
                colour.load_op = SDL_GPU_LOADOP_LOAD;
                colour.store_op = SDL_GPU_STOREOP_STORE;
                if (SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &colour, 1, nullptr))
                {
                    const SDL_Rect bad = {-8, 0, 16, 16};
                    SDL_SetGPUScissor(pass, &bad);
                    SDL_EndGPURenderPass(pass);
                }
            }
            pictureMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
            recordMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
            ++measured;
        }

        void pump(bool allowRealPads)
        {
            SDL_Event e;
            while (SDL_PollEvent(&e))
            {
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                {
                    quit = true;
                    continue;
                }
                if (allowRealPads && e.type == SDL_EVENT_GAMEPAD_ADDED)
                {
                    SDL_Gamepad *pad = SDL_OpenGamepad(e.gdevice.which);
                    std::printf("gamepad added: %s\n", pad ? SDL_GetGamepadName(pad) : "(could not open)");
                    continue;
                }
                overlay().handleEvent(e);
            }
        }

        // One frame: events, update, draw, present or download. Returns false when the frame could not be drawn.
        // `png` gets the picture when it is not empty.
        bool frame(bool allowRealPads, const std::string &png)
        {
            pump(allowRealPads);
            sizeFromWindow();
            overlay().setViewport(width, height, opt.scale);

            auto u0 = std::chrono::steady_clock::now();
            overlay().update(lastDt);
            updateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - u0).count();

            SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
            if (!cmd)
                die("SDL_AcquireGPUCommandBuffer");

            std::vector<uint8_t> rgba;
            const bool wantPng = !png.empty();
            if (window)
            {
                SDL_GPUTexture *swap = nullptr;
                Uint32 sw = 0, sh = 0;
                if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &sw, &sh))
                    die("SDL_WaitAndAcquireGPUSwapchainTexture");
                if (!swap)
                {
                    SDL_SubmitGPUCommandBuffer(cmd); // minimised: nothing to draw
                    return true;
                }
                width = static_cast<int>(sw);
                height = static_cast<int>(sh);
                overlay().setViewport(width, height, opt.scale);
                drawFrame(cmd, swap);
                if (wantPng)
                {
                    // Swapchain textures cannot be copied from, so the same frame is drawn again offscreen.
                    ensureOffscreen();
                    drawFrame(cmd, offscreen);
                    if (!downloadAndSubmit(device, cmd, offscreen, width, height, format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, rgba))
                        return false;
                }
                else
                {
                    SDL_SubmitGPUCommandBuffer(cmd);
                }
            }
            else
            {
                ensureOffscreen();
                drawFrame(cmd, offscreen);
                if (wantPng)
                {
                    if (!downloadAndSubmit(device, cmd, offscreen, width, height, format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, rgba))
                        return false;
                }
                else
                {
                    SDL_SubmitGPUCommandBuffer(cmd);
                }
            }
            if (wantPng)
            {
                if (!ps2x::test::writePng(png, rgba.data(), static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                          static_cast<uint32_t>(width) * 4))
                {
                    std::fprintf(stderr, "FAIL could not write %s\n", png.c_str());
                    return false;
                }
                std::printf("wrote %s\n", png.c_str());
            }
            return true;
        }

        int offscreenW = 0, offscreenH = 0;
        void ensureOffscreen()
        {
            if (offscreen && offscreenW == width && offscreenH == height)
                return;
            if (offscreen)
            {
                SDL_WaitForGPUIdle(device);
                SDL_ReleaseGPUTexture(device, offscreen);
            }
            offscreen = createTarget(device, format, width, height);
            if (!offscreen)
                die("SDL_CreateGPUTexture(offscreen target)");
            offscreenW = width;
            offscreenH = height;
        }

        void frames(int n, bool allowRealPads = true)
        {
            for (int i = 0; i < n && !quit; ++i)
                frame(allowRealPads, std::string());
        }
    };

    std::string sanitize(const std::string &step)
    {
        std::string s;
        for (char c : step)
            s += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
        return s;
    }

    // The page the demo is showing: the menu's in --menu mode, else the demo page.
    Rml::ElementDocument *currentDocument(Demo &d)
    {
        Rml::Context *context = d.overlay().context();
        return context && context->GetNumDocuments() > 0 ? context->GetDocument(0) : nullptr;
    }

    // The first element that matches an RCSS selector ("#quit", ".tab.selected"). "panel" is the content box that is showing (the
    // .panel without the class "hidden"); a hidden element has no box, so the answer is nullptr for one.
    Rml::Element *findElement(Demo &d, const std::string &selector)
    {
        Rml::ElementDocument *document = currentDocument(d);
        if (!document)
            return nullptr;
        if (selector == "panel")
        {
            Rml::ElementList panels;
            document->QuerySelectorAll(panels, ".panel");
            for (Rml::Element *panel : panels)
                if (!panel->IsClassSet("hidden"))
                    return panel;
            return nullptr;
        }
        // "SELECTOR|N": the Nth match (counting from 1), for lists of rows (".mod|3").
        const size_t bar = selector.rfind('|');
        if (bar != std::string::npos)
        {
            Rml::ElementList matches;
            document->QuerySelectorAll(matches, selector.substr(0, bar));
            const size_t n = static_cast<size_t>(std::max(1, std::atoi(selector.c_str() + bar + 1)));
            return n <= matches.size() ? matches[n - 1] : nullptr;
        }
        return document->QuerySelector(selector);
    }

    // Runs one scripted step: pushes its events, draws frames, writes the PNG. Returns false if the step is unknown.
    bool runStep(Demo &d, const std::string &step, int number)
    {
        auto press = [&](SDL_GamepadButton b) {
            padButton(b, true);
            d.frames(3, false);
            padButton(b, false);
            d.frames(static_cast<int>(kSettleFrames), false);
        };
        auto stick = [&](SDL_GamepadAxis axis, int value) {
            padAxis(axis, value);
            d.frames(3, false);
            padAxis(axis, 0);
            d.frames(static_cast<int>(kSettleFrames), false);
        };

        const size_t colon = step.find(':');
        const std::string name = step.substr(0, colon);
        const std::string arg = colon == std::string::npos ? std::string() : step.substr(colon + 1);

        if (name == "up") press(SDL_GAMEPAD_BUTTON_DPAD_UP);
        else if (name == "down") press(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        else if (name == "left") press(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        else if (name == "right") press(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        else if (name == "a") press(SDL_GAMEPAD_BUTTON_SOUTH);
        else if (name == "b") press(SDL_GAMEPAD_BUTTON_EAST);
        else if (name == "lb") press(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        else if (name == "rb") press(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        else if (name == "lsup") stick(SDL_GAMEPAD_AXIS_LEFTY, -32000);
        else if (name == "lsdown") stick(SDL_GAMEPAD_AXIS_LEFTY, 32000);
        else if (name == "lsleft") stick(SDL_GAMEPAD_AXIS_LEFTX, -32000);
        else if (name == "lsright") stick(SDL_GAMEPAD_AXIS_LEFTX, 32000);
        else if (name == "key")
        {
            const SDL_Keycode code = SDL_GetKeyFromName(arg.c_str());
            if (code == SDLK_UNKNOWN)
                return false;
            key(code, true);
            d.frames(3, false);
            key(code, false);
            d.frames(static_cast<int>(kSettleFrames), false);
        }
        else if (name == "drop")
        {
            // A file dropped on the window: the manager installs it and the menu shows the answer on the Mods tab.
#if defined(PS2X_DEMO_MODS)
            if (!d.fake.manager)
                return false;
            const ps2x::mods::Result result = d.fake.manager->install(fs::path(arg));
            std::printf("drop: %s\n", result.message.c_str());
            d.menu.showMods(result.message);
            d.frames(static_cast<int>(kSettleFrames), false);
#else
            return false;
#endif
        }
        else if (name == "pick")
        {
            // The question "Which save?" with the labels in arg, separated by |.
            std::vector<std::string> labels;
            size_t from = 0;
            while (from <= arg.size())
            {
                const size_t bar = arg.find('|', from);
                labels.push_back(arg.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
                if (bar == std::string::npos)
                    break;
                from = bar + 1;
            }
            d.menu.askWhichSave("That file holds " + std::to_string(labels.size()) + " saves of the game. Which one do you want to import?", labels,
                                [&d](size_t index) {
                                    d.fake.picked = static_cast<int>(index);
                                    std::printf("picked %zu\n", index);
                                });
            d.frames(static_cast<int>(kSettleFrames), false);
        }
        else if (name == "move" || name == "click")
        {
            float x = 0, y = 0;
            if (!arg.empty() && arg[0] == '@')
            {
                // The middle of the first element that matches a selector ("click:@#quit"), so a script does not depend on the layout.
                Rml::Element *target = findElement(d, arg.substr(1));
                if (!target || target->GetClientWidth() <= 0.0f)
                {
                    std::fprintf(stderr, "FAIL no visible element for '%s'\n", arg.c_str());
                    return false;
                }
                const Rml::Vector2f at = target->GetAbsoluteOffset(Rml::BoxArea::Border);
                const Rml::Vector2f size = target->GetBox().GetSize(Rml::BoxArea::Border);
                x = at.x + size.x * 0.5f;
                y = at.y + size.y * 0.5f;
            }
            else if (std::sscanf(arg.c_str(), "%f,%f", &x, &y) != 2)
                return false;
            mouseMove(x, y);
            d.frames(2, false);
            if (name == "click")
            {
                mouseButton(true);
                d.frames(2, false);
                mouseButton(false);
            }
            d.frames(static_cast<int>(kSettleFrames), false);
        }
        else if (name == "drag")
        {
            float x0, y0, x1, y1;
            if (std::sscanf(arg.c_str(), "%f,%f,%f,%f", &x0, &y0, &x1, &y1) != 4)
                return false;
            mouseMove(x0, y0);
            d.frames(2, false);
            mouseButton(true);
            d.frames(2, false);
            for (int i = 1; i <= 6; ++i)
            {
                mouseMove(x0 + (x1 - x0) * i / 6.0f, y0 + (y1 - y0) * i / 6.0f);
                d.frames(1, false);
            }
            mouseButton(false);
            d.frames(static_cast<int>(kSettleFrames), false);
        }
        else if (name == "wait")
        {
            d.frames(std::max(1, std::atoi(arg.c_str())), false);
        }
        else
        {
            return false;
        }

        if (!d.opt.pngDir.empty())
        {
            char prefix[16];
            std::snprintf(prefix, sizeof(prefix), "%02d_", number);
            const std::string path = (fs::path(d.opt.pngDir) / (std::string(prefix) + sanitize(step) + ".png")).string();
            d.frame(false, path);
        }
        return true;
    }

    std::string findAssets(const Options &opt, const char *argv0)
    {
        if (!opt.assets.empty())
            return opt.assets;
        const fs::path exeDir = fs::path(argv0).parent_path();
        for (const fs::path &candidate : {exeDir / "ui_assets", fs::path(PS2X_UI_SOURCE_ASSET_DIR)})
            if (fs::exists(candidate / "pages" / "settings.rml"))
                return candidate.string();
        return std::string();
    }

    // What the --expect keys focus, confirm, quit-locked, help and tab read from the menu's page.
    std::string menuState(Demo &d, const std::string &key)
    {
        Rml::Context *context = d.menu.overlay().context();
        Rml::ElementDocument *document = context && context->GetNumDocuments() > 0 ? context->GetDocument(0) : nullptr;
        if (!document)
            return "(no page)";
        if (key == "focus")
        {
            Rml::Element *focus = context->GetFocusElement();
            if (!focus)
                return "(none)";
            if (!focus->GetId().empty())
                return focus->GetId();
            // The Mods tab's elements have no id: name them by what they are.
            if (focus->IsClassSet("mod"))
                return "mod:" + focus->GetAttribute<Rml::String>("data-key", "");
            if (focus->IsClassSet("mbtn"))
                return "btn:" + focus->GetAttribute<Rml::String>("data-action", "");
            if (focus->IsClassSet("mrow"))
                return "setting:" + focus->GetAttribute<Rml::String>("data-setting", "");
            return "(unnamed)";
        }
        if (key.compare(0, 5, "rect:") == 0)
        {
            // "rect:#header" or "rect:panel": the element's border box in window pixels as "x y w h" (rounded), "(none)" if it is
            // missing or not shown. Check scripts compare these across tabs and sizes.
            Rml::Element *element = findElement(d, key.substr(5));
            if (!element || element->GetClientWidth() <= 0.0f)
                return "(none)";
            const Rml::Vector2f at = element->GetAbsoluteOffset(Rml::BoxArea::Border);
            const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
            char text[96];
            std::snprintf(text, sizeof(text), "%d %d %d %d", static_cast<int>(std::lround(at.x)), static_cast<int>(std::lround(at.y)),
                          static_cast<int>(std::lround(size.x)), static_cast<int>(std::lround(size.y)));
            return text;
        }
        if (key == "scale")
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.3f", d.overlay().scale());
            return text;
        }
        if (key == "mod-confirm")
        {
            Rml::Element *confirm = document->GetElementById("mod-confirm");
            return confirm && !confirm->IsClassSet("hidden") ? "shown" : "hidden";
        }
        if (key == "selected-mod")
        {
            Rml::ElementList rows;
            document->QuerySelectorAll(rows, ".mod.selected");
            return rows.empty() ? std::string("(none)") : rows.front()->GetAttribute<Rml::String>("data-key", "");
        }
        if (key == "mods-card")
        {
            Rml::Element *element = document->GetElementById("mods-card");
            return element ? element->GetInnerRML() : std::string("(no element)");
        }
        if (key == "import-asked")
            return d.fake.importAsked ? "1" : "0";
        if (key == "picked")
            return std::to_string(d.fake.picked);
        if (key == "back-card" || key == "keep-card" || key == "import-save")
        {
            Rml::Element *element = document->GetElementById(key);
            if (!element)
                return std::string("(no element)");
            const Rml::Property *display = element->GetProperty("display");
            return display && display->ToString() == "none" ? "hidden" : "shown";
        }
        if (key == "back-asked")
            return d.fake.backDone ? "1" : "0";
        if (key == "load-now" || key == "restart-now" || key == "mods-apply")
        {
            Rml::Element *element = document->GetElementById(key);
            if (!element)
                return std::string("(no element)");
            const Rml::Property *display = element->GetProperty("display");
            return display && display->ToString() == "none" ? "hidden" : "shown";
        }
        if (key == "load-now-locked" || key == "restart-now-locked")
        {
            Rml::Element *element = document->GetElementById(key == "load-now-locked" ? "load-now" : "restart-now");
            return element ? element->GetAttribute<Rml::String>("data-locked", "") : std::string("(no element)");
        }
        if (key == "mods-load-note")
        {
            Rml::Element *element = document->GetElementById("mods-load-note");
            return element ? element->GetInnerRML() : std::string("(no element)");
        }
        if (key == "mod-confirm-title" || key == "mod-confirm-text")
        {
            Rml::Element *element = document->GetElementById(key);
            return element ? element->GetInnerRML() : std::string("(no element)");
        }
        if (key == "load-asked")
            return std::to_string(d.fake.loadAsked);
        if (key == "restart-asked")
            return std::to_string(d.fake.restartAsked);
        if (key == "mods-status" || key == "mod-title" || key == "mod-problem" || key == "restart-note")
        {
            const char *id = key == "mods-status" ? "mods-status" : (key == "mod-title" ? "mod-title" : (key == "mod-problem" ? "mod-problem" : "restart-note"));
            Rml::Element *element = document->GetElementById(id);
            if (key == "restart-note" && element && element->IsClassSet("hidden"))
                return std::string();
            return element ? element->GetInnerRML() : std::string();
        }
        if (key == "confirm")
        {
            Rml::Element *confirm = document->GetElementById("confirm");
            return confirm && !confirm->IsClassSet("hidden") ? "shown" : "hidden";
        }
        if (key == "quit-locked")
        {
            Rml::Element *quit = document->GetElementById("quit");
            return quit ? quit->GetAttribute<Rml::String>("data-locked", "") : std::string("(no button)");
        }
        if (key == "tab")
        {
            Rml::ElementList tabs;
            document->QuerySelectorAll(tabs, ".tab.selected");
            return tabs.empty() ? std::string("(none)") : tabs.front()->GetAttribute<Rml::String>("data-tab", "");
        }
        Rml::Element *help = document->GetElementById("help-text");
        return help ? help->GetInnerRML() : std::string("(no help line)");
    }

    void usage()
    {
        std::fprintf(stderr,
                     "usage: ps2x_ui_demo [--driver vulkan|direct3d12|metal] [--debug] [--size WxH] [--scale S] [--assets DIR]\n"
                     "                    [--backdrop FILE.png] [--aspect W:H] [--png FILE] [--png-dir DIR] [--script STEPS]\n"
                     "                    [--expect key=value,...] [--window | --headless] [--frames N]\n");
    }
}

int main(int argc, char **argv)
{
    Demo d;
    Options &opt = d.opt;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--driver" && i + 1 < argc)
            opt.driver = next();
        else if (a == "--debug")
            opt.debug = true;
        else if (a == "--size" && i + 1 < argc)
        {
            if (std::sscanf(next(), "%dx%d", &opt.width, &opt.height) != 2)
            {
                usage();
                return 2;
            }
        }
        else if (a == "--scale" && i + 1 < argc)
            opt.scale = static_cast<float>(std::atof(next()));
        else if (a == "--assets" && i + 1 < argc)
            opt.assets = next();
        else if (a == "--backdrop" && i + 1 < argc)
            opt.backdrop = next();
        else if (a == "--aspect" && i + 1 < argc)
            opt.aspect = next();
        else if (a == "--png" && i + 1 < argc)
            opt.png = next();
        else if (a == "--png-dir" && i + 1 < argc)
            opt.pngDir = next();
        else if (a == "--script" && i + 1 < argc)
            opt.script = next();
        else if (a == "--expect" && i + 1 < argc)
            opt.expect = next();
        else if (a == "--headless")
            opt.headless = true;
        else if (a == "--validation-selftest")
            opt.breakScissor = true;
        else if (a == "--virtual-pad")
            opt.virtualPad = true;
        else if (a == "--page" && i + 1 < argc)
            opt.page = next();
        else if (a == "--menu")
            opt.menu = true;
        else if (a == "--set" && i + 1 < argc)
            opt.menuSet = next();
        else if (a == "--startup" && i + 1 < argc)
            opt.menuStartup = next();
        else if (a == "--env-lock" && i + 1 < argc)
            opt.menuEnvLock = next();
        else if (a == "--quit-blocked" && i + 1 < argc)
            opt.quitBlocked = next();
        else if (a == "--card-line" && i + 1 < argc)
            opt.cardLine = next();
        else if (a == "--card-buttons" && i + 1 < argc)
            opt.cardButtons = next();
        else if (a == "--dev-line" && i + 1 < argc)
            opt.devLine = next();
        else if (a == "--load-now" && i + 1 < argc)
            opt.loadNow = next();
        else if (a == "--restart-blocked" && i + 1 < argc)
            opt.restartBlocked = next();
        else if (a == "--no-restart")
            opt.noRestart = true;
        else if (a == "--mods-dir" && i + 1 < argc)
            opt.modsDir = next();
        else if (a == "--mods-state" && i + 1 < argc)
            opt.modsState = next();
        else if (a == "--window")
            opt.forceWindow = true;
        else if (a == "--frames" && i + 1 < argc)
            opt.frames = std::atoi(next());
        else
        {
            usage();
            return 2;
        }
    }

    const bool scripted = !opt.script.empty() || !opt.png.empty();
    const bool useWindow = !opt.headless && (!scripted || opt.forceWindow);
    d.width = opt.width;
    d.height = opt.height;
    if (!opt.pngDir.empty())
        fs::create_directories(opt.pngDir);

    const std::string assets = findAssets(opt, argv[0]);
    if (assets.empty())
    {
        std::fprintf(stderr, "FATAL: cannot find the UI assets (looked for ui_assets next to the program and in %s); use --assets\n",
                     PS2X_UI_SOURCE_ASSET_DIR);
        return 2;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD))
        die("SDL_Init");
    SDL_Gamepad *virtualPad = nullptr;
    if (opt.virtualPad)
    {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "ps2x_ui_demo virtual gamepad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        virtualPad = id ? SDL_OpenGamepad(id) : nullptr;
        if (!virtualPad)
            die("virtual gamepad");
        g_virtualJoystick = SDL_GetGamepadJoystick(virtualPad);
        std::printf("virtual gamepad: %s\n", SDL_GetGamepadName(virtualPad));
    }
    const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
    d.device = SDL_CreateGPUDevice(formats, opt.debug, opt.driver);
    if (!d.device)
        die("SDL_CreateGPUDevice");
    {
        const SDL_PropertiesID props = SDL_GetGPUDeviceProperties(d.device);
        std::printf("driver: %s, device: %s, driver version: %s\n", SDL_GetGPUDeviceDriver(d.device),
                    SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_NAME_STRING, "?"),
                    SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, "?"));
    }

    if (useWindow)
    {
        d.window = SDL_CreateWindow("ps2x_ui_demo", opt.width, opt.height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!d.window)
            die("SDL_CreateWindow");
        if (!SDL_ClaimWindowForGPUDevice(d.device, d.window))
            die("SDL_ClaimWindowForGPUDevice");
        d.format = SDL_GetGPUSwapchainTextureFormat(d.device, d.window);
        std::printf("window: swapchain format %d\n", static_cast<int>(d.format));
    }

    OverlayConfig config;
    config.device = d.device;
    config.targetFormat = d.format;
    config.assetRoot = assets;
    config.window = d.window;
    if (opt.menu)
    {
        for (auto &kv : parsePairs(opt.menuSet))
            d.fake.values[kv.first] = kv.second;
        d.fake.startup = d.fake.values;
        for (auto &kv : parsePairs(opt.menuStartup))
            d.fake.startup[kv.first] = kv.second;
        d.fake.env = parsePairs(opt.menuEnvLock);
        d.fake.quitBlocked = opt.quitBlocked;
        d.fake.cardLineText = opt.cardLine;
        d.fake.devLineText = opt.devLine;
        d.fake.keepShown = opt.cardButtons.find("keep") != std::string::npos;
        d.fake.backShown = opt.cardButtons.find("back") != std::string::npos;
        d.fake.importShown = opt.cardButtons.find("import") != std::string::npos;
        d.fake.restartOk = !opt.noRestart;
        d.fake.restartBlockedText = opt.restartBlocked;
        if (opt.loadNow == "waiting" || opt.loadNow.compare(0, 7, "pending") == 0)
        {
            LoadOffer &offer = d.fake.loadOfferValue;
            offer.shown = true;
            offer.note = "Data pack changes are waiting. Load Now applies them in the running game, or restart.";
            offer.enabled = opt.loadNow == "pending" || opt.loadNow == "pending-unsaved";
            offer.unsaved = opt.loadNow == "pending-unsaved";
            offer.help = opt.loadNow == "waiting"           ? "Waiting for the game to pause."
                         : opt.loadNow == "pending-wrongplace" ? "Go to the title screen or the main menu."
                                                               : "Load the memory card again now, with the data packs as they are. Asks first.";
            offer.question = offer.unsaved ? "The game has changes you have not saved yet, and they will be lost. The memory card is loaded again, as if you had restarted."
                                           : "Anything you changed since you last saved in the game will be lost.";
        }
        else if (opt.loadNow.compare(0, 5, "note:") == 0)
        {
            d.fake.loadOfferValue.note = opt.loadNow.substr(5);
        }
#if defined(PS2X_DEMO_MODS)
        if (!opt.modsDir.empty())
        {
            ps2x::mods::Manager::Config mc;
            mc.modsDir = opt.modsDir;
            mc.statePath = opt.modsState.empty() ? fs::path(opt.modsDir).parent_path() / "mods.toml" : fs::path(opt.modsState);
            if (!d.mods.init(mc))
            {
                std::fprintf(stderr, "FATAL: the mod manager could not start: %s\n", d.mods.initError().c_str());
                return 2;
            }
            for (const std::string &note : d.mods.notes())
                std::fprintf(stderr, "mods: %s\n", note.c_str());
            d.fake.manager = &d.mods;
            // What the game does at a start: the theme mods that are on add their style sheets after the game's own theme.
            config.extraStyleSheets = d.mods.sessionThemeStylesheets();
            config.virtualFiles = [&d](const std::string &name, std::vector<uint8_t> &bytes) {
                auto data = d.mods.readVirtual(name);
                if (!data)
                    return false;
                bytes = std::move(*data);
                return true;
            };
        }
#endif
        if (!d.menu.init(config, &d.fake))
        {
            std::fprintf(stderr, "FATAL: the overlay could not start\n");
            return 2;
        }
        d.overlayPtr = &d.menu.overlay();
    }
    else if (!d.overlay().init(config))
    {
        std::fprintf(stderr, "FATAL: the overlay could not start\n");
        return 2;
    }
    d.sizeFromWindow();
    d.overlay().setViewport(d.width, d.height, opt.scale);
    if (scripted)
        d.overlay().setMouseGuard(false); // a script pushes mouse and key steps back to back

    std::vector<uint8_t> pic;
    if (opt.backdrop.empty() || !loadBackdrop(opt.backdrop, pic))
    {
        if (!opt.backdrop.empty())
            std::fprintf(stderr, "warning: cannot read backdrop %s, using the built-in picture\n", opt.backdrop.c_str());
        pic = makeStandInPicture();
    }
    createPicture(d.overlay().renderer(), d.picture, pic);

    if (opt.menu)
    {
        d.menu.open();
        if (!d.menu.visible())
            return 2;
    }
    else
    {
        d.document = d.overlay().loadPage(opt.page);
        if (!d.document)
            return 2;
        d.overlay().setVisible(true);
    }

    auto applyAspect = [&](const std::string &v) {
        int aw = 4, ah = 3;
        if (std::sscanf(v.c_str(), "%d:%d", &aw, &ah) == 2 && aw > 0 && ah > 0)
        {
            d.picture.aspectW = static_cast<float>(aw);
            d.picture.aspectH = static_cast<float>(ah);
        }
    };
    applyAspect(opt.aspect);

    d.page.setOnChange([&](const std::string &key, const std::string &value) {
        std::printf("change: %s = %s\n", key.c_str(), value.c_str());
        d.changes.push_back(key + "=" + value);
        // Two settings act on the stand-in picture, to show that a change is live.
        if (key == "aspect")
            applyAspect(value);
        else if (key == "brightness")
            updateBrightness(d.overlay().renderer(), d.picture, static_cast<float>(std::atoi(value.c_str())) / 100.0f);
    });
    d.page.setOnBack([&]() {
        std::printf("back\n");
        d.backPressed = true;
    });
    // Another page (--page) is just shown: it has none of the settings page's rows to wire up.
    if (!opt.menu && opt.page == "pages/settings.rml")
    {
        if (!d.page.attach(d.document))
            return 2;
        // The page's own starting values drive the picture (so it matches what the page shows).
        applyAspect(d.page.value("aspect"));
        updateBrightness(d.overlay().renderer(), d.picture, static_cast<float>(std::atoi(d.page.value("brightness").c_str())) / 100.0f);
        d.page.focusFirstRow();
    }

    int status = 0;
    if (scripted)
    {
        // Frames to start with, so layout and the first focus are settled.
        d.frames(6, false);
        if (!opt.pngDir.empty())
            d.frame(false, (fs::path(opt.pngDir) / "00_start.png").string());
        if (!opt.png.empty())
            d.frame(false, opt.png);
        int number = 1;
        for (const std::string &step : splitCommas(opt.script))
        {
            if (!runStep(d, step, number))
            {
                std::fprintf(stderr, "FAIL unknown or malformed step '%s'\n", step.c_str());
                status = 1;
                break;
            }
            ++number;
            if (d.backPressed || d.fake.closeAsked || d.fake.quitAsked)
                break;
        }
        // Settings the script should have reached.
        if (!opt.expect.empty())
        {
            size_t start = 0;
            while (start < opt.expect.size())
            {
                size_t end = opt.expect.find(',', start);
                if (end == std::string::npos)
                    end = opt.expect.size();
                const std::string item = opt.expect.substr(start, end - start);
                start = end + 1;
                const size_t eq = item.find('=');
                if (eq == std::string::npos)
                    continue;
                const std::string key = item.substr(0, eq);
                const std::string want = item.substr(eq + 1);
                std::string got = opt.menu ? d.fake.values[key] : d.page.value(key);
                if (opt.menu && (key.compare(0, 5, "rect:") == 0 || key == "scale" || key == "focus" || key == "confirm" || key == "quit-locked" || key == "help" || key == "tab" ||
                                 key == "mod-confirm" || key == "selected-mod" || key == "mods-status" || key == "mod-title" ||
                                 key == "mod-problem" || key == "restart-note" || key == "mods-card" || key == "back-card" || key == "keep-card" ||
                                 key == "back-asked" || key == "import-save" || key == "import-asked" || key == "picked" || key == "load-now" ||
                                 key == "restart-now" || key == "mods-apply" || key == "load-now-locked" || key == "restart-now-locked" ||
                                 key == "mods-load-note" || key == "load-asked" || key == "restart-asked" || key == "mod-confirm-title" || key == "mod-confirm-text"))
                    got = menuState(d, key);
#if defined(PS2X_DEMO_MODS)
                if (opt.menu && d.fake.manager && (key == "mods-on" || key == "mods-active" || key == "mods-order" || key == "mods-restart" || key == "mods-count"))
                {
                    got.clear();
                    int count = 0;
                    for (const ps2x::mods::ModInfo &m : d.mods.list())
                    {
                        ++count;
                        const bool take = key == "mods-order" ? m.valid : (key == "mods-on" ? m.enabled : (key == "mods-active" ? m.active : false));
                        if (take)
                            got += (got.empty() ? "" : "|") + m.key;
                    }
                    if (key == "mods-restart")
                        got = d.mods.restartNeeded() ? "1" : "0";
                    else if (key == "mods-count")
                        got = std::to_string(count);
                }
#endif
                const bool ok = (!want.empty() && want[0] == '~') ? got.find(want.substr(1)) != std::string::npos : got == want;
                std::printf("%s expect %s = %s (got %s)\n", ok ? "PASS" : "FAIL", key.c_str(), want.c_str(), got.c_str());
                if (!ok)
                    status = 1;
            }
        }
    }
    else
    {
        int n = 0;
        const auto t0 = std::chrono::steady_clock::now();
        auto last = t0;
        while (!d.quit && !d.backPressed && (opt.frames == 0 || n < opt.frames))
        {
            const auto now = std::chrono::steady_clock::now();
            d.lastDt = std::min(0.1, std::chrono::duration<double>(now - last).count());
            last = now;
            d.frame(true, std::string());
            ++n;
            if (!d.window)
                SDL_DelayPrecise(16000000); // headless without a script: pace like 60 Hz
        }
        SDL_WaitForGPUIdle(d.device);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("ran %d frames in %.0f ms (%.1f fps)\n", n, ms, n * 1000.0 / std::max(ms, 1.0));
    }

    SDL_WaitForGPUIdle(d.device);
    const SdlGpuRenderer::FrameStats &fs_ = d.overlay().renderer().lastFrame();
    if (d.measured > 0)
        std::printf("per frame on this thread: picture pass %.3f ms, overlay record %.3f ms, update %.3f ms (%d frames)\n",
                    d.pictureMs / d.measured, d.recordMs / d.measured, d.updateMs / d.measured, d.measured);
    std::printf("last overlay frame: %u draws, %u geometries, %u vertices, %u indices, %u textures; alive: %zu textures (%.1f MB), %zu geometries\n",
                fs_.draws, fs_.geometries, fs_.vertices, fs_.indices, fs_.uniqueTextures, d.overlay().renderer().textureCount(),
                static_cast<double>(d.overlay().renderer().textureBytes()) / (1024.0 * 1024.0), d.overlay().renderer().geometryCount());
    const LogCounts logs = d.overlay().logCounts();
    std::printf("ui log: %d errors, %d warnings, %d info\n", logs.errors, logs.warnings, logs.infos);
    if (logs.errors > 0)
        status = 1;

    d.page.detach();
    d.overlay().renderer().ReleaseGeometry(d.picture.quad);
    d.overlay().renderer().ReleaseGeometry(d.picture.shade);
    d.overlay().renderer().ReleaseTexture(d.picture.texture);
    if (opt.menu)
        d.menu.shutdown();
    else
        d.overlay().shutdown();
    if (d.offscreen)
        SDL_ReleaseGPUTexture(d.device, d.offscreen);
    if (d.window)
    {
        SDL_ReleaseWindowFromGPUDevice(d.device, d.window);
        SDL_DestroyWindow(d.window);
    }
    SDL_DestroyGPUDevice(d.device);
    if (virtualPad)
        SDL_CloseGamepad(virtualPad);
    SDL_Quit();
    std::printf("RESULT: %s\n", status == 0 ? "ok" : "FAILED");
    return status;
}
