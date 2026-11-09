// The game's settings menu: the overlay, the settings page (assets/pages/game_settings.rml) and the rules that tie the
// page to the game's settings.
//
// The overlay (ui_overlay.h) and the page behaviour (ui_settings.h) are generic. This class is the part that knows what
// the game offers: which settings can be changed live, which need a restart, which are fixed by an environment
// variable, and which depend on others. It still knows nothing about how the game applies a setting; the host
// implements MenuHost for that, and the demo or a test can implement it with a map.
//
// What the menu does with a setting, in the player's words:
//   - a setting fixed by an environment variable is dimmed, tagged ENV and cannot be changed ("Set by PS2X_GS_SCALE.
//     Unset it to change it here.");
//   - a setting that needs the GPU renderer is dimmed while the renderer setting says CPU;
//   - a setting that only takes effect after a restart is tagged RESTART once it differs from what the running game
//     started with, and a line at the bottom says so.
//
// Not thread-safe: the host's main thread, like Overlay.
#pragma once

#include "ui_overlay.h"

#include <memory>
#include <string>
#include <vector>

namespace ps2x::ui
{
    // One line of the read-only Controls page.
    struct ControlLine
    {
        std::string action;     // "Cross", "Start"...
        std::string keyboard;   // "X, Space"
        std::string controller; // "A"
    };

    // What the menu asks of the game side.
    class MenuHost
    {
    public:
        virtual ~MenuHost() = default;
        // The setting's value now (what the menu shows), and the value the running game started with.
        virtual std::string value(const std::string &key) = 0;
        virtual std::string startupValue(const std::string &key) = 0;
        // True when an environment variable fixes the setting; `envName` gets its name.
        virtual bool fixedByEnvironment(const std::string &key, std::string &envName) = 0;
        // The player changed a setting in the menu. The host stores it and applies it if it can be applied live.
        virtual void change(const std::string &key, const std::string &value) = 0;
        // Back, B or Escape: the player is done. The host saves and closes the menu (not from inside this call:
        // the page is still handling an event; set a flag and close afterwards).
        virtual void closeRequested() = 0;
        // The lines of the Controls page, and the name of the controller in use ("" if none).
        virtual std::vector<ControlLine> controls() = 0;
        virtual std::string controllerName() { return std::string(); }
    };

    class GameMenu
    {
    public:
        GameMenu();
        ~GameMenu();
        GameMenu(const GameMenu &) = delete;
        GameMenu &operator=(const GameMenu &) = delete;

        // Creates the overlay (fonts and theme load now; the page loads the first time the menu opens). `host` must
        // outlive the menu.
        bool init(const OverlayConfig &config, MenuHost *host);
        void shutdown();
        bool ready() const;

        // Shows the menu: loads the page on first use, reads every setting from the host, focuses the first row.
        void open();
        void close();
        bool visible() const;
        // Reads the settings again (after the host changed something behind the menu's back).
        void refresh();

        // The overlay for events, update and record (see ui_overlay.h).
        Overlay &overlay();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
