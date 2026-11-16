// Greyblock proof of concept: the match settings hub. See ui_match_settings.h and docs/ui-greyblock-match-settings.md.

#include "ui_match_settings.h"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cstdio>

namespace ps2x::ui
{
    namespace
    {
        using Avail = MatchSettingsPage::Avail;
        using RowRule = MatchSettingsPage::RowRule;
        using List = std::vector<std::string>;

        // ---- The model: rows, sections, match types. Labels, values and defaults are the game's (docs/ui-map.md). ----------

        struct RowDef
        {
            const char *key;
            const char *label;
            int section;
            List values;
            const char *help;
            bool picker = false; // long list: A opens a picker
            const char *thumb = nullptr; // what the picker's and preview's placeholder box stands for
        };

        const char *const kSections[] = {"Match", "Rules", "Presentation", "CPU & speed"};
        constexpr int kSectionCount = 4;

        const List kOnOff = {"ON", "OFF"};

        const std::vector<RowDef> &rows()
        {
            static const std::vector<RowDef> r = {
                {"rule", "Win by", 0, {"3 Count", "2 Count", "Fall Only", "Give UP"}, "How a fall is won."},
                {"time", "Time limit", 0, {"3 Min", "5 Min", "10 Min", "15 Min", "20 Min", "30 Min", "45 Min", "60 Min", "Unlimited"},
                 "Length of the match (or of each round)."},
                {"rounds", "Rounds", 0, {"Round 1", "Round 2", "Round 3", "Round 5", "Round 10", "Round 12", "Round 15"}, "Number of rounds."},
                {"tko", "TKO after", 0, {"Down 1", "Down 2", "Down 3", "Down 4", "Down 5", "None"}, "Knockdowns that end the match."},
                {"explosion", "Explosion", 0, {"1 min", "5 min", "10 min", "15 min", "20 min", "30 min", "45 min", "60 min", "None"},
                 "When the barbed wire explodes."},
                {"matches", "Matches", 0, {"1 match", "Best of 3"}, "A single match or best of three."},
                {"corner", "Corner posts", 0, {"Barbed Wire", "Fluorescent"}, "What the corner posts are wrapped in."},
                {"weapons", "Weapons", 0, {"0", "1", "2", "3", "4", "5", "6", "7", "8"}, "Weapons placed around the ring."},
                {"overrope", "Over-the-rope", 1, kOnOff, "Throwing an opponent over the top rope. (Game label: Over-Rope.)"},
                {"rope", "Rope breaks", 1, kOnOff, "Touching the ropes breaks a pin or hold. (Game label: Rope.)"},
                {"dq", "DQ count", 1, kOnOff, "The referee counts to disqualification."},
                {"outside", "Ring-out count", 1, kOnOff, "The referee counts a wrestler out of the ring. (Game label: Outside.)"},
                {"lumber", "Lumberjacks", 1, kOnOff, "Wrestlers around the ring throw a fallen wrestler back in. (Game label: Lumber.)"},
                {"elimination", "Elimination", 1, kOnOff, "Elimination rules."},
                {"tornado", "Tornado", 1, kOnOff, "Everyone may be in the ring at once."},
                {"cutplay", "Cut play", 1, kOnOff, "Partners may interfere."},
                {"critical", "Critical", 1, {"None", "Low", "Medium", "High"}, "How easily a critical hit ends the match."},
                {"ring", "Ring", 2,
                 {"SWA", "VIEW JAPAN", "OLIVE JAPAN", "SUPER NOVA", "NEO1-MIX", "RIKI PRO", "BLIZZARD PRO", "AZTECA-PIT", "ZIP JAPAN",
                  "KANSAI", "S-DOJO", "BBT PRO", "AWG", "MEXICO", "HIGH CLASS", "BLADE", "S-1", "WOMEN", "NEW IW", "EWF", "GONGS", "UWH",
                  "BATTLE MAX"},
                 "The ring's mat and apron design.", true, "ring"},
                {"venue", "Venue", 2,
                 {"SPIKE DOME", "M. A. DOME", "BIG DOME", "TOWN HALL", "NO. 1 RING", "WEST DOME", "DOJO", "USA DOME", "RANDOM"},
                 "Where the match takes place.", true, "venue"},
                {"referee", "Referee", 2, {"Panther", "Saga", "Septiego", "Imada", "Dai", "Taki", "Mr. Fair", "Joe", "Rei", "Cindy"},
                 "Who referees the match.", true, "referee"},
                {"bgm", "Music", 2,
                 {"The Returns", "Hyper Elbow", "Panther", "Tornado", "Revolution", "Lonely Stage", "Blue Thunder", "Strange World",
                  "Dark Angel", "Holy Knight"},
                 "Music during the match. (Only the first ten tracks are mapped.)", true, "track"},
                {"comskill", "CPU skill", 3, {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"}, "How well CPU wrestlers play."},
                {"speed", "Game speed", 3, {"100%", "125%", "150%", "175%", "200%", "400%", "800%"}, "Speed of the match."},
                {"comskip", "Skip CPU matches", 3, kOnOff, "Skip matches between two CPU wrestlers."},
            };
            return r;
        }

        const RowDef *rowDef(const std::string &key)
        {
            for (const RowDef &d : rows())
                if (key == d.key)
                    return &d;
            return nullptr;
        }

        struct TypeDef
        {
            const char *name;
            const char *shortName;
            std::map<std::string, RowRule> rules;
        };

        RowRule E(const char *def, List values = {}, bool unverified = false) { return {Avail::Edit, std::move(values), def, unverified}; }
        RowRule F(const char *value, bool unverified = false) { return {Avail::Fixed, {}, value, unverified}; }
        RowRule EU(const char *def) { return E(def, {}, true); }

        // Rows every type shares unless it says otherwise.
        std::map<std::string, RowRule> common()
        {
            return {{"ring", E("SWA")},      {"venue", E("SPIKE DOME")}, {"referee", E("Panther")}, {"bgm", E("The Returns")},
                    {"comskill", E("4")},    {"speed", E("100%")},       {"critical", E("Medium")}};
        }

        std::map<std::string, RowRule> with(std::map<std::string, RowRule> m)
        {
            for (auto &kv : common())
                m.emplace(kv.first, kv.second);
            return m;
        }

        const std::vector<TypeDef> &types()
        {
            const List withEscape = {"3 Count", "2 Count", "Fall Only", "Give UP", "Escape"};
            const List swaTime = {"3 Min", "5 Min", "10 Min", "15 Min", "20 Min", "30 Min", "45 Min", "60 Min"};
            const List shortTime = {"3 Min", "5 Min", "10 Min", "15 Min", "20 Min", "30 Min"};
            static const std::vector<TypeDef> t = {
                {"Normal Match", "Normal",
                 with({{"rule", E("3 Count")}, {"time", E("30 Min")}, {"matches", E("1 match")}, {"overrope", E("OFF")}, {"rope", E("ON")},
                       {"dq", E("ON")}, {"outside", E("ON")}, {"lumber", E("OFF")}})},
                {"Cage Deathmatch", "Cage",
                 with({{"rule", E("Escape", withEscape)}, {"time", E("30 Min")}, {"matches", F("1 match")}, {"weapons", E("0")},
                       {"rope", E("ON")}, {"tornado", F("ON")}, {"dq", F("OFF")}, {"critical", F("None")}, {"outside", EU("ON")},
                       {"lumber", EU("OFF")}, {"venue", EU("SPIKE DOME")}})},
                {"Barbed Wire Deathmatch", "Barbed Wire",
                 with({{"rule", E("3 Count")}, {"time", E("30 Min")}, {"explosion", E("15 min")}, {"matches", F("1 match")},
                       {"weapons", E("0")}, {"tornado", F("ON")}, {"dq", F("OFF")}, {"outside", EU("ON")}, {"lumber", EU("OFF")},
                       {"venue", EU("SPIKE DOME")}})},
                {"Landmine Deathmatch", "Landmine",
                 with({{"rule", E("3 Count")}, {"time", E("30 Min")}, {"matches", F("1 match")}, {"corner", E("Barbed Wire")},
                       {"weapons", E("0")}, {"rope", E("ON")}, {"lumber", F("ON", true)}, {"tornado", F("ON", true)}, {"dq", F("OFF")},
                       {"outside", EU("ON")}, {"venue", EU("SPIKE DOME")}})},
                {"SWA Rules Match", "SWA Rules",
                 with({{"rule", E("3 Count")}, {"time", E("10 Min", swaTime)}, {"rounds", E("Round 3")}, {"tko", E("Down 3")},
                       {"matches", F("1 match")}, {"rope", E("ON")}, {"lumber", E("ON")}, {"dq", F("OFF")}, {"outside", EU("ON")}})},
                {"S-1 Rules Match", "S-1 Rules",
                 with({{"time", E("3 Min", shortTime)}, {"rounds", E("Round 5")}, {"tko", E("Down 3")}, {"matches", F("1 match")},
                       {"dq", F("ON")}, {"outside", EU("ON")}})},
                {"Gruesome Fighting", "Gruesome",
                 with({{"rule", F("Give UP")}, {"time", E("3 Min", shortTime)}, {"rounds", E("Round 5")}, {"matches", F("1 match")},
                       {"ring", F("Octagon Ring")}, {"venue", F("BAY SIDE")}})},
            };
            return t;
        }

        const List kControllers = {"CPU", "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"};

        std::string esc(const std::string &s)
        {
            std::string out;
            for (char c : s)
            {
                if (c == '<')
                    out += "&lt;";
                else if (c == '>')
                    out += "&gt;";
                else if (c == '&')
                    out += "&amp;";
                else
                    out += c;
            }
            return out;
        }

        // ON/OFF read as words in the new layout.
        std::string shown(const std::string &v)
        {
            if (v == "ON")
                return "On";
            if (v == "OFF")
                return "Off";
            return v;
        }

        int indexOf(const List &l, const std::string &v)
        {
            auto it = std::find(l.begin(), l.end(), v);
            return it == l.end() ? 0 : static_cast<int>(it - l.begin());
        }
    }

    // ---- Items: what Up/Down walks through ----------------------------------------------------------------------------------

    struct MatchSettingsPage::Item
    {
        enum Kind
        {
            Preset,
            Types,
            Corner,
            Row,
            Continue
        } kind;
        std::string id;  // element id
        std::string key; // row key, or "blue"/"red"
        int corner = 0;
    };

    MatchSettingsPage::MatchSettingsPage()
    {
        for (int p = 0; p < 4; ++p)
        {
            m_values[p].resize(types().size());
            for (size_t t = 0; t < types().size(); ++t)
                for (auto &kv : types()[t].rules)
                    m_values[p][t][kv.first] = kv.second.def;
            m_corners[p] = {"P1", "CPU"};
        }
    }

    MatchSettingsPage::~MatchSettingsPage() { detach(); }

    bool MatchSettingsPage::attach(Rml::ElementDocument *document)
    {
        detach();
        if (!document)
            return false;
        for (const char *id : {"types", "list", "corners", "continue", "picker", "preset"})
        {
            if (!document->GetElementById(id))
            {
                Rml::Log::Message(Rml::Log::LT_ERROR, "match settings page: no #%s", id);
                return false;
            }
        }
        m_document = document;
        document->AddEventListener(Rml::EventId::Keydown, this);
        document->AddEventListener(Rml::EventId::Click, this);
        rebuild();
        m_focus = 1; // the match type bar
        for (size_t i = 0; i < m_items.size(); ++i)
            if (m_items[i].kind == Item::Row)
            {
                m_focus = static_cast<int>(i);
                break;
            }
        refreshFocus(false);
        document->Focus();
        return true;
    }

    void MatchSettingsPage::detach()
    {
        if (!m_document)
            return;
        m_document->RemoveEventListener(Rml::EventId::Keydown, this);
        m_document->RemoveEventListener(Rml::EventId::Click, this);
        m_document = nullptr;
        m_items.clear();
    }

    const RowRule &MatchSettingsPage::rule(int type, const std::string &key) const
    {
        static const RowRule hidden;
        const auto &rules = types()[static_cast<size_t>(type)].rules;
        auto it = rules.find(key);
        return it == rules.end() ? hidden : it->second;
    }

    std::string &MatchSettingsPage::valueRef(const std::string &key)
    {
        return m_values[static_cast<size_t>(m_preset)][static_cast<size_t>(m_type)][key];
    }

    // ---- Building the page ------------------------------------------------------------------------------------------------

    void MatchSettingsPage::rebuild()
    {
        const std::string keep = (m_focus >= 0 && m_focus < static_cast<int>(m_items.size())) ? m_items[m_focus].id : std::string();
        m_items.clear();
        m_items.push_back({Item::Preset, "preset", "", 0});
        m_items.push_back({Item::Types, "types", "", 0});
        buildTypes();
        buildCorners();
        buildList();
        buildSections();
        m_items.push_back({Item::Continue, "continue", "", 0});
        m_focus = 0;
        for (size_t i = 0; i < m_items.size(); ++i)
            if (m_items[i].id == keep)
                m_focus = static_cast<int>(i);
        if (keep.empty() || m_items[m_focus].id != keep)
            m_focus = 1;
        if (Rml::Element *preset = m_document->GetElementById("preset"))
            preset->SetInnerRML("<span class='k'>Preset</span> &lt; " + std::to_string(m_preset + 1) + " / 4 &gt;");
        refreshPreview();
    }

    void MatchSettingsPage::buildTypes()
    {
        std::string rml;
        for (size_t t = 0; t < types().size(); ++t)
            rml += "<div class='chip" + std::string(static_cast<int>(t) == m_type ? " selected" : "") + "' data-type='" + std::to_string(t) +
                   "'>" + esc(types()[t].shortName) + "</div>";
        m_document->GetElementById("types")->SetInnerRML(rml);
    }

    void MatchSettingsPage::buildCorners()
    {
        static const char *names[2] = {"Blue corner", "Red corner"};
        static const char *cls[2] = {"blue", "red"};
        std::string rml;
        for (int c = 0; c < 2; ++c)
        {
            const std::string id = std::string("corner-") + cls[c];
            m_items.push_back({Item::Corner, id, cls[c], c});
            if (c == 1)
                rml += "<div class='vs'>VS</div>";
            rml += "<div class='corner " + std::string(cls[c]) + "' id='" + id + "'>" + "<div class='portrait'>wrestler</div><div class='corner-text'>" +
                   "<div class='corner-name'>" + names[c] + "</div><div class='corner-who'>Pick on the next screen</div>" +
                   "<div class='corner-ctrl'><span class='arrow' data-dir='-1'>&lt;</span><span class='val'>" +
                   m_corners[static_cast<size_t>(m_preset)][static_cast<size_t>(c)] +
                   "</span><span class='arrow' data-dir='1'>&gt;</span></div></div></div>";
        }
        m_document->GetElementById("corners")->SetInnerRML(rml);
    }

    void MatchSettingsPage::buildList()
    {
        std::string rml;
        int lastSection = -1;
        for (const RowDef &d : rows())
        {
            const RowRule &r = rule(m_type, d.key);
            if (r.avail == Avail::Hidden)
                continue;
            if (d.section != lastSection)
            {
                lastSection = d.section;
                rml += "<div class='section-head' id='sec-" + std::to_string(d.section) + "'>" + esc(kSections[d.section]) + "</div>";
            }
            const std::string id = std::string("row-") + d.key;
            m_items.push_back({Item::Row, id, d.key, 0});
            const List &values = r.values.empty() ? d.values : r.values;
            const std::string v = r.avail == Avail::Fixed ? r.def : valueRef(d.key);
            std::string cls = "row";
            if (r.avail == Avail::Fixed)
                cls += " locked";
            if (r.unverified)
                cls += " unverified";
            rml += "<div class='" + cls + "' id='" + id + "'><div class='row-label'>" + esc(d.label) + "</div>";
            if (r.avail == Avail::Fixed)
            {
                rml += "<div class='row-value'><span class='val'>" + esc(shown(v)) + "</span></div><div class='lock'>Set by " +
                       esc(types()[static_cast<size_t>(m_type)].shortName) + "</div>";
            }
            else
            {
                rml += "<div class='row-value'><span class='arrow' data-dir='-1'>&lt;</span><span class='val'>" + esc(shown(v)) +
                       "</span><span class='arrow' data-dir='1'>&gt;</span></div>";
                const int at = indexOf(values, v);
                if (values.size() <= 7) // pips read at a glance only for short lists; longer ones show "3 / 9"
                {
                    rml += "<div class='pips'>";
                    for (size_t i = 0; i < values.size(); ++i)
                        rml += std::string("<span class='pip") + (static_cast<int>(i) == at ? " on" : "") + "'></span>";
                    rml += "</div>";
                }
                else
                {
                    rml += "<div class='count'>" + std::to_string(at + 1) + " / " + std::to_string(values.size()) + "</div>";
                }
                if (d.picker)
                    rml += "<div class='more'>A: list</div>";
            }
            rml += "</div>";
        }
        m_document->GetElementById("list")->SetInnerRML(rml);
    }

    void MatchSettingsPage::buildSections()
    {
        Rml::Element *nav = m_document->GetElementById("sections");
        if (!nav)
            return;
        int counts[kSectionCount] = {};
        for (const RowDef &d : rows())
            if (rule(m_type, d.key).avail != Avail::Hidden)
                ++counts[d.section];
        std::string rml;
        for (int s = 0; s < kSectionCount; ++s)
            if (counts[s] > 0)
                rml += "<div class='nav' data-section='" + std::to_string(s) + "' id='nav-" + std::to_string(s) + "'>" + esc(kSections[s]) +
                       "<span class='n'>" + std::to_string(counts[s]) + "</span></div>";
        nav->SetInnerRML(rml);
    }

    std::string MatchSettingsPage::summary() const
    {
        auto &v = m_values[static_cast<size_t>(m_preset)][static_cast<size_t>(m_type)];
        auto get = [&](const char *key) {
            const RowRule &r = rule(m_type, key);
            if (r.avail == Avail::Hidden)
                return std::string();
            if (r.avail == Avail::Fixed)
                return r.def;
            auto it = v.find(key);
            return it == v.end() ? std::string() : it->second;
        };
        std::string s = types()[static_cast<size_t>(m_type)].name;
        for (const char *k : {"rule", "time", "rounds", "matches"})
        {
            const std::string x = get(k);
            if (!x.empty())
                s += " \xC2\xB7 " + x;
        }
        return s;
    }

    void MatchSettingsPage::refreshPreview()
    {
        auto set = [&](const char *id, const std::string &rml) {
            if (Rml::Element *e = m_document->GetElementById(id))
                e->SetInnerRML(rml);
        };
        auto get = [&](const char *key) {
            const RowRule &r = rule(m_type, key);
            return r.avail == Avail::Fixed ? r.def : valueRef(key);
        };
        const std::string corners = m_corners[static_cast<size_t>(m_preset)][0] + " vs " + m_corners[static_cast<size_t>(m_preset)][1];
        set("preview-title", esc(types()[static_cast<size_t>(m_type)].name));
        set("preview-venue", "venue image<br/>" + esc(get("venue")));
        set("preview-summary", esc(summary().substr(summary().find("\xC2\xB7") == std::string::npos ? 0 : summary().find("\xC2\xB7") + 3)));
        set("preview-ring", "<span class='k'>Ring</span> " + esc(get("ring")));
        set("preview-ref", "<div class='face'>referee</div><span class='k'>Referee</span> " + esc(get("referee")));
        set("preview-corners", esc(corners));
        set("summary-line", esc(summary()) + " \xC2\xB7 " + esc(get("venue")));
    }

    // ---- Focus and input ----------------------------------------------------------------------------------------------------

    void MatchSettingsPage::refreshFocus(bool scroll)
    {
        int section = -1;
        for (size_t i = 0; i < m_items.size(); ++i)
        {
            Rml::Element *e = m_document->GetElementById(m_items[i].id);
            if (!e)
                continue;
            const bool on = static_cast<int>(i) == m_focus;
            e->SetClass("focused", on);
            if (on && scroll)
                e->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
            if (on && m_items[i].kind == Item::Row)
                if (const RowDef *d = rowDef(m_items[i].key))
                    section = d->section;
        }
        for (int s = 0; s < kSectionCount; ++s)
            if (Rml::Element *n = m_document->GetElementById("nav-" + std::to_string(s)))
                n->SetClass("current", s == section);

        // The help line says what the focused thing is and does.
        std::string help;
        const Item &it = m_items[static_cast<size_t>(m_focus)];
        switch (it.kind)
        {
        case Item::Preset: help = "Four saved sets of match settings (the game's Settings 1-4). Left/Right to switch."; break;
        case Item::Types: help = "The kind of match. Left/Right here, or LB/RB from anywhere."; break;
        case Item::Corner: help = "Who controls this corner. The wrestler is chosen on the next screen."; break;
        case Item::Continue: help = "Go to wrestler select."; break;
        case Item::Row:
            if (const RowDef *d = rowDef(it.key))
            {
                const RowRule &r = rule(m_type, it.key);
                help = d->help;
                if (r.avail == Avail::Fixed)
                    help = "Fixed for " + std::string(types()[static_cast<size_t>(m_type)].name) + ". " + help;
                if (r.unverified)
                    help += " [Not confirmed in the game for this match type.]";
            }
            break;
        }
        if (Rml::Element *h = m_document->GetElementById("help"))
            h->SetInnerRML(esc(help));
    }

    void MatchSettingsPage::move(int delta)
    {
        const int n = static_cast<int>(m_items.size());
        m_focus = std::clamp(m_focus + delta, 0, n - 1);
        refreshFocus(true);
    }

    void MatchSettingsPage::setType(int index)
    {
        const int n = static_cast<int>(types().size());
        m_type = (index % n + n) % n;
        rebuild();
        refreshFocus(true);
    }

    void MatchSettingsPage::setPreset(int index)
    {
        m_preset = (index % 4 + 4) % 4;
        rebuild();
        refreshFocus(true);
    }

    void MatchSettingsPage::change(int delta)
    {
        const Item it = m_items[static_cast<size_t>(m_focus)];
        if (it.kind == Item::Preset)
            return setPreset(m_preset + delta);
        if (it.kind == Item::Types)
            return setType(m_type + delta);
        if (it.kind == Item::Corner)
        {
            std::string &c = m_corners[static_cast<size_t>(m_preset)][static_cast<size_t>(it.corner)];
            const int n = static_cast<int>(kControllers.size());
            c = kControllers[static_cast<size_t>(((indexOf(kControllers, c) + delta) % n + n) % n)];
            rebuild();
            refreshFocus(false);
            return;
        }
        if (it.kind != Item::Row)
            return;
        const RowDef *d = rowDef(it.key);
        const RowRule &r = rule(m_type, it.key);
        if (!d || r.avail != Avail::Edit)
            return;
        const List &values = r.values.empty() ? d->values : r.values;
        std::string &v = valueRef(it.key);
        // Clamp, don't wrap: the pips show where the value sits, and a wrap would jump across them.
        v = values[static_cast<size_t>(std::clamp(indexOf(values, v) + delta, 0, static_cast<int>(values.size()) - 1))];
        rebuild();
        refreshFocus(false);
    }

    void MatchSettingsPage::activate()
    {
        const Item &it = m_items[static_cast<size_t>(m_focus)];
        if (it.kind == Item::Continue)
        {
            if (m_onContinue)
                m_onContinue(summary());
            return;
        }
        if (it.kind == Item::Row)
        {
            const RowDef *d = rowDef(it.key);
            if (d && d->picker && rule(m_type, it.key).avail == Avail::Edit)
                return openPicker();
            // Short lists: A steps on, wrapping (the value is in reach either way).
            if (d && rule(m_type, it.key).avail == Avail::Edit)
            {
                const RowRule &r = rule(m_type, it.key);
                const List &values = r.values.empty() ? d->values : r.values;
                std::string &v = valueRef(it.key);
                v = values[static_cast<size_t>((indexOf(values, v) + 1) % static_cast<int>(values.size()))];
                rebuild();
                refreshFocus(false);
            }
            return;
        }
        change(1);
    }

    void MatchSettingsPage::openPicker()
    {
        const Item &it = m_items[static_cast<size_t>(m_focus)];
        const RowDef *d = rowDef(it.key);
        const RowRule &r = rule(m_type, it.key);
        const List &values = r.values.empty() ? d->values : r.values;
        m_picking = true;
        m_pickKey = it.key;
        m_pickIndex = indexOf(values, valueRef(it.key));
        std::string rml;
        for (size_t i = 0; i < values.size(); ++i)
            rml += "<div class='pick" + std::string(static_cast<int>(i) == m_pickIndex ? " current" : "") + "' id='pick-" + std::to_string(i) +
                   "' data-pick='" + std::to_string(i) + "'><div class='thumb'>" + (d->thumb ? d->thumb : "") + "</div><div class='pick-name'>" +
                   esc(values[i]) + "</div></div>";
        m_document->GetElementById("picker-list")->SetInnerRML(rml);
        if (Rml::Element *t = m_document->GetElementById("picker-title"))
            t->SetInnerRML(esc(d->label) + " <span class='k'>" + std::to_string(values.size()) + " choices</span>");
        m_document->GetElementById("picker")->SetClass("hidden", false);
        m_document->GetElementById("screen")->SetClass("picking", true);
        pickerMove(0);
    }

    void MatchSettingsPage::pickerMove(int delta)
    {
        const RowDef *d = rowDef(m_pickKey);
        const RowRule &r = rule(m_type, m_pickKey);
        const List &values = r.values.empty() ? d->values : r.values;
        m_pickIndex = std::clamp(m_pickIndex + delta, 0, static_cast<int>(values.size()) - 1);
        for (size_t i = 0; i < values.size(); ++i)
            if (Rml::Element *e = m_document->GetElementById("pick-" + std::to_string(i)))
            {
                e->SetClass("focused", static_cast<int>(i) == m_pickIndex);
                if (static_cast<int>(i) == m_pickIndex)
                    e->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
            }
        if (Rml::Element *h = m_document->GetElementById("help"))
            h->SetInnerRML(esc(values[static_cast<size_t>(m_pickIndex)]) + " \xC2\xB7 A to choose, B to keep " + esc(shown(valueRef(m_pickKey))));
    }

    void MatchSettingsPage::closePicker(bool apply)
    {
        if (apply)
        {
            const RowDef *d = rowDef(m_pickKey);
            const RowRule &r = rule(m_type, m_pickKey);
            const List &values = r.values.empty() ? d->values : r.values;
            valueRef(m_pickKey) = values[static_cast<size_t>(m_pickIndex)];
        }
        m_picking = false;
        m_document->GetElementById("picker")->SetClass("hidden", true);
        m_document->GetElementById("screen")->SetClass("picking", false);
        rebuild();
        refreshFocus(false);
    }

    int MatchSettingsPage::itemAt(Rml::Element *element) const
    {
        for (Rml::Element *e = element; e; e = e->GetParentNode())
            for (size_t i = 0; i < m_items.size(); ++i)
                if (!e->GetId().empty() && e->GetId() == m_items[i].id)
                    return static_cast<int>(i);
        return -1;
    }

    void MatchSettingsPage::ProcessEvent(Rml::Event &event)
    {
        if (!m_document)
            return;
        if (event.GetId() == Rml::EventId::Keydown)
        {
            const int key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
            bool used = true;
            if (m_picking)
            {
                if (key == Rml::Input::KI_UP || key == Rml::Input::KI_LEFT)
                    pickerMove(-1);
                else if (key == Rml::Input::KI_DOWN || key == Rml::Input::KI_RIGHT)
                    pickerMove(1);
                else if (key == Rml::Input::KI_RETURN)
                    closePicker(true);
                else if (key == Rml::Input::KI_ESCAPE)
                    closePicker(false);
                else
                    used = false;
            }
            else if (key == Rml::Input::KI_UP)
                move(-1);
            else if (key == Rml::Input::KI_DOWN)
                move(1);
            else if (key == Rml::Input::KI_LEFT)
                change(-1);
            else if (key == Rml::Input::KI_RIGHT)
                change(1);
            else if (key == Rml::Input::KI_RETURN)
                activate();
            else if (key == Rml::Input::KI_PRIOR)
                setType(m_type - 1);
            else if (key == Rml::Input::KI_NEXT)
                setType(m_type + 1);
            else if (key == Rml::Input::KI_ESCAPE)
            {
                if (m_onBack)
                    m_onBack();
            }
            else
                used = false;
            if (used)
                event.StopPropagation();
            return;
        }
        if (event.GetId() == Rml::EventId::Click)
        {
            Rml::Element *target = event.GetTargetElement();
            // Read everything from the target before anything rebuilds the page (which destroys it).
            int type = -1, pick = -1, section = -1, dir = 0;
            for (Rml::Element *e = target; e; e = e->GetParentNode())
            {
                if (type < 0 && e->HasAttribute("data-type"))
                    type = e->GetAttribute<int>("data-type", -1);
                if (pick < 0 && e->HasAttribute("data-pick"))
                    pick = e->GetAttribute<int>("data-pick", -1);
                if (section < 0 && e->HasAttribute("data-section"))
                    section = e->GetAttribute<int>("data-section", -1);
                if (dir == 0 && e->HasAttribute("data-dir"))
                    dir = e->GetAttribute<int>("data-dir", 0);
            }
            const int item = itemAt(target);
            if (m_picking)
            {
                if (pick >= 0)
                {
                    m_pickIndex = pick;
                    closePicker(true);
                }
                else if (target && target->GetId() == "picker")
                    closePicker(false);
                return;
            }
            if (type >= 0)
                return setType(type);
            if (section >= 0)
            {
                for (size_t i = 0; i < m_items.size(); ++i)
                    if (m_items[i].kind == Item::Row && rowDef(m_items[i].key) && rowDef(m_items[i].key)->section == section)
                    {
                        m_focus = static_cast<int>(i);
                        refreshFocus(true);
                        return;
                    }
                return;
            }
            if (item >= 0)
            {
                m_focus = item;
                if (dir != 0)
                    change(dir);
                else
                    activate();
                refreshFocus(false);
            }
        }
    }

    std::string MatchSettingsPage::value(const std::string &key) const
    {
        if (key == "type")
            return types()[static_cast<size_t>(m_type)].shortName;
        if (key == "preset")
            return std::to_string(m_preset + 1);
        if (key == "blue")
            return m_corners[static_cast<size_t>(m_preset)][0];
        if (key == "red")
            return m_corners[static_cast<size_t>(m_preset)][1];
        if (key == "focus")
            return m_items.empty() ? std::string() : m_items[static_cast<size_t>(m_focus)].id;
        if (key == "picker")
            return m_picking ? "open" : "closed";
        if (key == "summary")
            return summary();
        if (key == "visible-rows")
        {
            std::string s;
            for (const Item &it : m_items)
                if (it.kind == Item::Row)
                    s += (s.empty() ? "" : "|") + it.key;
            return s;
        }
        const RowRule &r = rule(m_type, key);
        if (r.avail == Avail::Hidden)
            return "(hidden)";
        if (r.avail == Avail::Fixed)
            return r.def;
        auto &v = m_values[static_cast<size_t>(m_preset)][static_cast<size_t>(m_type)];
        auto it = v.find(key);
        return it == v.end() ? std::string() : it->second;
    }
}
