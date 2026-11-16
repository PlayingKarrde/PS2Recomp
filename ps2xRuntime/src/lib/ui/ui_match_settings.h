// Greyblock proof of concept: a modern match settings screen (assets/pages/match_settings.rml, theme/greyblock.rcss).
//
// What it replaces, in the game: the Exhibition match type list and the match settings screen (docs/ui-map.md,
// `modes.exhibition` and `modes.exhibition.*settings`). The game shows a seven-entry type list, then one 23-row list split
// over two panes, with rows that do not apply drawn as "-" and long lists in popups next to the row.
//
// What this page does instead (the design is in docs/ui-greyblock-match-settings.md):
//   - one hub: the match type is a row of chips at the top (LB/RB from anywhere), and the rows follow the type;
//   - the two corners first, as cards (controller CPU / P1..P8, the wrestler is picked on the next screen);
//   - rows grouped into sections; a row a type does not use is hidden, a row a type fixes stays, locked, with the reason;
//   - Left/Right change a value in place; A on a long list (Ring, Venue, Referee, Music) opens a picker;
//   - a preview card that says in one line what match this is;
//   - the game's "Settings 1-4" shown as four presets (each keeps its own values).
//
// It is not connected to the game: the values live in this class. The option lists and defaults come from the UI map;
// rows the map could not confirm are marked unverified (help line). The class follows SettingsPage's pattern: plain RML and
// RCSS, behaviour here, wired through the document's events.
//
// Keys (the overlay's Input turns the controller into these): Up/Down move, Left/Right change, Return (A) activates,
// Escape (B) closes the picker or goes back, Page Up/Page Down (LB/RB) change the match type.
#pragma once

#include <RmlUi/Core/EventListener.h>

#include <array>
#include <functional>
#include <map>
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
    class MatchSettingsPage final : public Rml::EventListener
    {
    public:
        MatchSettingsPage();
        ~MatchSettingsPage() override;
        MatchSettingsPage(const MatchSettingsPage &) = delete;
        MatchSettingsPage &operator=(const MatchSettingsPage &) = delete;

        // Wires the page. False (and logs) if the page lacks #types, #list, #corners, #continue or #picker.
        bool attach(Rml::ElementDocument *document);
        void detach();

        // Back with nothing open (B / Escape). Continue (the Continue button), with summary() as the argument.
        void setOnBack(std::function<void()> callback) { m_onBack = std::move(callback); }
        void setOnContinue(std::function<void(const std::string &)> callback) { m_onContinue = std::move(callback); }

        // For scripted checks. Keys: a row's key ("time", "ring"...), "type", "preset", "blue", "red", "focus" (the focused
        // item's id), "picker" (open or closed), "summary", "visible-rows" (keys of the rows shown, joined by |).
        std::string value(const std::string &key) const;

        void ProcessEvent(Rml::Event &event) override;

        // The model, public so the .cpp's tables can be plain aggregates.
        enum class Avail : unsigned char
        {
            Hidden,   // the type does not use the row
            Edit,     // the player can change it
            Fixed     // the type sets it; shown locked
        };
        struct RowRule
        {
            Avail avail = Avail::Hidden;
            std::vector<std::string> values; // empty: the row's base values
            std::string def;                 // default (or the fixed value)
            bool unverified = false;         // the map did not confirm this row's state for the type
        };

    private:
        struct Item; // one focusable thing, in navigation order

        void rebuild();                 // the whole dynamic part, after a type or preset change
        void buildTypes();
        void buildCorners();
        void buildList();
        void buildSections();
        void refreshPreview();
        void refreshFocus(bool scroll);
        void move(int delta);
        void change(int delta);
        void activate();
        void setType(int index);
        void setPreset(int index);
        void openPicker();
        void closePicker(bool apply);
        void pickerMove(int delta);
        std::string &valueRef(const std::string &key);
        const RowRule &rule(int type, const std::string &key) const;
        std::string summary() const;
        int itemAt(Rml::Element *element) const;

        Rml::ElementDocument *m_document = nullptr;
        std::vector<Item> m_items;
        int m_focus = 0;
        int m_type = 0;
        int m_preset = 0;
        // values[preset][type][row key]; corners per preset
        std::array<std::vector<std::map<std::string, std::string>>, 4> m_values;
        std::array<std::array<std::string, 2>, 4> m_corners;
        // picker
        bool m_picking = false;
        std::string m_pickKey;
        int m_pickIndex = 0;
        std::function<void()> m_onBack;
        std::function<void(const std::string &)> m_onContinue;
    };
}
