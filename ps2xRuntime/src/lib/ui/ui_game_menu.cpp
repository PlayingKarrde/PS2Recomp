#include "ui_game_menu.h"

#include "ui_settings.h"

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
            if (!page.attach(document))
                return false;
            page.setOnChange([this](const std::string &key, const std::string &value) {
                host->change(key, value);
                refresh();
            });
            page.setOnBack([this]() { host->closeRequested(); });
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
        m_impl->fillControls();
        m_impl->refresh();
        m_impl->overlay.setVisible(true);
        m_impl->page.focusFirstRow();
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

    Overlay &GameMenu::overlay()
    {
        return m_impl->overlay;
    }
}
