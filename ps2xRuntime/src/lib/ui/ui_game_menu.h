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
//     started with, and a line at the bottom says so;
//   - Quit Game asks "Quit to the desktop?" first (Cancel has the focus) and only then tells the host; the host can keep
//     the button dimmed, with a reason in the help line (quitBlockedReason);
//   - the Mods tab (ui_mods.h) is there when the host gives the menu a mod manager (MenuHost::modManager); a change to the
//     mods that is for the next start adds "Mods" to the "Restart the game to apply" note.
//
// Not thread-safe: the host's main thread, like Overlay.
#pragma once

#include "ui_overlay.h"

#include <memory>
#include <string>
#include <vector>

namespace ps2x::mods
{
    class Manager;
}

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
        // The player answered "Quit to the desktop?" with Quit. The host saves its settings and ends the game, as closing the
        // window does (not from inside this call: the page is still handling an event; set a flag and quit afterwards).
        virtual void quitRequested() {}
        // "" when Quit Game can be used now. Otherwise one sentence saying why not, for the help line ("The game is writing to
        // the memory card..."); the button is dimmed and does nothing. Asked on open and after every refresh().
        virtual std::string quitBlockedReason() { return std::string(); }
        // The lines of the Controls page, and the name of the controller in use ("" if none).
        virtual std::vector<ControlLine> controls() = 0;
        virtual std::string controllerName() { return std::string(); }
        // The mod manager the Mods tab works on, or nullptr: the tab is then left out of the menu. It must outlive the menu.
        virtual ps2x::mods::Manager *modManager() { return nullptr; }
        // The modder's tools shown at the foot of the Mods tab (task t2): where the dev pack folder is, and the dump button. An empty
        // devPackLine leaves the tools out. dumpTextures returns one line saying what started (or why not); openDumpsFolder opens the
        // folder dumps go to; lastDumpResult is the result of the last dump ("" if none yet).
        virtual std::string devPackLine() { return std::string(); }
        virtual std::string dumpTextures() { return std::string(); }
        virtual void openDumpsFolder() {}
        virtual std::string lastDumpResult() { return std::string(); }
        // Data packs (task s2): the line under the mods saying which memory card the game is using ("" leaves it out), whether "Keep this
        // card" has a profile save to copy, and doing it (one sentence back; the menu has already asked "are you sure").
        virtual std::string cardLine() { return std::string(); }
        virtual bool canKeepCard() { return false; }
        virtual std::string keepCard() { return std::string(); }
        // Task s3: "Back to my card" (the game plays from the player's own card again from the next start). canBackToOwnCard() is true only
        // while no data pack is on and the profile has a save; backToOwnCard() records the choice (the menu has already asked) and returns
        // one sentence.
        virtual bool canBackToOwnCard() { return false; }
        virtual std::string backToOwnCard() { return std::string(); }
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
        // Shows the Mods tab with `message` under the list ("Installed X.", "That is not a mod file."). The host calls it after a file was
        // dropped on the window; it opens the menu first if it is not open.
        void showMods(const std::string &message);

        // The overlay for events, update and record (see ui_overlay.h).
        Overlay &overlay();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
