#include "ui_game_menu.h"

#include "ui_settings.h"

#if defined(PS2X_UI_MODS)
#include "mod_manager.h"
#include "ui_mods.h"
#endif

#include <RmlUi/Core.h>

#include <SDL3/SDL_log.h>

#include <algorithm>

namespace ps2x::ui
{
    namespace
    {
        // HTML-safe text for SetInnerRML (control names and the controller's name come from the host).
        std::string escape(const std::string &text)
        {
            std::string out;
            for (const char c : text)
            {
                switch (c)
                {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                default: out += c; break;
                }
            }
            return out;
        }
    }

    struct GameMenu::Impl
    {
        Overlay overlay;
        SettingsPage page;
        MenuHost *host = nullptr;
        Rml::ElementDocument *document = nullptr;
        bool built = false;
#if defined(PS2X_UI_MODS)
        ModsPage mods;
        bool modsAttached = false;
#endif

        // Settings that only work with the GPU renderer.
        static bool needsGpuRenderer(const std::string &key)
        {
            return key == "resolution" || key == "aspect";
        }

        bool loadPage()
        {
            if (built)
                return true;
            document = overlay.loadPage("pages/game_settings.rml");
            if (!document)
                return false;
            // Without a mod manager the Mods tab is not part of the menu: it is removed before the settings page looks for its tabs.
            bool withMods = false;
#if defined(PS2X_UI_MODS)
            withMods = host->modManager() != nullptr;
#endif
            if (!withMods)
            {
                for (const char *id : {"tab-mods", "panel-mods", "mod-confirm"})
                    if (Rml::Element *element = document->GetElementById(id))
                        if (Rml::Element *parent = element->GetParentNode())
                            parent->RemoveChild(element);
            }
            if (!page.attach(document))
                return false;
            page.setOnChange([this](const std::string &key, const std::string &value) {
                host->change(key, value);
                refresh();
            });
            page.setOnBack([this]() { host->closeRequested(); });
            page.setOnQuit([this]() { host->quitRequested(); });
#if defined(PS2X_UI_MODS)
            if (withMods && mods.attach(document, host->modManager()))
            {
                mods.setOnChange([this]() { refresh(); });
                modsAttached = true;
            }
#endif
            built = true;
            return true;
        }

        void fillControls()
        {
            if (!document)
                return;
            Rml::Element *list = document->GetElementById("controls-list");
            if (!list)
                return;
            std::string rml = "<div class=\"ctl head\"><span class=\"c-act\">Button</span><span class=\"c-key\">Keyboard</span>"
                              "<span class=\"c-pad\">Controller</span></div>";
            for (const ControlLine &line : host->controls())
            {
                rml += "<div class=\"ctl\"><span class=\"c-act\">" + escape(line.action) + "</span><span class=\"c-key\">" +
                       escape(line.keyboard) + "</span><span class=\"c-pad\">" + escape(line.controller) + "</span></div>";
            }
            list->SetInnerRML(rml);
            if (Rml::Element *pad = document->GetElementById("controller-name"))
            {
                const std::string name = host->controllerName();
                pad->SetInnerRML(name.empty() ? "No controller connected. Plug one in and it is used at once."
                                              : "Controller in use: " + escape(name));
            }
        }

        void refresh()
        {
            if (!built)
                return;
            const bool cpu = host->value("renderer") == "cpu";
            std::string restartNames;
            for (const std::string &key : page.keys())
            {
                page.setValue(key, host->value(key));
                std::string envName;
                std::string reason;
                std::string tag;
                bool locked = false;
                if (host->fixedByEnvironment(key, envName))
                {
                    locked = true;
                    reason = "Fixed by the environment variable " + envName + ".";
                    tag = "ENV";
                }
                else if (cpu && needsGpuRenderer(key))
                {
                    locked = true;
                    reason = "Needs the GPU renderer.";
                }
                if (!locked && page.attribute(key, "data-restart") == "1" && host->value(key) != host->startupValue(key))
                {
                    tag = "RESTART";
                    const std::string label = page.attribute(key, "data-name");
                    restartNames += (restartNames.empty() ? "" : ", ") + (label.empty() ? key : label);
                }
                page.setLocked(key, locked, reason, tag);
            }
            page.setQuitLocked(host->quitBlockedReason());
#if defined(PS2X_UI_MODS)
            if (modsAttached && host->modManager() && host->modManager()->restartNeeded())
                restartNames += std::string(restartNames.empty() ? "" : ", ") + "Mods";
#endif
            if (document)
            {
                if (Rml::Element *note = document->GetElementById("restart-note"))
                {
                    note->SetInnerRML(restartNames.empty() ? std::string()
                                                           : "Restart the game to apply: " + escape(restartNames) + ".");
                    note->SetClass("hidden", restartNames.empty());
                }
            }
        }
    };

    GameMenu::GameMenu() : m_impl(std::make_unique<Impl>()) {}

    GameMenu::~GameMenu()
    {
        shutdown();
    }

    bool GameMenu::init(const OverlayConfig &config, MenuHost *host)
    {
        if (!host)
            return false;
        m_impl->host = host;
        return m_impl->overlay.init(config);
    }

    void GameMenu::shutdown()
    {
        if (!m_impl)
            return;
#if defined(PS2X_UI_MODS)
        m_impl->mods.detach();
        m_impl->modsAttached = false;
#endif
        m_impl->page.detach();
        m_impl->overlay.shutdown();
        m_impl->document = nullptr;
        m_impl->built = false;
    }

    bool GameMenu::ready() const
    {
        return m_impl && m_impl->overlay.ready();
    }

    void GameMenu::open()
    {
        if (!ready() || !m_impl->loadPage())
            return;
        m_impl->page.closeConfirm(); // a question left showing when the menu was closed
#if defined(PS2X_UI_MODS)
        if (m_impl->modsAttached)
        {
            m_impl->mods.closeConfirm();
            m_impl->host->modManager()->rescan(); // a file may have been added or removed by hand since the menu was last open
            m_impl->mods.setStatus(std::string());
            m_impl->mods.refresh();
        }
#endif
        m_impl->fillControls();
        m_impl->refresh();
        m_impl->overlay.setVisible(true);
        m_impl->page.focusFirstRow();
#if defined(PS2X_UI_MODS)
        if (m_impl->modsAttached && m_impl->page.selectedTab() == "mods")
            m_impl->mods.focusList();
#endif
    }

    void GameMenu::close()
    {
        if (ready())
            m_impl->overlay.setVisible(false);
    }

    bool GameMenu::visible() const
    {
        return ready() && m_impl->overlay.visible();
    }

    void GameMenu::refresh()
    {
        if (ready())
            m_impl->refresh();
    }

    void GameMenu::showMods(const std::string &message)
    {
#if defined(PS2X_UI_MODS)
        if (!ready() || !m_impl->loadPage() || !m_impl->modsAttached)
            return;
        m_impl->page.selectTab("mods");
        m_impl->mods.refresh();
        m_impl->mods.setStatus(message);
        m_impl->refresh();
        m_impl->mods.focusList();
#else
        (void)message;
#endif
    }

    Overlay &GameMenu::overlay()
    {
        return m_impl->overlay;
    }
}
