#include "ui_overlay.h"

#include "ui_input.h"
#include "ui_render_sdlgpu.h"

#include <RmlUi/Core.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>

namespace ps2x::ui
{
    namespace
    {
        // The UI is designed for a 720 pixel high window at scale 1.0 (1 dp = 1 pixel there). The page fills the whole window at any shape
        // (it is a flex layout, see assets/theme/fpr.rcss), so the scale follows the window's height; the width only matters when the
        // window is so narrow that the Mods tab would no longer fit: the page needs about 840 dp across.
        constexpr float kDesignHeight = 720.0f;
        constexpr float kDesignWidth = 840.0f;
        // A small window would make the text too small to read (a 480 pixel high one would give 12 pixel text), so the scale does not
        // fall under kSmallScale while the window is at least kLeastHeight dp high at that scale (the layout is 540 dp high at the
        // least: the content box shrinks and scrolls). A window shorter than that scales down with its height again.
        constexpr float kSmallScale = 0.75f;
        constexpr float kLeastHeight = 540.0f;

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

        // RmlUi's window onto files. Real files are opened through SDL (which takes UTF-8 paths on every system, unlike fopen on
        // Windows). A name that starts with "mod:" belongs to a mod: it is asked of the reader in OverlayConfig::virtualFiles
        // and served from memory.
        class FileInterface final : public Rml::FileInterface
        {
        public:
            void setVirtualReader(std::function<bool(const std::string &, std::vector<uint8_t> &)> reader) { m_reader = std::move(reader); }

            Rml::FileHandle Open(const Rml::String &path) override
            {
                if (path.compare(0, 4, "mod:") == 0)
                {
                    if (!m_reader)
                        return 0;
                    auto *file = new File();
                    if (!m_reader(path, file->bytes))
                    {
                        delete file;
                        return 0;
                    }
                    return reinterpret_cast<Rml::FileHandle>(file);
                }
                SDL_IOStream *io = SDL_IOFromFile(path.c_str(), "rb");
                if (!io)
                    return 0;
                auto *file = new File();
                file->io = io;
                return reinterpret_cast<Rml::FileHandle>(file);
            }

            void Close(Rml::FileHandle handle) override
            {
                auto *file = reinterpret_cast<File *>(handle);
                if (file->io)
                    SDL_CloseIO(file->io);
                delete file;
            }

            size_t Read(void *buffer, size_t size, Rml::FileHandle handle) override
            {
                auto *file = reinterpret_cast<File *>(handle);
                if (file->io)
                    return SDL_ReadIO(file->io, buffer, size);
                const size_t available = file->bytes.size() - std::min(file->position, file->bytes.size());
                const size_t count = std::min(size, available);
                if (count)
                    std::memcpy(buffer, file->bytes.data() + file->position, count);
                file->position += count;
                return count;
            }

            bool Seek(Rml::FileHandle handle, long offset, int origin) override
            {
                auto *file = reinterpret_cast<File *>(handle);
                if (file->io)
                {
                    const SDL_IOWhence whence = origin == SEEK_SET ? SDL_IO_SEEK_SET : (origin == SEEK_CUR ? SDL_IO_SEEK_CUR : SDL_IO_SEEK_END);
                    return SDL_SeekIO(file->io, offset, whence) >= 0;
                }
                long long target = offset;
                if (origin == SEEK_CUR)
                    target += static_cast<long long>(file->position);
                else if (origin == SEEK_END)
                    target += static_cast<long long>(file->bytes.size());
                if (target < 0 || target > static_cast<long long>(file->bytes.size()))
                    return false;
                file->position = static_cast<size_t>(target);
                return true;
            }

            size_t Tell(Rml::FileHandle handle) override
            {
                auto *file = reinterpret_cast<File *>(handle);
                if (file->io)
                {
                    const Sint64 at = SDL_TellIO(file->io);
                    return at < 0 ? 0 : static_cast<size_t>(at);
                }
                return file->position;
            }

        private:
            struct File
            {
                SDL_IOStream *io = nullptr;
                std::vector<uint8_t> bytes;
                size_t position = 0;
            };
            std::function<bool(const std::string &, std::vector<uint8_t> &)> m_reader;
        };

        std::string escapeAttribute(const std::string &text)
        {
            std::string out;
            for (const char c : text)
            {
                switch (c)
                {
                case '&': out += "&amp;"; break;
                case '"': out += "&quot;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                default: out += c; break;
                }
            }
            return out;
        }

        bool g_rmlInitialised = false; // RmlUi's own state is process-wide, so only one Overlay at a time
    }

    struct Overlay::Impl
    {
        explicit Impl(SDL_Window *window) : system(window) {}

        SystemInterface system;
        FileInterface files;
        std::vector<std::string> extraStyleSheets;
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

        impl->files.setVirtualReader(config.virtualFiles);
        impl->extraStyleSheets = config.extraStyleSheets;

        // The interfaces must be set before RmlUi starts.
        Rml::SetSystemInterface(&impl->system);
        Rml::SetFileInterface(&impl->files);
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
        // Automatic scale: height / 720 (see kSmallScale for small windows), but not so large that the page would be narrower than
        // kDesignWidth, rounded down to a multiple of 1/8. RmlUi builds a font atlas for every font size it is asked for and keeps it
        // until shutdown, so a scale that followed every pixel of a window drag would pile up atlases (a resize storm left 60
        // textures alive); in steps there are few. Rounding down keeps the page inside the window.
        const float byHeight = std::max(static_cast<float>(height) / kDesignHeight, std::min(kSmallScale, static_cast<float>(height) / kLeastHeight));
        const float fit = std::min(byHeight, static_cast<float>(width) / kDesignWidth);
        const float effective = scale > 0.0f ? scale : std::max(0.25f, std::floor(fit * 8.0f) / 8.0f);
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

    void Overlay::setMouseGuard(bool on)
    {
        if (m_impl)
            m_impl->input.setMouseGuard(on);
    }

    Rml::ElementDocument *Overlay::loadPage(const std::string &relativePath)
    {
        if (!m_impl)
            return nullptr;
        const std::string path = m_impl->assetRoot + "/" + relativePath;
        Rml::ElementDocument *document = nullptr;
        if (m_impl->extraStyleSheets.empty())
        {
            document = m_impl->context->LoadDocument(path);
        }
        else
        {
            // The theme mods' style sheets go after the page's own links, so they win where they say the same thing. RmlUi
            // reads a page's links while it parses it, so the links are added to the text and the page is loaded from memory
            // (with its real path as the source, so its own relative links still resolve).
            std::string text;
            if (SDL_IOStream *io = SDL_IOFromFile(path.c_str(), "rb"))
            {
                size_t size = 0;
                void *data = SDL_LoadFile_IO(io, &size, true);
                if (data)
                {
                    text.assign(static_cast<const char *>(data), size);
                    SDL_free(data);
                }
            }
            const size_t headEnd = text.find("</head>");
            if (text.empty() || headEnd == std::string::npos)
            {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[ui] page %s has no </head>; the theme mods' style sheets are not added",
                            relativePath.c_str());
                document = m_impl->context->LoadDocument(path);
            }
            else
            {
                std::string links;
                for (const std::string &sheet : m_impl->extraStyleSheets)
                    links += "    <link type=\"text/rcss\" href=\"" + escapeAttribute(sheet) + "\"/>\n";
                text.insert(headEnd, links);
                // RmlUi writes a drive letter's colon as '|' in the URL of a source it opens itself (StreamFile::Open) and turns it
                // back when it joins a relative link; a source given from memory has to follow the same convention.
                std::string sourceUrl = path;
                std::replace(sourceUrl.begin(), sourceUrl.end(), ':', '|');
                document = m_impl->context->LoadDocumentFromMemory(text, sourceUrl);
            }
        }
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
