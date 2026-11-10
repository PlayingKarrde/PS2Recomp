// The settings overlay inside the desktop host. See ps2_host_ui.h for what it is. This file is the game side of it:
// opening and closing, input, pausing, and what each setting does to the running game.
//
// ---- How the overlay is opened and closed, and why these inputs ----
//
//   keyboard    F2             The pad map (ps2_pad.cpp) uses these keys: arrows, W A S D, X C Z V, Space, Escape, Q E,
//                              Left/Right Shift, Enter, Tab, keypad 0 and 1; the host adds F1 (the debug panel). F2 is not
//                              used by the game, and tools/drive_game.py never sends it.
//   controller  Guide button   (the Xbox / PS / Home button). The host passes no Guide button to the game (the PS2 pad has
//                              none), so a press can never reach it.
//               L3 + R3 held   Both stick clicks together for 0.4 s, for controllers without a Guide button (or when the
//                              system keeps it, as the Xbox Game Bar does). The PS2 pad has these buttons, but the host maps
//                              no key to them and the game's menus and matches were not seen to react to them (task u2
//                              report). A hold of 0.4 s stops a stray double click while playing from opening the menu.
//                              The same inputs close it again; B (Escape in the UI) and the Back button also close it.
//
// While the overlay is open: the guest is paused and the sound is silent (ps2_pause.h), the overlay takes all keyboard,
// mouse and controller events, and the game's pad readers see no input (setInputCapturedByOverlay). On close, a button
// that is still held (the one that closed the menu) stays hidden from the game until it is released.
//
// ---- Quit Game (task q1) ----
//
// The menu's Quit Game button asks "Quit to the desktop?" (Cancel has the focus) and then calls GameHost::quitRequested.
// update() saves the settings and calls requestQuit(), which sets the flag the window's close button sets: pumpEvents()
// returns false, the run loop calls PS2Runtime::requestStop and joins the game thread. The guest is paused at that moment;
// waitWhileGuestPaused polls the stop flag every 20 ms, so the pause cannot hold the shutdown up.
//
// The memory card: a file the game writes is a working copy (<name>.mctmp) that replaces the real one only when the game
// closes it, so quitting never damages a save that is already on the card. It can lose the save that was being written:
// one save is three files (the data, view.ico, icon.sys) written within about 17 ms, and the data file is open for 4 ms of
// that. The game is paused while the menu is open and cannot finish, so waiting is not possible; instead Quit Game is dimmed,
// with a reason in the help line, when the card was being written at the moment the game stopped:
//   - the stubs report a file open for writing (ps2_card_activity.h): "The game is saving...";
//   - or the last card write was less than kCardQuietMs ago (the gap between two of the three files): "...just saved...".
//   The rule is cardQuitBlockReason() in ps2_card_activity.h, where the unit tests reach it.
// The state is read once, when the guest has really stopped (guestBlocked()), not when the menu opens: the guest runs on
// until its next vertical blank. Until then the button is dimmed with "Waiting for the game to pause.".
//
// Test hook: PS2X_TEST_PAD=<file> makes the host attach an SDL virtual gamepad and apply the lines a script appends to
// the file ("down south", "up south", "axis leftx 20000"...). Real controllers are untouched; scripted runs set
// SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT so only this one is seen (tools/drive_game.py does).

#include "ps2_host_ui.h"

#include "ps2_host_backend.h"
#include "ps2_host_mods.h"
#include "ps2_settings.h"
#include "runtime/gs/gs_gpu_host.h"
#include "runtime/gs/gs_gpu_threads.h"
#include "runtime/ps2_card_activity.h"
#include "runtime/ps2_pause.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(PS2X_HAS_UI)
#include "ui_game_menu.h"
#endif

namespace ps2x_host::ui
{
    namespace
    {
        constexpr uint64_t kChordHoldNs = 400'000'000ull; // L3 + R3 held this long opens or closes the overlay

        bool envFlagOff(const char *name)
        {
            const char *value = std::getenv(name);
            return value && value[0] == '0';
        }

        struct TestPad
        {
            SDL_Joystick *joystick = nullptr;
            std::string file;
            uint64_t offset = 0;
            std::string pending; // a partial last line
        };

        struct State
        {
            bool created = false;
            SDL_Window *window = nullptr;
            std::atomic<bool> visible{false};
            bool closeRequested = false;
            uint64_t lastUpdateNs = 0;
            uint64_t openedAtNs = 0;
            uint64_t fullscreenAskedNs = 0; // when the menu last asked the window to switch full screen on or off
            bool l3 = false, r3 = false;
            uint64_t chordSinceNs = 0;
            bool chordFired = false;
            TestPad testPad;
            // Quit Game
            bool quitAsked = false;     // the player confirmed; update() carries it out
            bool quitChecked = false;   // the card was looked at for this opening of the menu
            uint64_t blockEntriesAtOpen = 0; // guestBlockEntries() when the menu opened: only a later pause counts
            std::string quitBlock;      // why Quit Game is dimmed ("" = it is not)
            bool testOpenOnCardWrite = false; // PS2X_TEST_OPEN_ON_CARD_WRITE=1
        } g;

        // ---- test pad -------------------------------------------------------------------------------------------

        bool padButtonByName(const std::string &name, SDL_GamepadButton &button)
        {
            static const struct { const char *name; SDL_GamepadButton button; } names[] = {
                {"south", SDL_GAMEPAD_BUTTON_SOUTH},        {"east", SDL_GAMEPAD_BUTTON_EAST},
                {"west", SDL_GAMEPAD_BUTTON_WEST},          {"north", SDL_GAMEPAD_BUTTON_NORTH},
                {"back", SDL_GAMEPAD_BUTTON_BACK},          {"guide", SDL_GAMEPAD_BUTTON_GUIDE},
                {"start", SDL_GAMEPAD_BUTTON_START},        {"lstick", SDL_GAMEPAD_BUTTON_LEFT_STICK},
                {"rstick", SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {"lshoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
                {"rshoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"dpup", SDL_GAMEPAD_BUTTON_DPAD_UP},
                {"dpdown", SDL_GAMEPAD_BUTTON_DPAD_DOWN},   {"dpleft", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
                {"dpright", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
            };
            for (const auto &entry : names)
                if (name == entry.name)
                {
                    button = entry.button;
                    return true;
                }
            return false;
        }

        bool padAxisByName(const std::string &name, SDL_GamepadAxis &axis)
        {
            static const struct { const char *name; SDL_GamepadAxis axis; } names[] = {
                {"leftx", SDL_GAMEPAD_AXIS_LEFTX},           {"lefty", SDL_GAMEPAD_AXIS_LEFTY},
                {"rightx", SDL_GAMEPAD_AXIS_RIGHTX},         {"righty", SDL_GAMEPAD_AXIS_RIGHTY},
                {"lefttrigger", SDL_GAMEPAD_AXIS_LEFT_TRIGGER}, {"righttrigger", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
            };
            for (const auto &entry : names)
                if (name == entry.name)
                {
                    axis = entry.axis;
                    return true;
                }
            return false;
        }

        void createTestPad()
        {
            const char *file = std::getenv("PS2X_TEST_PAD");
            if (!file || !file[0])
                return;
            SDL_VirtualJoystickDesc desc;
            SDL_INIT_INTERFACE(&desc);
            desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
            desc.vendor_id = 0xDEAD; // the pair tools/drive_game.py lets through SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT
            desc.product_id = 0xBEEF;
            desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
            desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
            desc.name = "FPWR test pad";
            const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
            g.testPad.joystick = id ? SDL_OpenJoystick(id) : nullptr;
            if (!g.testPad.joystick)
            {
                std::fprintf(stderr, "[ui] PS2X_TEST_PAD: could not attach a virtual gamepad: %s\n", SDL_GetError());
                return;
            }
            g.testPad.file = file;
            std::printf("[ui] PS2X_TEST_PAD: virtual gamepad attached, reading commands from %s\n", file);
            std::fflush(stdout);
        }

        void runTestPadLine(const std::string &line)
        {
            char verb[16] = {};
            char name[32] = {};
            int value = 0;
            const int fields = std::sscanf(line.c_str(), "%15s %31s %d", verb, name, &value);
            if (fields < 2)
                return;
            if (std::strcmp(verb, "axis") == 0 && fields == 3)
            {
                SDL_GamepadAxis axis;
                if (padAxisByName(name, axis))
                    SDL_SetJoystickVirtualAxis(g.testPad.joystick, static_cast<int>(axis), static_cast<Sint16>(value));
                return;
            }
            SDL_GamepadButton button;
            if (padButtonByName(name, button))
                SDL_SetJoystickVirtualButton(g.testPad.joystick, static_cast<int>(button), std::strcmp(verb, "down") == 0);
        }

        // ---- what the controls page shows (ps2_pad.cpp has the real map) ----------------------------------------

#if defined(PS2X_HAS_UI)
        std::string assetRoot()
        {
            std::vector<std::filesystem::path> candidates;
            if (const char *env = std::getenv("PS2X_UI_ASSETS"))
                candidates.emplace_back(env);
            if (const char *base = SDL_GetBasePath())
                candidates.emplace_back(std::filesystem::path(base) / "ui_assets");
#if defined(PS2X_UI_SOURCE_ASSET_DIR)
            candidates.emplace_back(PS2X_UI_SOURCE_ASSET_DIR);
#endif
            for (const auto &candidate : candidates)
            {
                std::error_code ec;
                if (std::filesystem::exists(candidate / "pages" / "game_settings.rml", ec))
                    return candidate.string();
            }
            return std::string();
        }

        // The game side of the menu: settings in the store, applied live where they can be.
        class GameHost final : public ps2x::ui::MenuHost
        {
        public:
            std::string value(const std::string &key) override
            {
                if (key == "fullscreen")
                {
                    // The row follows the real window. Right after the menu asked for a switch the window may not have
                    // caught up (some systems take a moment): the stored value stands in for 2 s. Otherwise, if the
                    // window differs from what is stored (it was switched some other way), the store follows the window.
                    const std::string stored = ps2x_settings::get(key);
                    if (!ps2x_settings::fixedByEnvironment(key) && SDL_GetTicksNS() - g.fullscreenAskedNs < 2'000'000'000ull)
                        return stored;
                    const std::string actual = windowFullscreen() ? "on" : "off";
                    if (actual != stored && !ps2x_settings::fixedByEnvironment(key))
                        ps2x_settings::set(key, actual);
                    return actual;
                }
                return ps2x_settings::get(key);
            }
            std::string startupValue(const std::string &key) override { return ps2x_settings::startupValue(key); }

            bool fixedByEnvironment(const std::string &key, std::string &envName) override
            {
                if (!ps2x_settings::fixedByEnvironment(key))
                    return false;
                envName = ps2x_settings::envName(key);
                return true;
            }

            void change(const std::string &key, const std::string &value) override
            {
                if (!ps2x_settings::set(key, value))
                    return;
                const std::string now = ps2x_settings::get(key);
                std::printf("[ui] setting %s = %s\n", key.c_str(), now.c_str());
                std::fflush(stdout);
                // Settings that can change while the game runs. The others (renderer, resolution, aspect) are in the
                // store and take effect at the next start.
                if (key == "fullscreen")
                {
                    g.fullscreenAskedNs = SDL_GetTicksNS();
                    setWindowFullscreen(now == "on");
                }
                else if (key == "vsync")
                    gpu_present::setVsync(now == "on");
                else if (key == "filter")
                    gpu_present::setPictureFilter(now == "linear" ? ScaleMode::Bilinear : ScaleMode::Sharp);
                else if (key == "volume")
                    setMasterVolume(static_cast<float>(std::atoi(now.c_str())) / 100.0f);
            }

            void closeRequested() override { g.closeRequested = true; }

            void quitRequested() override { g.quitAsked = true; }

            std::string quitBlockedReason() override
            {
                // Until the guest has stopped its state is not known (it may be in the middle of a save).
                return g.quitChecked ? g.quitBlock : std::string("Waiting for the game to pause.");
            }

            std::vector<ps2x::ui::ControlLine> controls() override
            {
                return {
                    {"Move", "Arrow keys, W A S D", "D-pad, left stick"},
                    {"Cross", "X, Space", "A (bottom button)"},
                    {"Circle", "C, Escape", "B (right button)"},
                    {"Square", "Z, Keypad 0", "X (left button)"},
                    {"Triangle", "V, Keypad 1", "Y (top button)"},
                    {"L1 / R1", "Q / E", "LB / RB"},
                    {"L2 / R2", "Left Shift / Right Shift", "LT / RT"},
                    {"Start", "Enter", "Start"},
                    {"Select", "Tab", "Back"},
                    {"This menu", "F2", "Guide, or hold L3 + R3"},
                    {"Debug panel", "F1", "-"},
                };
            }

            std::string controllerName() override { return gamepadName(); }

            ps2x::mods::Manager *modManager() override { return ps2x_host_mods::manager(); }

            // The modder's tools at the foot of the Mods tab (task t2).
            std::string devPackLine() override
            {
                const std::string folder = ps2x_gs::devPackFolder();
                if (folder.empty())
                    return std::string();
                // One line, no wrapping: the folder when all is well, the newest complaint about a file when there is one.
                const auto notes = ps2x_gs::devPackNotes();
                if (!notes.empty())
                    return "Dev pack: " + notes.back();
                return "Dev pack folder: " + folder + (ps2x_gs::textureToolsOn() ? std::string() : std::string("  (Texture Tools are off)"));
            }
            // Data packs (task s2): which memory card the game is using, and "Keep this card".
            std::string cardLine() override { return ps2x_host_mods::cardLine(); }
            bool canKeepCard() override { return ps2x_host_mods::canKeepCard(); }
            std::string keepCard() override { return ps2x_host_mods::keepCard(); }
            std::string dumpTextures() override { return ps2x_gs::requestTextureDump(); }
            void openDumpsFolder() override { ps2x_gs::openFolder(ps2x_gs::dumpsFolder()); }
            std::string lastDumpResult() override
            {
                bool busy = false;
                const std::string message = ps2x_gs::textureDumpMessage(&busy);
                return busy ? std::string() : message;
            }
        };

        GameHost &gameHost()
        {
            static GameHost host;
            return host;
        }

        ps2x::ui::GameMenu &menu()
        {
            static ps2x::ui::GameMenu m;
            return m;
        }
#endif

#if defined(PS2X_HAS_UI)
        // Once per opening of the menu, as soon as the guest has really stopped: is the memory card being written? Sets
        // Quit Game's state (see the comment at the top of this file).
        void checkQuit()
        {
            if (g.quitChecked || !guestBlocked() || guestBlockEntries() == g.blockEntriesAtOpen)
                return;
            const CardWriteStatus card = cardWriteStatus();
            g.quitBlock = cardQuitBlockReason(card);
            g.quitChecked = true;
            std::printf("[ui] Quit Game %s (card files open for writing: %u, ms since the last card write: %lld)\n",
                        g.quitBlock.empty() ? "is available" : "is dimmed", card.writeHandlesOpen,
                        card.anyWriteYet ? static_cast<long long>(card.msSinceLastWrite) : -1ll);
            std::fflush(stdout);
            menu().refresh();
        }
#endif

        void openOverlay(const char *how)
        {
#if defined(PS2X_HAS_UI)
            if (!g.created || g.visible.load())
                return;
            setInputCapturedByOverlay(true);
            // Quit Game starts dimmed ("Waiting for the game to pause.") until the guest has stopped: see checkQuit.
            g.quitChecked = false;
            g.quitBlock.clear();
            g.quitAsked = false;
            g.blockEntriesAtOpen = guestBlockEntries();
            menu().open();
            if (!menu().visible())
            {
                setInputCapturedByOverlay(false);
                return;
            }
            g.visible.store(true);
            g.openedAtNs = SDL_GetTicksNS();
            g.lastUpdateNs = g.openedAtNs;
            setGuestPaused(true);
            std::printf("[ui] overlay opened (%s)\n", how);
            std::fflush(stdout);
#else
            (void)how;
#endif
        }

        void closeOverlay(const char *how)
        {
#if defined(PS2X_HAS_UI)
            if (!g.created || !g.visible.load())
                return;
            menu().close();
            g.visible.store(false);
            ps2x_settings::save(); // Back saves
            // The input comes back first, then the game: the first frame it runs reads the real state.
            setInputCapturedByOverlay(false);
            setGuestPaused(false);
            std::printf("[ui] overlay closed (%s) after %.2f s\n", how,
                        static_cast<double>(SDL_GetTicksNS() - g.openedAtNs) / 1e9);
            std::fflush(stdout);
#else
            (void)how;
#endif
        }

        void toggleOverlay(const char *how)
        {
            if (g.visible.load())
                closeOverlay(how);
            else
                openOverlay(how);
        }

        // A file was dropped on the window (SDL_EVENT_DROP_FILE, or a line of the PS2X_TEST_DROP file): the mod manager installs
        // it if it is a good .fprmod, and the player is shown the answer on the Mods tab, which opens for the purpose (the game
        // pauses while the menu is open, as always). Without an overlay the answer only goes to the log.
        void handleDroppedFile(const std::string &path)
        {
            const ps2x_host_mods::DropOutcome outcome = ps2x_host_mods::installDropped(path);
#if defined(PS2X_HAS_UI)
            if (!outcome.handled || !g.created)
                return;
            if (!g.visible.load())
                openOverlay("file dropped");
            if (g.visible.load())
                menu().showMods(outcome.message);
#else
            (void)outcome;
#endif
        }

        // PS2X_TEST_DROP=<file>: for scripted runs. Each line the script appends to the file is "drop <path>" (the rest of the line is
        // the path, spaces included) and does what dropping that file on the window does, from the same function the real
        // SDL_EVENT_DROP_FILE event calls. (A script cannot drag a file onto a window from outside the process.)
        struct TestDrop
        {
            bool checked = false;
            std::string file;
            uint64_t offset = 0;
            std::string pending;
        } testDrop;

        void pumpTestDrop()
        {
            if (!testDrop.checked)
            {
                testDrop.checked = true;
                if (const char *env = std::getenv("PS2X_TEST_DROP"))
                    testDrop.file = env;
            }
            if (testDrop.file.empty())
                return;
            const size_t newline = testDrop.pending.find('\n');
            if (newline != std::string::npos)
            {
                std::string line = testDrop.pending.substr(0, newline);
                testDrop.pending.erase(0, newline + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.compare(0, 5, "drop ") == 0)
                {
                    std::printf("[ui] PS2X_TEST_DROP: dropping %s\n", line.c_str() + 5);
                    std::fflush(stdout);
                    handleDroppedFile(line.substr(5));
                }
                return;
            }
            std::error_code ec;
            const uint64_t size = std::filesystem::file_size(testDrop.file, ec);
            if (ec || size <= testDrop.offset)
                return;
            std::FILE *file = std::fopen(testDrop.file.c_str(), "rb");
            if (!file)
                return;
            std::fseek(file, static_cast<long>(testDrop.offset), SEEK_SET);
            std::string chunk(static_cast<size_t>(size - testDrop.offset), '\0');
            const size_t got = std::fread(chunk.data(), 1, chunk.size(), file);
            std::fclose(file);
            chunk.resize(got);
            testDrop.offset += got;
            testDrop.pending += chunk;
        }
    }

    bool create(SDL_Window *window, SDL_GPUDevice *device, SDL_GPUTextureFormat swapchainFormat)
    {
        if (g.created)
            return true;
        if (envFlagOff("PS2X_UI"))
        {
            std::printf("[ui] PS2X_UI=0: the settings overlay is off\n");
            std::fflush(stdout);
            return false;
        }
#if defined(PS2X_HAS_UI)
        const std::string root = assetRoot();
        if (root.empty())
        {
            std::fprintf(stderr, "[ui] no settings overlay: the ui_assets folder is not next to the program (PS2X_UI_ASSETS can name it)\n");
            return false;
        }
        ps2x::ui::OverlayConfig config;
        config.device = device;
        config.targetFormat = swapchainFormat;
        config.assetRoot = root;
        config.window = window;
        // Theme mods: the style sheets of the mods that were on when the game started go after the game's own theme, and the
        // overlay opens their files (and the pictures of the Mods tab) through the manager.
        config.extraStyleSheets = ps2x_host_mods::themeStyleSheets();
        config.virtualFiles = [](const std::string &name, std::vector<uint8_t> &bytes) { return ps2x_host_mods::readVirtualFile(name, bytes); };
        if (!menu().init(config, &gameHost()))
        {
            std::fprintf(stderr, "[ui] no settings overlay: it could not start (see above)\n");
            return false;
        }
        g.window = window;
        g.created = true;
        g.testOpenOnCardWrite = std::getenv("PS2X_TEST_OPEN_ON_CARD_WRITE") != nullptr && std::getenv("PS2X_TEST_OPEN_ON_CARD_WRITE")[0] == '1';
        createTestPad();
        std::printf("[ui] settings overlay ready (assets %s). Open it with F2, the Guide button, or L3 + R3 held.\n", root.c_str());
        if (!config.extraStyleSheets.empty())
        {
            std::printf("[ui] theme mods: %zu style sheet(s) added after the game's own theme:", config.extraStyleSheets.size());
            for (const std::string &sheet : config.extraStyleSheets)
                std::printf(" %s", sheet.c_str());
            std::printf("\n");
        }
        std::fflush(stdout);
        return true;
#else
        (void)window;
        (void)device;
        (void)swapchainFormat;
        std::printf("[ui] this build has no settings overlay (PS2X_BUILD_UI is off)\n");
        std::fflush(stdout);
        return false;
#endif
    }

    void destroy()
    {
        if (!g.created)
            return;
#if defined(PS2X_HAS_UI)
        if (g.visible.load())
            closeOverlay("shutdown");
        else
            ps2x_settings::save();
        menu().shutdown();
#endif
        if (g.testPad.joystick)
        {
            const SDL_JoystickID id = SDL_GetJoystickID(g.testPad.joystick);
            SDL_CloseJoystick(g.testPad.joystick);
            SDL_DetachVirtualJoystick(id);
            g.testPad.joystick = nullptr;
        }
        g.created = false;
    }

    bool active()
    {
        return g.created;
    }

    bool visible()
    {
        return g.visible.load();
    }

    bool handleEvent(const SDL_Event &event)
    {
        // A file dropped on the window is a request to install a mod, with or without the overlay.
        if (event.type == SDL_EVENT_DROP_FILE && event.drop.data && event.drop.data[0] != '\0')
        {
            handleDroppedFile(event.drop.data);
            return true;
        }
        if (!g.created)
            return false;
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F2)
        {
            toggleOverlay("F2");
            return true;
        }
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)
        {
            toggleOverlay("Guide button");
            return true;
        }
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
        {
            const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            if (event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK)
                g.l3 = down;
            else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK)
                g.r3 = down;
            if (g.l3 && g.r3 && g.chordSinceNs == 0)
                g.chordSinceNs = SDL_GetTicksNS();
            if (!(g.l3 && g.r3))
            {
                g.chordSinceNs = 0;
                g.chordFired = false;
            }
        }
#if defined(PS2X_HAS_UI)
        if (g.visible.load())
            return menu().overlay().handleEvent(event);
#endif
        return false;
    }

    void update()
    {
        if (!g.created)
            return;
#if defined(PS2X_HAS_UI)
        const uint64_t now = SDL_GetTicksNS();
        if (g.l3 && g.r3 && !g.chordFired && g.chordSinceNs != 0 && now - g.chordSinceNs >= kChordHoldNs)
        {
            g.chordFired = true;
            toggleOverlay("L3 + R3 held");
        }
        if (g.closeRequested)
        {
            g.closeRequested = false;
            closeOverlay("Back");
        }
        // Test hook (tests only): open the menu the moment the game has a file open for writing on the card. With
        // PS2X_TEST_CARD_HOLD_MS (the stubs) the game waits there, so the menu is open in the middle of a save.
        if (g.testOpenOnCardWrite && !g.visible.load() && cardActivityState().writeHandlesOpen.load() > 0)
        {
            g.testOpenOnCardWrite = false;
            openOverlay("test: card file open for writing");
        }
        if (g.visible.load())
        {
            checkQuit();
            if (g.quitAsked)
            {
                g.quitAsked = false;
                if (g.quitChecked && g.quitBlock.empty())
                {
                    // Save changed settings first; the shutdown path saves them again if anything is still unsaved.
                    ps2x_settings::save();
                    std::printf("[ui] Quit Game confirmed: stopping the game\n");
                    std::fflush(stdout);
                    requestQuit();
                }
            }
            ps2x::ui::Overlay &overlay = menu().overlay();
            int width = 0, height = 0;
            SDL_GetWindowSizeInPixels(g.window, &width, &height);
            overlay.setPixelDensity(SDL_GetWindowPixelDensity(g.window));
            overlay.setViewport(width, height);
            const double dt = std::min(0.1, static_cast<double>(now - g.lastUpdateNs) / 1e9);
            g.lastUpdateNs = now;
            overlay.update(dt);
        }
#endif
    }

    void record(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUTexture *swapchain, uint32_t width, uint32_t height)
    {
#if defined(PS2X_HAS_UI)
        if (!g.created || !g.visible.load())
            return;
        ps2x::ui::Overlay &overlay = menu().overlay();
        overlay.setViewport(static_cast<int>(width), static_cast<int>(height));
        ps2x_gs::DebugPassLock lock; // validation only: see gs_gpu_threads.h
        overlay.record(commandBuffer, swapchain);
#else
        (void)commandBuffer;
        (void)swapchain;
        (void)width;
        (void)height;
#endif
    }

    void pumpTestPad()
    {
        pumpTestDrop(); // PS2X_TEST_DROP: files a script "dropped" on the window
        if (!g.testPad.joystick)
            return;
        // One command per call, so SDL sees every state for at least one event pump: a button set down and up again before
        // SDL looked would otherwise never be seen to go down (frames can be slow, for instance in a large window).
        const size_t newline = g.testPad.pending.find('\n');
        if (newline != std::string::npos)
        {
            runTestPadLine(g.testPad.pending.substr(0, newline));
            g.testPad.pending.erase(0, newline + 1);
            return;
        }
        std::error_code ec;
        const uint64_t size = std::filesystem::file_size(g.testPad.file, ec);
        if (ec || size <= g.testPad.offset)
            return;
        std::FILE *file = std::fopen(g.testPad.file.c_str(), "rb");
        if (!file)
            return;
        std::fseek(file, static_cast<long>(g.testPad.offset), SEEK_SET);
        std::string chunk(static_cast<size_t>(size - g.testPad.offset), '\0');
        const size_t got = std::fread(chunk.data(), 1, chunk.size(), file);
        std::fclose(file);
        chunk.resize(got);
        g.testPad.offset += got;
        g.testPad.pending += chunk;
    }
}
