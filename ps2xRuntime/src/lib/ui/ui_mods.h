// The Mods tab of the game's settings menu: the list of installed mods on the left, the selected mod's details and actions on the
// right. It is the behaviour of the #panel-mods part of assets/pages/game_settings.rml, in the same way SettingsPage (ui_settings.h)
// is the behaviour of the settings rows. It talks to the mod manager (src/lib/mods/mod_manager.h) and knows nothing else about the game.
//
// What the player can do (mouse, keyboard and controller; the list and the buttons are focusable like every other part of the menu):
//   - move along the list: the details follow the focused mod (hovering a row with the mouse focuses it);
//   - A / Return / a click on a row turns the mod on or off; the "Turn On" button does the same;
//   - Move Up / Move Down change the load order ("mods lower in the list load later and win");
//   - Uninstall asks first ("Uninstall <name>?", Cancel focused, B or Escape cancels) and then moves the file to the .trash folder;
//   - Next Picture steps through the mod's thumbnail and screenshots when there is more than one;
//   - a mod's own settings (bool, choice, int) are rows in the details: Left/Right change them;
//   - Load Now (task h1) applies data-pack changes to the running game without a restart; it asks first. Restart Now (beside the menu's
//     restart note) closes the game and starts it again; it asks first too.
// A mod that cannot be turned on shows why (a missing dependency, a conflict, a wrong game, a broken package) instead of failing
// silently; the line under the list says what the last action did. All changes apply when the game starts again, which the menu's
// "Restart needed" note says.
//
// Not thread-safe: the host's main thread, like the rest of the overlay.
#pragma once

#include "ui_game_menu.h"

#include <RmlUi/Core/EventListener.h>

#include <functional>
#include <string>
#include <vector>

namespace Rml
{
    class Element;
    class ElementDocument;
    class Event;
}

namespace ps2x::mods
{
    class Manager;
}

namespace ps2x::ui
{
    class ModsPage final : public Rml::EventListener
    {
    public:
        ModsPage() = default;
        ~ModsPage() override;
        ModsPage(const ModsPage &) = delete;
        ModsPage &operator=(const ModsPage &) = delete;

        // Wires the page's events. The page needs #mods-list, #mod-details, #mods-status, #mods-folder and the #mod-confirm layer.
        // `manager` must outlive the page. Returns false (and logs) when the page lacks those.
        bool attach(Rml::ElementDocument *document, ps2x::mods::Manager *manager);
        void detach();

        // Called after anything that may change which mods are loaded at the next start (the menu refreshes its "Restart needed" note).
        void setOnChange(std::function<void()> callback) { m_onChange = std::move(callback); }

        // The modder's tools at the foot of the tab (#dev-folder, #dump-textures, #open-dumps; the page may lack them): the dev pack folder
        // line and the buttons "Dump Textures" (arms a dump of the next frames; the game is paused while this menu is open, so the frames come
        // after it closes) and "Dumps Folder". With no devPackLine the tools are hidden.
        struct Tools
        {
            std::function<std::string()> devPackLine;
            std::function<std::string()> dumpTextures;
            std::function<void()> openDumpsFolder;
            std::function<std::string()> lastDumpResult;
            // Data packs: the line saying which memory card is in use (#mods-card), and "Keep this card" (#keep-card: shown only while
            // canKeepCard() is true; asks first; keepCard() does it and returns one sentence).
            std::function<std::string()> cardLine;
            std::function<bool()> canKeepCard;
            std::function<std::string()> keepCard;
            // "Back to my card" (#back-card: shown only while canBackToOwnCard() is true; asks first; backToOwnCard() records the choice and
            // returns one sentence).
            std::function<bool()> canBackToOwnCard;
            std::function<std::string()> backToOwnCard;
            // Task s4b, "Import a Save..." (#import-save: shown while canImportSave() is true). importSave() starts the file dialog (the answer
            // comes later, through the host) and returns one sentence for the status line.
            std::function<bool()> canImportSave;
            std::function<std::string()> importSave;
            // Task h1: "Load Now" (#load-now and the line #mods-load-note in #mods-apply). loadOffer() says whether it shows and whether it
            // works; loadNow() runs after "Load now?" was answered Yes and returns one sentence for the status line.
            std::function<LoadOffer()> loadOffer;
            std::function<std::string()> loadNow;
            // "Restart Now" (#restart-now, shown while the restart note shows): canRestart() is false where the platform cannot start the game
            // again; restartBlockedReason() is "" or why not now; restartNow() runs after "Restart the game?" was answered Yes.
            std::function<bool()> canRestart;
            std::function<std::string()> restartBlockedReason;
            std::function<void()> restartNow;
        };
        void setTools(Tools tools);

        // Makes the mod with this key the one whose page shows (the next refresh() draws it); a mod that is not there is ignored.
        void selectMod(const std::string &key);
        // Builds the list and the details again from the manager, keeping the selected mod if it is still there.
        void refresh();
        // The line under the list.
        void setStatus(const std::string &message);
        // The focus goes to the selected mod (or the first one); with no mods, to the tab.
        void focusList();
        // "Which save?": the file the player picked holds more than one save of the game. The confirm layer shows `body` and one button per
        // label (at most three) and Cancel; `picked(i)` is called with the index of the label chosen (not for Cancel). The focus starts on Cancel.
        void askWhichSave(const std::string &body, const std::vector<std::string> &labels, std::function<void(size_t)> picked);
        // The question about uninstalling is showing.
        bool confirming() const { return m_confirming; }
        void closeConfirm();
        // Shows or hides Load Now and its line, and Restart Now, from the host (the menu calls it with every refresh). `restartNeeded` is
        // whether the menu's restart note is showing.
        void updateApplyRow(bool restartNeeded);

        void ProcessEvent(Rml::Event &event) override;

    private:
        Rml::Element *byId(const char *id) const;
        void renderList();
        void renderDetails();
        void select(const std::string &key);
        void changed();
        void act(const std::string &action, bool fromRow = false);
        void changeSetting(Rml::Element *row, int direction);
        void openConfirm();
        void leaveConfirm(bool refocus);
        void showHelp(const std::string &text);
        void listenToFocus(Rml::Element *root);
        void focusAction(const std::string &action);
        void focusSelectedRow();

        Rml::ElementDocument *m_document = nullptr;
        ps2x::mods::Manager *m_manager = nullptr;
        std::string m_selected;   // the key of the mod whose details show
        size_t m_picture = 0;     // which of the mod's pictures shows
        bool m_confirming = false;
        std::string m_confirmKey; // the mod the question is about
        // What the question on the confirm layer is about.
        enum class Ask { Uninstall, KeepCard, BackToOwnCard, PickSave, LoadNow, Restart };
        std::function<void(size_t)> m_picked;
        void leavePick(size_t index);
        Ask m_ask = Ask::Uninstall;
        void openKeepConfirm();
        void openBackConfirm();
        void openCardConfirm(Ask ask, const char *title, const char *body, const char *yes, const char *yesHelp);
        void updateCardRow();
        void openLoadConfirm();
        void openRestartConfirm();
        bool m_restartNeeded = false;
        bool m_loadNowShown = false; // the Load Now button is on the tab (the controller reaches it from the list and the mod's buttons)
        std::function<void()> m_onChange;
        Tools m_tools;
    };
}
