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
//        data-min / data-max / data-step   slider rows only (default 0, 100, 5), data-suffix e.g. "%"
//        data-help=TEXT                    shown in #help-text while the row has focus
//        .value                            the element that shows "[ON]", "[16:9]" or "[ 50%]"
//        .slider .track, .fill             the slider's track (mouse hits) and the filled part (its width is set)
//   #help-text, #back                      the help line and the Back button
//
// Keys on a row: Left/Right change the value (toggle: flip, choice: previous/next with wrap, slider: step), Return
// or a click activates (toggle: flip, choice: next). Page Up / Page Down (the controller's shoulder buttons) switch
// tabs, Escape (the B button) is Back. Up/Down and the tab bar are plain RmlUi focus navigation.
//
// Nothing here changes the game. The host sets a callback to learn about changes.
#pragma once

#include <RmlUi/Core/EventListener.h>

#include <functional>
#include <string>

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

        // Reads and writes a setting by key. setValue updates the display and does not call the change callback.
        std::string value(const std::string &key) const;
        bool setValue(const std::string &key, const std::string &value);

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

        Rml::ElementDocument *m_document = nullptr;
        std::string m_tab;
        Rml::Element *m_dragRow = nullptr; // the slider row being dragged with the mouse
        ChangeCallback m_onChange;
        std::function<void()> m_onBack;
    };
}
