#include "ui_settings.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace ps2x::ui
{
    namespace
    {
        std::vector<std::string> split(const std::string &text, char separator)
        {
            std::vector<std::string> parts;
            size_t start = 0;
            while (true)
            {
                const size_t end = text.find(separator, start);
                parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            return parts;
        }

        std::string attr(const Rml::Element *e, const char *name, const std::string &fallback = std::string())
        {
            return e->GetAttribute<Rml::String>(name, fallback);
        }

        int attrInt(const Rml::Element *e, const char *name, int fallback)
        {
            const std::string text = attr(e, name);
            return text.empty() ? fallback : std::atoi(text.c_str());
        }

        std::string upper(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return s;
        }
    }

    SettingsPage::~SettingsPage()
    {
        detach();
    }

    bool SettingsPage::attach(Rml::ElementDocument *document)
    {
        detach();
        if (!document)
            return false;
        m_document = document;

        Rml::ElementList tabs;
        Rml::ElementList rows;
        document->QuerySelectorAll(tabs, ".tab");
        document->QuerySelectorAll(rows, ".row");
        Rml::Element *help = document->GetElementById("help-text");
        if (tabs.empty() || rows.empty() || !help)
        {
            Rml::Log::Message(Rml::Log::LT_ERROR, "SettingsPage: the page needs .tab and .row elements and #help-text");
            m_document = nullptr;
            return false;
        }

        // Events that bubble are heard once on the document. focus and blur do not bubble, so those are per element.
        for (const char *type : {"click", "keydown", "mousedown", "mousemove", "mouseup", "mouseover"})
            document->AddEventListener(type, this);
        for (Rml::Element *row : rows)
        {
            row->AddEventListener("focus", this);
            refreshRow(row);
        }
        for (Rml::Element *tab : tabs)
            tab->AddEventListener("focus", this);
        if (Rml::Element *back = document->GetElementById("back"))
            back->AddEventListener("focus", this);

        // The tab that is marked selected in the RML, else the first one.
        std::string initial;
        for (Rml::Element *tab : tabs)
            if (tab->IsClassSet("selected"))
                initial = attr(tab, "data-tab");
        if (initial.empty())
            initial = attr(tabs.front(), "data-tab");
        selectTab(initial);
        return true;
    }

    void SettingsPage::detach()
    {
        if (!m_document)
            return;
        for (const char *type : {"click", "keydown", "mousedown", "mousemove", "mouseup", "mouseover"})
            m_document->RemoveEventListener(type, this);
        Rml::ElementList elements;
        m_document->QuerySelectorAll(elements, ".row, .tab, #back");
        for (Rml::Element *e : elements)
            e->RemoveEventListener("focus", this);
        m_document = nullptr;
        m_dragRow = nullptr;
    }

    Rml::Element *SettingsPage::rowForKey(const std::string &key) const
    {
        if (!m_document)
            return nullptr;
        Rml::ElementList rows;
        m_document->QuerySelectorAll(rows, ".row");
        for (Rml::Element *row : rows)
            if (attr(row, "data-key") == key)
                return row;
        return nullptr;
    }

    Rml::Element *SettingsPage::rowOf(Rml::Element *element) const
    {
        for (; element; element = element->GetParentNode())
            if (element->IsClassSet("row"))
                return element;
        return nullptr;
    }

    Rml::Element *SettingsPage::panelOf(Rml::Element *element) const
    {
        for (; element; element = element->GetParentNode())
            if (element->IsClassSet("panel"))
                return element;
        return nullptr;
    }

    std::string SettingsPage::value(const std::string &key) const
    {
        if (Rml::Element *row = rowForKey(key))
            return attr(row, "data-value");
        return std::string();
    }

    bool SettingsPage::setValue(const std::string &key, const std::string &value)
    {
        Rml::Element *row = rowForKey(key);
        if (!row)
            return false;
        row->SetAttribute("data-value", value);
        refreshRow(row);
        return true;
    }

    // Shows the row's value: "[ON]" / "[16:9]" / "[85%]", and sets a slider's fill.
    void SettingsPage::refreshRow(Rml::Element *row)
    {
        const std::string kind = attr(row, "data-kind");
        const std::string value = attr(row, "data-value");
        std::string text;
        if (kind == "toggle")
        {
            text = "[" + upper(value) + "]";
        }
        else if (kind == "slider")
        {
            const int lo = attrInt(row, "data-min", 0);
            const int hi = std::max(attrInt(row, "data-max", 100), lo + 1);
            const int v = std::clamp(std::atoi(value.c_str()), lo, hi);
            text = "[" + std::to_string(v) + attr(row, "data-suffix") + "]";
            if (Rml::Element *fill = row->QuerySelector(".fill"))
            {
                char width[32];
                std::snprintf(width, sizeof(width), "%.2f%%", 100.0 * (v - lo) / (hi - lo));
                fill->SetProperty("width", width);
            }
        }
        else
        {
            text = "[" + value + "]";
        }
        if (Rml::Element *shown = row->QuerySelector(".value"))
            shown->SetInnerRML(text);
    }

    // direction: -1 / +1 for Left / Right, 0 for activation (Return or a click).
    void SettingsPage::changeRow(Rml::Element *row, int direction, bool activate)
    {
        const std::string kind = attr(row, "data-kind");
        const std::string key = attr(row, "data-key");
        const std::string old = attr(row, "data-value");
        std::string now = old;
        if (kind == "toggle")
        {
            now = (old == "on") ? "off" : "on";
        }
        else if (kind == "choice")
        {
            const std::vector<std::string> choices = split(attr(row, "data-choices"), '|');
            if (choices.empty())
                return;
            auto it = std::find(choices.begin(), choices.end(), old);
            int index = it == choices.end() ? 0 : static_cast<int>(it - choices.begin());
            const int step = activate ? 1 : direction;
            index = (index + step + static_cast<int>(choices.size())) % static_cast<int>(choices.size());
            now = choices[static_cast<size_t>(index)];
        }
        else if (kind == "slider")
        {
            if (activate)
                return; // a slider has nothing to activate; Left/Right move it
            const int lo = attrInt(row, "data-min", 0);
            const int hi = std::max(attrInt(row, "data-max", 100), lo + 1);
            const int step = std::max(attrInt(row, "data-step", 5), 1);
            const int v = std::clamp(std::atoi(old.c_str()) + direction * step, lo, hi);
            now = std::to_string(v);
        }
        if (now == old)
            return;
        row->SetAttribute("data-value", now);
        refreshRow(row);
        if (m_onChange)
            m_onChange(key, now);
    }

    void SettingsPage::setSliderFromMouse(Rml::Element *row, float mouseX)
    {
        Rml::Element *track = row->QuerySelector(".track");
        if (!track)
            return;
        const float left = track->GetAbsoluteLeft();
        const float width = track->GetOffsetWidth();
        if (width <= 0.0f)
            return;
        const int lo = attrInt(row, "data-min", 0);
        const int hi = std::max(attrInt(row, "data-max", 100), lo + 1);
        const int step = std::max(attrInt(row, "data-step", 5), 1);
        const float t = std::clamp((mouseX - left) / width, 0.0f, 1.0f);
        int v = lo + static_cast<int>(std::lround((hi - lo) * t / step)) * step;
        v = std::clamp(v, lo, hi);
        const std::string now = std::to_string(v);
        if (now == attr(row, "data-value"))
            return;
        row->SetAttribute("data-value", now);
        refreshRow(row);
        if (m_onChange)
            m_onChange(attr(row, "data-key"), now);
    }

    void SettingsPage::showHelp(Rml::Element *element)
    {
        if (!m_document)
            return;
        if (Rml::Element *help = m_document->GetElementById("help-text"))
        {
            std::string text = attr(element, "data-help");
            if (text.empty() && element->GetId() == "back")
                text = "Close the settings.";
            help->SetInnerRML(text);
        }
    }

    void SettingsPage::selectTab(const std::string &name)
    {
        if (!m_document)
            return;
        Rml::ElementList tabs;
        m_document->QuerySelectorAll(tabs, ".tab");
        bool found = false;
        for (Rml::Element *tab : tabs)
            found = found || attr(tab, "data-tab") == name;
        if (!found)
            return;
        m_tab = name;
        for (Rml::Element *tab : tabs)
            tab->SetClass("selected", attr(tab, "data-tab") == name);
        Rml::ElementList panels;
        m_document->QuerySelectorAll(panels, ".panel");
        for (Rml::Element *panel : panels)
            panel->SetClass("hidden", panel->GetId() != "panel-" + name);

        // A focused row inside a panel that just disappeared would leave the focus on an invisible element.
        Rml::Element *focus = m_document->GetFocusLeafNode();
        if (Rml::Element *row = rowOf(focus))
        {
            if (Rml::Element *panel = panelOf(row); panel && panel->IsClassSet("hidden"))
                focusFirstRow();
        }
    }

    void SettingsPage::switchTab(int delta)
    {
        if (!m_document)
            return;
        Rml::ElementList tabs;
        m_document->QuerySelectorAll(tabs, ".tab");
        int index = 0;
        for (size_t i = 0; i < tabs.size(); ++i)
            if (attr(tabs[i], "data-tab") == m_tab)
                index = static_cast<int>(i);
        const int count = static_cast<int>(tabs.size());
        index = (index + delta + count) % count;
        selectTab(attr(tabs[static_cast<size_t>(index)], "data-tab"));
    }

    void SettingsPage::focusFirstRow()
    {
        if (!m_document)
            return;
        Rml::Element *panel = m_document->GetElementById("panel-" + m_tab);
        if (!panel)
            return;
        if (Rml::Element *row = panel->QuerySelector(".row"))
            row->Focus(true);
    }

    void SettingsPage::ProcessEvent(Rml::Event &event)
    {
        Rml::Element *target = event.GetTargetElement();
        const Rml::String &type = event.GetType();

        if (type == "focus")
        {
            showHelp(event.GetCurrentElement());
        }
        else if (type == "mouseover")
        {
            // The highlight follows the mouse, as the game's cursor does: hovering a row focuses it.
            if (Rml::Element *row = rowOf(target))
                if (!row->IsPseudoClassSet("focus") && !m_dragRow)
                    row->Focus(false);
        }
        else if (type == "mousedown")
        {
            if (event.GetParameter<int>("button", 0) != 0)
                return;
            Rml::Element *row = rowOf(target);
            if (row && attr(row, "data-kind") == "slider" && target)
            {
                // Anywhere on the slider (track or fill) starts a drag.
                for (Rml::Element *e = target; e && e != row->GetParentNode(); e = e->GetParentNode())
                {
                    if (e->IsClassSet("slider") || e->IsClassSet("track") || e->IsClassSet("fill"))
                    {
                        m_dragRow = row;
                        row->Focus(false);
                        setSliderFromMouse(row, event.GetParameter<float>("mouse_x", 0.0f));
                        break;
                    }
                }
            }
        }
        else if (type == "mousemove")
        {
            if (m_dragRow)
                setSliderFromMouse(m_dragRow, event.GetParameter<float>("mouse_x", 0.0f));
        }
        else if (type == "mouseup")
        {
            m_dragRow = nullptr;
        }
        else if (type == "click")
        {
            for (Rml::Element *e = target; e; e = e->GetParentNode())
            {
                if (e->IsClassSet("tab"))
                {
                    selectTab(attr(e, "data-tab"));
                    return;
                }
                if (e->GetId() == "back")
                {
                    if (m_onBack)
                        m_onBack();
                    return;
                }
                if (e->IsClassSet("row"))
                {
                    // Sliders react to the mouse in mousedown; toggles and choices step on a click or Return.
                    if (attr(e, "data-kind") != "slider")
                        changeRow(e, 1, true);
                    return;
                }
            }
        }
        else if (type == "keydown")
        {
            const int key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
            if (key == Rml::Input::KI_PRIOR || key == Rml::Input::KI_NEXT)
            {
                switchTab(key == Rml::Input::KI_NEXT ? 1 : -1);
                event.StopPropagation();
            }
            else if (key == Rml::Input::KI_ESCAPE)
            {
                if (m_onBack)
                    m_onBack();
                event.StopPropagation();
            }
            else if (key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT)
            {
                if (Rml::Element *row = rowOf(target))
                {
                    changeRow(row, key == Rml::Input::KI_RIGHT ? 1 : -1, false);
                    event.StopPropagation(); // not for focus navigation: Left/Right belong to the row
                }
            }
        }
    }
}
