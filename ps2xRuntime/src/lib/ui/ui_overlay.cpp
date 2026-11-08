#include "ui_overlay.h"

#include "ui_input.h"
#include "ui_render_sdlgpu.h"

#include <RmlUi/Core.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace ps2x::ui
{
    namespace
    {
        // The UI is designed for a 720 pixel high window at scale 1.0 (1 dp = 1 pixel there).
        constexpr float kDesignHeight = 720.0f;

        // RmlUi's window onto the platform: time, log, the mouse cursor and the clipboard, through SDL.
        class SystemInterface final : public Rml::SystemInterface
        {
        public:
            explicit SystemInterface(SDL_Window *window) : m_window(window) {}

            ~SystemInterface() override
            {
                for (auto &entry : m_cursors)
                    if (entry.second)
                        SDL_DestroyCursor(entry.second);
            }

            double GetElapsedTime() override { return static_cast<double>(SDL_GetTicksNS()) * 1e-9; }

            bool LogMessage(Rml::Log::Type type, const Rml::String &message) override
            {
                switch (type)
                {
                case Rml::Log::LT_ERROR:
                case Rml::Log::LT_ASSERT:
                    ++counts.errors;
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[ui] %s", message.c_str());
                    break;
                case Rml::Log::LT_WARNING:
                    ++counts.warnings;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[ui] %s", message.c_str());
                    break;
                default:
                    ++counts.infos;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[ui] %s", message.c_str());
                    break;
                }
                return true;
            }

            void SetMouseCursor(const Rml::String &name) override
            {
                if (!m_window)
                    return;
                SDL_SystemCursor id = SDL_SYSTEM_CURSOR_DEFAULT;
                if (name == "move")
                    id = SDL_SYSTEM_CURSOR_MOVE;
                else if (name == "pointer")
                    id = SDL_SYSTEM_CURSOR_POINTER;
                else if (name == "resize")
                    id = SDL_SYSTEM_CURSOR_NWSE_RESIZE;
                else if (name == "cross")
                    id = SDL_SYSTEM_CURSOR_CROSSHAIR;
                else if (name == "text")
                    id = SDL_SYSTEM_CURSOR_TEXT;
                else if (name == "unavailable")
                    id = SDL_SYSTEM_CURSOR_NOT_ALLOWED;
                SDL_Cursor *&cursor = m_cursors[id];
                if (!cursor)
                    cursor = SDL_CreateSystemCursor(id);
                if (cursor)
                    SDL_SetCursor(cursor);
            }

            void SetClipboardText(const Rml::String &text) override { SDL_SetClipboardText(text.c_str()); }

            void GetClipboardText(Rml::String &text) override
            {
                if (char *clip = SDL_GetClipboardText())
                {
                    text = clip;
                    SDL_free(clip);
                }
            }

            LogCounts counts;

        private:
            SDL_Window *m_window;
            std::map<SDL_SystemCursor, SDL_Cursor *> m_cursors;
        };

        bool g_rmlInitialised = false; // RmlUi's own state is process-wide, so only one Overlay at a time
    }

    struct Overlay::Impl
    {
        explicit Impl(SDL_Window *window) : system(window) {}

        SystemInterface system;
        SdlGpuRenderer renderer;
        Input input;
        Rml::Context *context = nullptr;
        std::string assetRoot;
        int width = 0;
        int height = 0;
        float scale = 1.0f;
        float requestedScale = 0.0f;
        bool visible = false;
    };

    Overlay::Overlay() = default;

    Overlay::~Overlay()
    {
        shutdown();
    }

    bool Overlay::init(const OverlayConfig &config)
    {
        shutdown();
        if (g_rmlInitialised)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[ui] only one Overlay can exist at a time");
            return false;
        }
        if (!config.device || config.assetRoot.empty())
            return false;

        auto impl = std::make_unique<Impl>(config.window);
        impl->assetRoot = config.assetRoot;
        while (!impl->assetRoot.empty() && (impl->assetRoot.back() == '/' || impl->assetRoot.back() == '\\'))
            impl->assetRoot.pop_back();

        if (!impl->renderer.init(config.device, config.targetFormat))
            return false;

        // The interfaces must be set before RmlUi starts.
        Rml::SetSystemInterface(&impl->system);
        Rml::SetRenderInterface(&impl->renderer);
        if (!Rml::Initialise())
        {
            impl->renderer.shutdown();
            return false;
        }
        g_rmlInitialised = true;

        // Lato (SIL OFL 1.1, assets/fonts): regular, italic, bold and bold italic. The game's own menus are bold
        // italic, so that face is the main one; the first face loaded is also the fallback for missing glyphs.
        static const std::array<const char *, 4> faces = {"LatoLatin-Regular.ttf", "LatoLatin-Italic.ttf", "LatoLatin-Bold.ttf",
                                                          "LatoLatin-BoldItalic.ttf"};
        bool first = true;
        for (const char *face : faces)
        {
            if (!Rml::LoadFontFace(impl->assetRoot + "/fonts/" + face, first))
            {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[ui] cannot load font %s/fonts/%s", impl->assetRoot.c_str(), face);
                Rml::Shutdown();
                g_rmlInitialised = false;
                impl->renderer.shutdown();
                return false;
            }
            first = false;
        }

        impl->width = 1280;
        impl->height = 720;
        impl->context = Rml::CreateContext("ps2x", Rml::Vector2i(impl->width, impl->height));
        if (!impl->context)
        {
            Rml::Shutdown();
            g_rmlInitialised = false;
            impl->renderer.shutdown();
            return false;
        }
        impl->input.setContext(impl->context);
        m_impl = std::move(impl);
        return true;
    }

    void Overlay::shutdown()
    {
        if (!m_impl)
            return;
        // Shutdown releases every texture and geometry through the renderer, so the renderer goes last.
        Rml::Shutdown();
        g_rmlInitialised = false;
        m_impl->renderer.shutdown();
        m_impl.reset();
    }

    void Overlay::setViewport(int width, int height, float scale)
    {
        if (!m_impl || width <= 0 || height <= 0)
            return;
        Impl &s = *m_impl;
        // Automatic scale: height / 720, rounded down to a multiple of 1/8. RmlUi builds a font atlas for every font
        // size it is asked for and keeps it until shutdown, so a scale that followed every pixel of a window drag
        // would pile up atlases (a resize storm left 60 textures alive); in steps there are few. Rounding down
        // keeps the page inside the window.
        const float effective =
            scale > 0.0f ? scale : std::max(0.25f, std::floor(static_cast<float>(height) / kDesignHeight * 8.0f) / 8.0f);
        if (width == s.width && height == s.height && effective == s.scale && scale == s.requestedScale)
            return;
        s.width = width;
        s.height = height;
        s.scale = effective;
        s.requestedScale = scale;
        s.context->SetDimensions(Rml::Vector2i(width, height));
        s.context->SetDensityIndependentPixelRatio(effective);
    }

    int Overlay::width() const { return m_impl ? m_impl->width : 0; }
    int Overlay::height() const { return m_impl ? m_impl->height : 0; }
    float Overlay::scale() const { return m_impl ? m_impl->scale : 1.0f; }

    void Overlay::setPixelDensity(float density)
    {
        if (m_impl)
            m_impl->input.setPixelDensity(density);
    }

    Rml::ElementDocument *Overlay::loadPage(const std::string &relativePath)
    {
        if (!m_impl)
            return nullptr;
        Rml::ElementDocument *document = m_impl->context->LoadDocument(m_impl->assetRoot + "/" + relativePath);
        if (!document)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[ui] cannot load page %s", relativePath.c_str());
            return nullptr;
        }
        document->Show();
        return document;
    }

    void Overlay::closePages()
    {
        if (m_impl)
            m_impl->context->UnloadAllDocuments();
    }

    Rml::Context *Overlay::context()
    {
        return m_impl ? m_impl->context : nullptr;
    }

    void Overlay::setVisible(bool visible)
    {
        if (!m_impl || m_impl->visible == visible)
            return;
        m_impl->visible = visible;
        if (!visible)
            m_impl->input.releaseAll(); // nothing may stay held while the game has the controller again
    }

    bool Overlay::visible() const
    {
        return m_impl && m_impl->visible;
    }

    bool Overlay::handleEvent(const SDL_Event &event)
    {
        if (!m_impl || !m_impl->visible)
            return false;
        m_impl->input.handleEvent(event);
        return Input::isInputEvent(event.type);
    }

    void Overlay::update(double seconds)
    {
        if (!m_impl || !m_impl->visible)
            return;
        m_impl->input.update(seconds);
        m_impl->context->Update();
    }

    void Overlay::record(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target)
    {
        if (!m_impl || !m_impl->visible)
            return;
        Impl &s = *m_impl;
        s.renderer.beginFrame(cmd, target, static_cast<uint32_t>(s.width), static_cast<uint32_t>(s.height));
        s.context->Render();
        s.renderer.endFrame();
    }

    SdlGpuRenderer &Overlay::renderer()
    {
        return m_impl->renderer;
    }

    LogCounts Overlay::logCounts() const
    {
        return m_impl ? m_impl->system.counts : LogCounts{};
    }
}
