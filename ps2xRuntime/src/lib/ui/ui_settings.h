// Behaviour of the settings page (assets/pages/settings.rml): tabs, toggle rows, choice rows, slider rows and
// the help line. The page itself is plain RML and RCSS; RmlUi has no scripting here, so the few things a page
// needs to do are done by this class, which listens to the page's events.
//
// A page is built from these parts (the attributes are what this class reads):
//
//   .tab[data-tab=NAME]                    a tab button; shows .panel#panel-NAME and hides the others
//   .row[data-kind=toggle|choice|slider]   one setting, focusable (tab-index: auto) so the controller can reach it
//        data-key=KEY                      the setting's name, handed to the change callback
//        data-value=VALUE                  the current value (toggle: on/off; choice: one of data-choices; slider: a number)
//        data-choices=A|B|C                choice rows only
//        data-labels=a|b|c                 choice rows only, optional: what is shown for each choice (default: the value)
//        data-restart=1                    the setting only takes effect after a restart (the host can flag it)
//        data-min / data-max / data-step   slider rows only (default 0, 100, 5), data-suffix e.g. "%"
//        data-help=TEXT                    shown in #help-text while the row has focus
//        .value                            the element that shows "[ON]", "[16:9]" or "[ 50%]"
//        .slider .track, .fill             the slider's track (mouse hits) and the filled part (its width is set)
//   #help-text, #back                      the help line and the Back button
//   #quit, #confirm, #window               optional. A Quit Game button that asks first: a click shows #confirm (a layer over
//        #confirm-yes, #confirm-no         the page with a question and two buttons; #window gets the class "confirming" so
//                                          nothing behind it takes the focus), Cancel (#confirm-no) has the focus, Escape or
//                                          the B button cancels, and only #confirm-yes calls the quit callback.
//
// Keys on a row: Left/Right change the value (toggle: flip, choice: previous/next with wrap, slider: step), Return
// or a click activates (toggle: flip, choice: next). Page Up / Page Down (the controller's shoulder buttons) switch
// tabs, Escape (the B button) is Back. Up/Down and the tab bar are plain RmlUi focus navigation.
//
// A row can be locked (setLocked): it stays focusable, so the help line can say why, but its value cannot change and
// it is drawn dimmed. A page may also have a `.tag` element in a row, which shows a short word ("ENV", "RESTART").
//
// Nothing here changes the game. The host sets a callback to learn about changes (and to learn that the player
// confirmed Quit Game).
#pragma once

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

namespace ps2x::ui
{
    class SettingsPage final : public Rml::EventListener
    {
    public:
        using ChangeCallback = std::function<void(const std::string &key, const std::string &value)>;

        SettingsPage() = default;
        ~SettingsPage() override;
        SettingsPage(const SettingsPage &) = delete;
        SettingsPage &operator=(const SettingsPage &) = delete;

        // Wires the page's events. Returns false (and logs why) if it lacks the pieces listed above. `document` must
        // outlive the page or be closed after detach().
        bool attach(Rml::ElementDocument *document);
        void detach();

        void setOnChange(ChangeCallback callback) { m_onChange = std::move(callback); }
        void setOnBack(std::function<void()> callback) { m_onBack = std::move(callback); }
        void setOnQuit(std::function<void()> callback) { m_onQuit = std::move(callback); }

        // Reads and writes a setting by key. setValue updates the display and does not call the change callback.
        std::string value(const std::string &key) const;
        bool setValue(const std::string &key, const std::string &value);
        // The keys of every row, in page order, and a row's attribute (for example "data-restart"); "" if there is none.
        std::vector<std::string> keys() const;
        std::string attribute(const std::string &key, const std::string &name) const;
        // Locks or unlocks a row. `reason` replaces the help text while the row is focused; `tag` goes in the row's
        // .tag element ("" clears it).
        bool setLocked(const std::string &key, bool locked, const std::string &reason, const std::string &tag);
        bool setTag(const std::string &key, const std::string &tag);
        // Dims the Quit Game button and makes it do nothing; `reason` replaces its help text while it is focused. An empty
        // reason unlocks it. False if the page has no #quit.
        bool setQuitLocked(const std::string &reason);
        // Hides the question if it is showing (the menu was closed or opened again).
        void closeConfirm();
        bool confirming() const { return m_confirming; }

        void selectTab(const std::string &name);
        std::string selectedTab() const { return m_tab; }

        // Puts the focus on the first row of the current tab (what the overlay does when it opens).
        void focusFirstRow();

        void ProcessEvent(Rml::Event &event) override;

    private:
        Rml::Element *rowForKey(const std::string &key) const;
        Rml::Element *rowOf(Rml::Element *element) const;
        Rml::Element *panelOf(Rml::Element *element) const;
        void refreshRow(Rml::Element *row);
        void changeRow(Rml::Element *row, int direction, bool activate);
        void setSliderFromMouse(Rml::Element *row, float mouseX);
        void showHelp(Rml::Element *element);
        void switchTab(int delta);
        void openConfirm();
        void leaveConfirm(bool focusQuit);

        Rml::ElementDocument *m_document = nullptr;
        std::string m_tab;
        Rml::Element *m_dragRow = nullptr; // the slider row being dragged with the mouse
        bool m_confirming = false;         // the Quit Game question is showing
        ChangeCallback m_onChange;
        std::function<void()> m_onBack;
        std::function<void()> m_onQuit;
    };
}
