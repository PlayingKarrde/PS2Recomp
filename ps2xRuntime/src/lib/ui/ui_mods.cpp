#include "ui_mods.h"

#include "mod_manager.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Log.h>

#include <algorithm>
#include <cstdlib>

namespace ps2x::ui
{
    namespace
    {
        using ps2x::mods::Kind;
        using ps2x::mods::ModInfo;
        using ps2x::mods::SettingDecl;

        // Text for an RML element's content: markup-safe, and a newline becomes a line break.
        std::string text(const std::string &value)
        {
            std::string out;
            for (const char c : value)
            {
                switch (c)
                {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '\n': out += "<br/>"; break;
                default: out += c; break;
                }
            }
            return out;
        }

        // Text for an attribute value.
        std::string attr(const std::string &value)
        {
            std::string out;
            for (const char c : value)
            {
                switch (c)
                {
                case '&': out += "&amp;"; break;
                case '"': out += "&quot;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '\n': out += ' '; break;
                default: out += c; break;
                }
            }
            return out;
        }

        std::string get(const Rml::Element *e, const char *name)
        {
            return e ? e->GetAttribute<Rml::String>(name, std::string()) : std::string();
        }

        const char *kindTitle(Kind kind)
        {
            switch (kind)
            {
            case Kind::Assets: return "Assets";
            case Kind::Data: return "Data";
            case Kind::Patch: return "Patch";
            case Kind::Code: return "Code";
            case Kind::Theme: return "Theme";
            }
            return "";
        }

        // What a mod of this kind does in this build.
        const char *kindNote(Kind kind)
        {
            switch (kind)
            {
            case Kind::Theme: return "A theme changes how this menu looks. It takes effect when the game starts.";
            case Kind::Assets: return "Replacement pictures (texture packs). They take effect when the game starts, and only with the GPU renderer.";
            case Kind::Code: return "Code mods are not supported yet, so this does nothing.";
            case Kind::Data: return "A data pack changes what is in your save (names, for now). While one is on, the game plays from a copy of your memory card, and your own card is not changed. It takes effect when the game starts.";
            default: return "This kind of mod does nothing yet. Support comes in a later version.";
            }
        }

        // The value of a setting as the row shows it.
        std::string settingShown(const SettingDecl &decl, const std::string &value)
        {
            if (decl.type == SettingDecl::Type::Bool)
                return value == "true" ? "[ON]" : "[OFF]";
            return "[" + value + "]";
        }

        int intStep(const SettingDecl &decl)
        {
            return std::max(1, (decl.maxValue - decl.minValue) / 100);
        }
    }

    ModsPage::~ModsPage()
    {
        detach();
    }

    Rml::Element *ModsPage::byId(const char *id) const
    {
        return m_document ? m_document->GetElementById(id) : nullptr;
    }

    bool ModsPage::attach(Rml::ElementDocument *document, ps2x::mods::Manager *manager)
    {
        detach();
        if (!document || !manager)
            return false;
        m_document = document;
        m_manager = manager;
        for (const char *id : {"mods-list", "mod-details", "mods-status", "mods-folder", "mod-confirm", "mod-confirm-title", "mod-confirm-text",
                               "mod-confirm-yes", "mod-confirm-no", "window"})
        {
            if (!byId(id))
            {
                Rml::Log::Message(Rml::Log::LT_ERROR, "ModsPage: the page has no #%s", id);
                m_document = nullptr;
                m_manager = nullptr;
                return false;
            }
        }
        // Events that bubble are heard once on the document. focus and blur do not bubble, so those are per element. keydown is also
        // heard in the capture phase, ahead of the settings page, so that the uninstall question gets Escape and the tab keys first.
        for (const char *type : {"click", "keydown", "mouseover"})
            document->AddEventListener(type, this);
        document->AddEventListener("keydown", this, true);
        listenToFocus(byId("mod-confirm"));
        listenToFocus(byId("mods-tools")); // the Keep Card, Dump Textures and Dumps Folder buttons show their help line too
        if (Rml::Element *folder = byId("mods-folder"))
        {
            const auto folderText = m_manager->modsDir().u8string();
            folder->SetInnerRML("Mods folder: " + text(std::string(folderText.begin(), folderText.end())));
        }
        refresh();
        return true;
    }

    void ModsPage::setTools(Tools tools)
    {
        m_tools = std::move(tools);
        const std::string line = m_tools.devPackLine ? m_tools.devPackLine() : std::string();
        if (Rml::Element *dev = byId("dev-folder"))
            dev->SetInnerRML(line.empty() ? std::string() : text(line));
        // The modder's buttons only with the dev pack line; the card row has its own button.
        for (const char *id : {"dump-textures", "open-dumps"})
            if (Rml::Element *button = byId(id))
                button->SetProperty("display", line.empty() ? "none" : "block");
        updateCardRow();
        if (m_tools.lastDumpResult)
        {
            const std::string last = m_tools.lastDumpResult();
            if (!last.empty())
                setStatus(last);
        }
    }

    void ModsPage::detach()
    {
        if (!m_document)
            return;
        for (const char *type : {"click", "keydown", "mouseover"})
            m_document->RemoveEventListener(type, this);
        m_document->RemoveEventListener("keydown", this, true);
        m_document = nullptr;
        m_manager = nullptr;
        m_confirming = false;
    }

    // ---- showing ----------------------------------------------------------------------------------------------

    void ModsPage::listenToFocus(Rml::Element *root)
    {
        if (!root)
            return;
        Rml::ElementList elements;
        root->QuerySelectorAll(elements, ".mod, .mbtn, .mrow, .button");
        for (Rml::Element *e : elements)
            e->AddEventListener("focus", this);
    }

    void ModsPage::showHelp(const std::string &help)
    {
        if (!m_document)
            return;
        if (Rml::Element *line = m_document->GetElementById("help-text"))
            line->SetInnerRML(text(help));
    }

    void ModsPage::setStatus(const std::string &message)
    {
        if (Rml::Element *line = byId("mods-status"))
            line->SetInnerRML(text(message));
    }

    void ModsPage::refresh()
    {
        if (!m_document || !m_manager)
            return;
        if (m_tools.devPackLine)
            if (Rml::Element *dev = byId("dev-folder"))
                dev->SetInnerRML(text(m_tools.devPackLine())); // the line carries the newest complaint about a dev pack file
        updateCardRow();
        const auto mods = m_manager->list();
        const bool there = std::any_of(mods.begin(), mods.end(), [&](const ModInfo &m) { return m.key == m_selected; });
        if (!there)
        {
            m_selected = mods.empty() ? std::string() : mods.front().key;
            m_picture = 0;
        }
        renderList();
        renderDetails();
    }

    void ModsPage::renderList()
    {
        Rml::Element *list = byId("mods-list");
        if (!list)
            return;
        const auto mods = m_manager->list();
        std::string rml;
        if (mods.empty())
        {
            rml = "<div id=\"mods-empty\">No mods installed.</div>";
        }
        for (const ModInfo &m : mods)
        {
            std::string classes = "mod";
            std::string help;
            std::string flag;
            std::string state;
            if (!m.valid)
            {
                classes += " bad";
                flag = "!";
                state = "BAD";
                help = "This mod cannot be used. The reason is shown on the right.";
            }
            else if (m.enabled)
            {
                classes += m.active ? " on" : " on warn";
                state = "ON";
                if (!m.active)
                {
                    flag = "!";
                    help = "On, but it does nothing: " + m.problem;
                }
                else
                {
                    help = "A: turn off. Right: more actions for this mod.";
                }
            }
            else
            {
                classes += " off";
                state = "OFF";
                if (!m.problem.empty())
                {
                    classes += " blocked";
                    flag = "!";
                    help = "Cannot be turned on yet: " + m.problem;
                }
                else
                {
                    help = "A: turn on. Right: more actions for this mod.";
                }
            }
            if (m.key == m_selected)
                classes += " selected";
            rml += "<div class=\"" + classes + "\" data-key=\"" + attr(m.key) + "\" data-help=\"" + attr(help) + "\">";
            rml += "<span class=\"m-pos\">" + (m.valid ? std::to_string(m.position) : std::string()) + "</span>";
            rml += "<span class=\"m-name\">" + text(m.name) + "</span>";
            rml += "<span class=\"m-flag\">" + flag + "</span>";
            rml += "<span class=\"m-state\">" + state + "</span>";
            rml += "</div>";
        }
        list->SetInnerRML(rml);
        listenToFocus(list);
    }

    void ModsPage::renderDetails()
    {
        Rml::Element *details = byId("mod-details");
        if (!details)
            return;
        const auto info = m_selected.empty() ? std::optional<ModInfo>() : m_manager->find(m_selected);
        if (!info)
        {
            details->SetInnerRML("<div id=\"mod-title\">Mods</div>"
                                 "<div id=\"mod-desc\">Mods add things to the game. To install one, drop its .fprmod file on the game window, "
                                 "or copy it into the mods folder shown below.</div>");
            return;
        }
        const ModInfo &m = *info;
        // Everything but the buttons scrolls (a long description, many settings); the buttons stay in view.
        std::string rml = "<div id=\"mod-info\">";
        rml += "<div id=\"mod-title\">" + text(m.name) + "</div>";

        std::string meta;
        if (!m.version.empty())
            meta += "v" + text(m.version);
        if (!m.authors.empty())
            meta += std::string(meta.empty() ? "" : " - ") + "by " + text(m.authors);
        if (m.valid)
            meta += std::string(meta.empty() ? "" : " - ") + kindTitle(m.kind) + " mod";
        if (m.valid && m.position > 0)
            meta += " - load order " + std::to_string(m.position);
        if (!m.valid)
            meta += std::string(meta.empty() ? "" : " - ") + text(m.file);
        rml += "<div id=\"mod-meta\">" + meta + "</div>";

        // Pictures: the thumbnail first, then the screenshots.
        std::vector<std::string> pictures;
        if (!m.thumbnail.empty())
            pictures.push_back(m.thumbnail);
        for (const std::string &shot : m.screenshots)
            pictures.push_back(shot);

        rml += "<div id=\"mod-main\"><div id=\"mod-text\">";
        if (!m.description.empty())
            rml += "<div id=\"mod-desc\">" + text(m.description) + "</div>";
        if (!m.valid)
            rml += "<div id=\"mod-problem\" class=\"bad\">" + text(m.error) + "</div>";
        else if (!m.problem.empty())
            rml += std::string("<div id=\"mod-problem\" class=\"") + (m.enabled ? "warn" : "bad") + "\">" +
                   (m.enabled ? "On, but it does nothing: " : "Cannot be turned on yet: ") + text(m.problem) + "</div>";
        if (m.valid)
        {
            // What the running game found wrong with the mod's files (texture pictures it refused, and why).
            const std::vector<std::string> notes = m_manager->runtimeNotes(m.id);
            if (!notes.empty())
            {
                rml += "<div id=\"mod-notes\">The game could not use some files of this mod:";
                const size_t shown = std::min<size_t>(notes.size(), 5);
                for (size_t i = 0; i < shown; ++i)
                    rml += "<div class=\"mod-notes-line\">" + text(notes[i]) + "</div>";
                if (notes.size() > shown)
                    rml += "<div class=\"mod-notes-line\">and " + std::to_string(notes.size() - shown) + " more (see the log).</div>";
                rml += "</div>";
            }
            // What a data pack did to the memory card (and what it kept of the player's own changes).
            const std::vector<std::string> infos = m_manager->infoNotes(m.id);
            if (!infos.empty())
            {
                rml += "<div id=\"mod-infonotes\">On your memory card:";
                const size_t shown = std::min<size_t>(infos.size(), 5);
                for (size_t i = 0; i < shown; ++i)
                    rml += "<div class=\"mod-notes-line\">" + text(infos[i]) + "</div>";
                if (infos.size() > shown)
                    rml += "<div class=\"mod-notes-line\">and " + std::to_string(infos.size() - shown) + " more (see the log).</div>";
                rml += "</div>";
            }
            std::string links;
            for (const auto &dep : m.depends)
            {
                const auto other = m_manager->find(dep.id);
                links += (links.empty() ? "Needs: " : ", ") + text(other ? other->name : dep.id) + (dep.constraint.empty() ? "" : " " + text(dep.constraint));
            }
            std::string conflicts;
            for (const std::string &id : m.conflicts)
            {
                const auto other = m_manager->find(id);
                conflicts += (conflicts.empty() ? "" : ", ") + text(other ? other->name : id);
            }
            if (!conflicts.empty())
                links += std::string(links.empty() ? "" : ". ") + "Does not work with: " + conflicts;
            if (!links.empty())
                rml += "<div id=\"mod-links\">" + links + ".</div>";
            rml += std::string("<div id=\"mod-note\">") + kindNote(m.kind) + "</div>";
        }
        rml += "</div>";
        if (!pictures.empty())
        {
            const std::string &shot = pictures[m_picture % pictures.size()];
            rml += "<div id=\"mod-picbox\"><img id=\"mod-pic\" src=\"" + attr(ps2x::mods::Manager::virtualName(m.id, shot)) + "\"/></div>";
        }
        rml += "</div>";

        if (m.valid && !m.settings.empty())
        {
            rml += "<div id=\"mod-settings\">";
            for (const SettingDecl &decl : m.settings)
            {
                const auto found = m.settingValues.find(decl.key);
                const std::string value = found != m.settingValues.end() ? found->second : decl.defaultValue;
                const std::string help = decl.help.empty() ? "Left and Right change it. Kept for this mod; nothing uses it yet." : decl.help;
                rml += "<div class=\"mrow\" data-setting=\"" + attr(decl.key) + "\" data-help=\"" + attr(help) + "\"><span class=\"label\">" +
                       text(decl.label) + "</span><span class=\"value\">" + text(settingShown(decl, value)) + "</span></div>";
            }
            rml += "</div>";
        }

        rml += "</div>";
        rml += "<div id=\"mod-actions\">";
        if (m.valid)
        {
            const bool blocked = !m.enabled && !m.problem.empty();
            rml += std::string("<div class=\"mbtn") + (blocked ? " locked" : "") + "\" data-action=\"toggle\" data-help=\"" +
                   attr(blocked ? "Cannot be turned on yet: " + m.problem : std::string("Turn this mod on or off. It takes effect when the game starts.")) +
                   "\">" + (m.enabled ? "Turn Off" : "Turn On") + "</div>";
            rml += "<div class=\"mbtn\" data-action=\"up\" data-help=\"Load this mod earlier. Mods lower in the list win over the ones above.\">Move Up</div>";
            rml += "<div class=\"mbtn\" data-action=\"down\" data-help=\"Load this mod later, so it wins over the mods above it.\">Move Down</div>";
        }
        if (pictures.size() > 1)
            rml += "<div class=\"mbtn\" data-action=\"picture\" data-help=\"Show the next picture of this mod.\">Picture " +
                   std::to_string(m_picture % pictures.size() + 1) + "/" + std::to_string(pictures.size()) + "</div>";
        rml += "<div class=\"mbtn\" data-action=\"uninstall\" data-help=\"Move this mod's file to the .trash folder in the mods folder.\">Uninstall</div>";
        rml += "</div>";

        details->SetInnerRML(rml);
        listenToFocus(details);
    }

    void ModsPage::select(const std::string &key)
    {
        if (key == m_selected || !m_document)
            return;
        m_selected = key;
        m_picture = 0;
        if (Rml::Element *list = byId("mods-list"))
        {
            Rml::ElementList rows;
            list->QuerySelectorAll(rows, ".mod");
            for (Rml::Element *row : rows)
                row->SetClass("selected", get(row, "data-key") == key);
        }
        renderDetails();
    }

    void ModsPage::focusSelectedRow()
    {
        if (!m_document)
            return;
        m_document->UpdateDocument();
        Rml::Element *list = byId("mods-list");
        if (!list)
            return;
        Rml::ElementList rows;
        list->QuerySelectorAll(rows, ".mod");
        Rml::Element *target = nullptr;
        for (Rml::Element *row : rows)
            if (get(row, "data-key") == m_selected)
                target = row;
        if (!target && !rows.empty())
            target = rows.front();
        if (target)
        {
            target->Focus(true);
            return;
        }
        // No mods: the focus goes to the tab, so the controller still has a place.
        if (Rml::Element *tab = byId("tab-mods"))
            tab->Focus(true);
    }

    void ModsPage::focusList()
    {
        focusSelectedRow();
    }

    void ModsPage::focusAction(const std::string &action)
    {
        if (!m_document)
            return;
        m_document->UpdateDocument();
        Rml::Element *details = byId("mod-details");
        if (!details)
            return;
        Rml::ElementList buttons;
        details->QuerySelectorAll(buttons, ".mbtn");
        for (Rml::Element *button : buttons)
        {
            if (get(button, "data-action") == action)
            {
                button->Focus(true);
                return;
            }
        }
        focusSelectedRow();
    }

    void ModsPage::changed()
    {
        if (m_onChange)
            m_onChange();
    }

    // ---- doing ------------------------------------------------------------------------------------------------

    void ModsPage::act(const std::string &action, bool fromRow)
    {
        if (m_selected.empty())
            return;
        const auto info = m_manager->find(m_selected);
        if (!info)
            return;
        if (action == "uninstall")
        {
            openConfirm();
            return;
        }
        if (action == "picture")
        {
            ++m_picture;
            renderDetails();
            focusAction("picture");
            return;
        }
        ps2x::mods::Result result;
        if (action == "toggle")
            result = m_manager->setEnabled(m_selected, !info->enabled);
        else if (action == "up")
            result = m_manager->move(m_selected, -1);
        else if (action == "down")
            result = m_manager->move(m_selected, 1);
        else
            return;
        setStatus(result.message);
        refresh();
        changed();
        if (fromRow)
            focusSelectedRow();
        else
            focusAction(action);
    }

    void ModsPage::changeSetting(Rml::Element *row, int direction)
    {
        const auto info = m_manager->find(m_selected);
        if (!info)
            return;
        const std::string key = get(row, "data-setting");
        const SettingDecl *decl = nullptr;
        for (const SettingDecl &d : info->settings)
            if (d.key == key)
                decl = &d;
        if (!decl)
            return;
        const auto found = info->settingValues.find(key);
        const std::string old = found != info->settingValues.end() ? found->second : decl->defaultValue;
        std::string now = old;
        if (decl->type == SettingDecl::Type::Bool)
        {
            now = old == "true" ? "false" : "true";
        }
        else if (decl->type == SettingDecl::Type::Choice)
        {
            const auto it = std::find(decl->choices.begin(), decl->choices.end(), old);
            const int count = static_cast<int>(decl->choices.size());
            int index = it == decl->choices.end() ? 0 : static_cast<int>(it - decl->choices.begin());
            index = (index + (direction == 0 ? 1 : direction) + count) % count;
            now = decl->choices[static_cast<size_t>(index)];
        }
        else
        {
            int value = std::atoi(old.c_str());
            if (direction == 0)
                value = value >= decl->maxValue ? decl->minValue : value + intStep(*decl);
            else
                value += direction * intStep(*decl);
            value = std::clamp(value, decl->minValue, decl->maxValue);
            now = std::to_string(value);
        }
        if (now == old)
            return;
        const ps2x::mods::Result result = m_manager->setSetting(m_selected, key, now);
        setStatus(result.message);
        renderDetails();
        // The row was rebuilt: the focus goes back to it.
        if (m_document)
        {
            m_document->UpdateDocument();
            Rml::ElementList rows;
            if (Rml::Element *details = byId("mod-details"))
                details->QuerySelectorAll(rows, ".mrow");
            for (Rml::Element *r : rows)
                if (get(r, "data-setting") == key)
                    r->Focus(true);
        }
    }

    void ModsPage::openConfirm()
    {
        const auto info = m_manager->find(m_selected);
        Rml::Element *window = byId("window");
        Rml::Element *confirm = byId("mod-confirm");
        Rml::Element *cancel = byId("mod-confirm-no");
        if (!info || !window || !confirm || !cancel || m_confirming)
            return;
        m_confirming = true;
        m_confirmKeep = false;
        m_confirmKey = info->key;
        if (Rml::Element *title = byId("mod-confirm-title"))
            title->SetInnerRML("Uninstall " + text(info->name) + "?");
        if (Rml::Element *body = byId("mod-confirm-text"))
            body->SetInnerRML("The file is moved to the .trash folder inside the mods folder. You can put it back by hand.");
        if (Rml::Element *yes = byId("mod-confirm-yes"))
        {
            yes->SetInnerRML("Uninstall");
            yes->SetAttribute("data-help", "Move the mod's file to the .trash folder.");
        }
        window->SetClass("confirming", true);
        confirm->SetClass("hidden", false);
        m_document->UpdateDocument();
        cancel->Focus(true);
    }

    // "Keep this card?": the same layer as the uninstall question, with its own words. Cancel has the focus.
    void ModsPage::openKeepConfirm()
    {
        Rml::Element *window = byId("window");
        Rml::Element *confirm = byId("mod-confirm");
        Rml::Element *cancel = byId("mod-confirm-no");
        if (!window || !confirm || !cancel || m_confirming)
            return;
        m_confirming = true;
        m_confirmKeep = true;
        m_confirmKey.clear();
        if (Rml::Element *title = byId("mod-confirm-title"))
            title->SetInnerRML("Keep this card?");
        if (Rml::Element *body = byId("mod-confirm-text"))
            body->SetInnerRML("Copies the save on the profile card over the one on your own card. Your own card's files are saved first, in the profile folder.");
        if (Rml::Element *yes = byId("mod-confirm-yes"))
        {
            yes->SetInnerRML("Keep Card");
            yes->SetAttribute("data-help", "Copy the profile card's save to your own card.");
        }
        window->SetClass("confirming", true);
        confirm->SetClass("hidden", false);
        m_document->UpdateDocument();
        cancel->Focus(true);
    }

    // The card line and the Keep Card button follow the host.
    void ModsPage::updateCardRow()
    {
        if (Rml::Element *line = byId("mods-card"))
            line->SetInnerRML(m_tools.cardLine ? text(m_tools.cardLine()) : std::string());
        if (Rml::Element *button = byId("keep-card"))
            button->SetProperty("display", m_tools.canKeepCard && m_tools.keepCard && m_tools.canKeepCard() ? "block" : "none");
    }

    void ModsPage::leaveConfirm(bool refocus)
    {
        if (!m_document || !m_confirming)
            return;
        const bool wasKeep = m_confirmKeep;
        m_confirming = false;
        m_confirmKeep = false;
        if (Rml::Element *window = byId("window"))
            window->SetClass("confirming", false);
        if (Rml::Element *confirm = byId("mod-confirm"))
            confirm->SetClass("hidden", true);
        m_document->UpdateDocument();
        if (refocus)
        {
            if (wasKeep)
            {
                if (Rml::Element *keep = byId("keep-card"))
                    keep->Focus(true);
            }
            else
            {
                focusAction("uninstall");
            }
        }
    }

    void ModsPage::closeConfirm()
    {
        leaveConfirm(false);
    }

    // ---- events -----------------------------------------------------------------------------------------------

    void ModsPage::ProcessEvent(Rml::Event &event)
    {
        Rml::Element *target = event.GetTargetElement();
        const Rml::String &type = event.GetType();

        if (type == "focus")
        {
            Rml::Element *element = event.GetCurrentElement();
            if (!element)
                return;
            if (element->IsClassSet("mod"))
                select(get(element, "data-key"));
            const std::string help = get(element, "data-help");
            if (!help.empty())
                showHelp(help);
            return;
        }

        if (type == "mouseover")
        {
            // The highlight follows the mouse, as the game's cursor does.
            for (Rml::Element *e = target; e; e = e->GetParentNode())
            {
                if (e->IsClassSet("mod") || e->IsClassSet("mbtn") || e->IsClassSet("mrow"))
                {
                    if (!e->IsPseudoClassSet("focus") && !m_confirming)
                        e->Focus(false);
                    return;
                }
            }
            return;
        }

        if (type == "click")
        {
            for (Rml::Element *e = target; e; e = e->GetParentNode())
            {
                if (e->GetId() == "dump-textures" && !m_confirming)
                {
                    const std::string said = m_tools.dumpTextures ? m_tools.dumpTextures() : std::string();
                    // The instruction comes first: the status line does not wrap, and a long folder path would push it out of sight.
                    const bool started = said.rfind("Dumping", 0) == 0;
                    setStatus(said.empty() ? "The texture tools are not available." : (started ? "Close this menu so the game can draw it. " : std::string()) + said);
                    return;
                }
                if (e->GetId() == "open-dumps" && !m_confirming)
                {
                    if (m_tools.openDumpsFolder)
                        m_tools.openDumpsFolder();
                    setStatus("Opened the dumps folder.");
                    return;
                }
                if (e->GetId() == "mod-confirm-no")
                {
                    leaveConfirm(true);
                    return;
                }
                if (e->GetId() == "keep-card" && !m_confirming)
                {
                    openKeepConfirm();
                    return;
                }
                if (e->GetId() == "mod-confirm-yes")
                {
                    if (!m_confirming)
                        return;
                    if (m_confirmKeep)
                    {
                        leaveConfirm(false);
                        setStatus(m_tools.keepCard ? m_tools.keepCard() : std::string());
                        updateCardRow();
                        if (Rml::Element *keep = byId("keep-card"))
                            keep->Focus(true);
                        return;
                    }
                    const std::string key = m_confirmKey;
                    leaveConfirm(false);
                    const ps2x::mods::Result result = m_manager->uninstall(key);
                    setStatus(result.message);
                    m_selected.clear();
                    refresh();
                    changed();
                    focusSelectedRow();
                    return;
                }
                if (e->IsClassSet("mbtn"))
                {
                    act(get(e, "data-action"));
                    return;
                }
                if (e->IsClassSet("mrow"))
                {
                    changeSetting(e, 0);
                    return;
                }
                if (e->IsClassSet("mod"))
                {
                    select(get(e, "data-key"));
                    act("toggle", true);
                    return;
                }
            }
            return;
        }

        if (type == "keydown")
        {
            const int key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
            if (event.GetPhase() == Rml::EventPhase::Capture)
            {
                // Only the uninstall question gets a say before the rest of the menu: B or Escape answers it with Cancel, and the tabs
                // behind it do not switch.
                if (!m_confirming)
                    return;
                if (key == Rml::Input::KI_ESCAPE)
                {
                    leaveConfirm(true);
                    event.StopPropagation();
                }
                else if (key == Rml::Input::KI_PRIOR || key == Rml::Input::KI_NEXT)
                {
                    event.StopPropagation();
                }
                return;
            }
            if (key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT)
            {
                for (Rml::Element *e = target; e; e = e->GetParentNode())
                {
                    if (e->IsClassSet("mrow"))
                    {
                        changeSetting(e, key == Rml::Input::KI_RIGHT ? 1 : -1);
                        event.StopPropagation(); // Left and Right belong to the row, not to focus navigation
                        return;
                    }
                }
            }
            if ((key == Rml::Input::KI_RIGHT || key == Rml::Input::KI_LEFT) && !m_confirming)
            {
                // Between the list and the details: Right from a mod goes to the first button, Left from the first button goes back
                // to the list (the focus navigation of RmlUi does not reliably cross from one column to the other).
                Rml::Element *details = byId("mod-details");
                Rml::ElementList buttons;
                if (details)
                    details->QuerySelectorAll(buttons, ".mbtn");
                for (Rml::Element *e = target; e; e = e->GetParentNode())
                {
                    if (key == Rml::Input::KI_RIGHT && e->IsClassSet("mod"))
                    {
                        if (!buttons.empty())
                        {
                            buttons.front()->Focus(true);
                            event.StopPropagation();
                        }
                        return;
                    }
                    if (key == Rml::Input::KI_LEFT && e->IsClassSet("mbtn") && !buttons.empty() && buttons.front() == e)
                    {
                        focusSelectedRow();
                        event.StopPropagation();
                        return;
                    }
                }
            }
            if ((key == Rml::Input::KI_DOWN || key == Rml::Input::KI_UP) && !m_confirming)
            {
                // Inside the details: the settings rows and then the row of buttons form one column (Up and Down), and the buttons
                // are side by side (Left and Right, which the focus navigation of RmlUi handles). Up from the first setting, or
                // from the buttons when there is no setting, goes back to the list; Down from the buttons is left to the focus
                // navigation (it reaches Quit Game and Back).
                Rml::Element *details = byId("mod-details");
                if (details)
                {
                    Rml::ElementList settingRows;
                    Rml::ElementList buttons;
                    details->QuerySelectorAll(settingRows, ".mrow");
                    details->QuerySelectorAll(buttons, ".mbtn");
                    for (Rml::Element *e = target; e; e = e->GetParentNode())
                    {
                        const bool onSetting = e->IsClassSet("mrow");
                        const bool onButton = e->IsClassSet("mbtn");
                        if (!onSetting && !onButton)
                            continue;
                        size_t index = 0;
                        if (onSetting)
                            while (index < settingRows.size() && settingRows[index] != e)
                                ++index;
                        if (key == Rml::Input::KI_UP)
                        {
                            if (onSetting && index > 0)
                                settingRows[index - 1]->Focus(true);
                            else if (onButton && !settingRows.empty())
                                settingRows.back()->Focus(true);
                            else
                                focusSelectedRow();
                            event.StopPropagation();
                            return;
                        }
                        if (onSetting)
                        {
                            if (index + 1 < settingRows.size())
                                settingRows[index + 1]->Focus(true);
                            else if (!buttons.empty())
                                buttons.front()->Focus(true);
                            event.StopPropagation();
                            return;
                        }
                        break; // Down from a button: the focus navigation decides
                    }
                }
                // The tab bar and the list: Down from a tab goes to the list, Up from its first mod goes to the Mods tab. The focus
                // navigation of RmlUi picks the nearest element by position, which from a tab at the right edge would skip the list.
                Rml::Element *panel = byId("panel-mods");
                if (!panel || panel->IsClassSet("hidden"))
                    return;
                for (Rml::Element *e = target; e; e = e->GetParentNode())
                {
                    if (key == Rml::Input::KI_DOWN && e->IsClassSet("tab"))
                    {
                        // With no mods installed (a modder's usual start) the modder's buttons are what the controller can reach.
                        Rml::Element *list = byId("mods-list");
                        Rml::ElementList rows;
                        if (list)
                            list->QuerySelectorAll(rows, ".mod");
                        Rml::Element *dump = byId("dump-textures");
                        const bool toolsShown = m_tools.devPackLine && !m_tools.devPackLine().empty();
                        if (rows.empty() && dump && toolsShown)
                            dump->Focus(true);
                        else
                            focusSelectedRow();
                        event.StopPropagation();
                        return;
                    }
                    if (key == Rml::Input::KI_UP && e->IsClassSet("mod"))
                    {
                        Rml::Element *list = byId("mods-list");
                        Rml::ElementList rows;
                        if (list)
                            list->QuerySelectorAll(rows, ".mod");
                        if (!rows.empty() && rows.front() == e)
                        {
                            if (Rml::Element *tab = byId("tab-mods"))
                            {
                                tab->Focus(true);
                                event.StopPropagation();
                            }
                        }
                        return;
                    }
                }
            }
        }
    }
}
