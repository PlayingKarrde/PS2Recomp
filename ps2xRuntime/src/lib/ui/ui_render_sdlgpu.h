// RmlUi renderer on SDL_GPU, for the UI overlay.
//
// RmlUi (the UI library) never draws anything itself. It lays out the page, turns every box, border and glyph
// into triangles, and calls a "render interface" to draw them. This class is that interface for SDL_GPU, the API
// the game's picture is already presented with. It is written for the overlay, not taken from RmlUi's own SDL_GPU
// backend (RmlUi 6.3, Backends/RmlUi_Renderer_SDL_GPU.cpp), because that one needs SDL3_image, ties its pipelines
// to a window's swapchain format (no offscreen target for tests) and has no gradients. The design follows it where
// it was sound (premultiplied alpha, one pipeline per kind of fill); RmlUi is MIT licensed, like that file.
//
// How a frame goes. The host (or the demo) owns the command buffer and the target texture, and has already drawn
// the game picture:
//
//     renderer.beginFrame(cmd, target, width, height);   // width, height: the target's size in pixels
//     context->Render();                                  // RmlUi calls RenderGeometry() and friends
//     renderer.endFrame();                                // uploads the geometry used, then draws it all
//
// beginFrame/endFrame only record draw calls into the list; endFrame does the GPU work: one copy pass that fills a
// vertex and an index buffer with every geometry used this frame, then one render pass over `target` (loading
// what is there, so the game picture shows through) that issues the draws in order. Nothing is submitted: the
// caller submits `cmd`. The render pass is a separate one from the game picture's, so the host records the UI
// after its own pass has ended.
//
// What is supported: coloured and textured triangles, scissor rectangles, matrix transforms, and RmlUi's gradient
// shaders (linear-gradient, radial-gradient, conic-gradient, repeating or not), which is what themes need for
// fills. Not supported, because they need offscreen layers or stencil: filters (blur, drop-shadow), box-shadow,
// mask-image, clip masks for rotated overflow, backdrop-filter. RmlUi logs a warning and skips them.
//
// Threads: use it from one thread, the one that owns the command buffer (SDL_GPU's rule). LoadTexture and
// GenerateTexture submit their own small command buffers, so they work outside frames.
#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ps2x::ui
{
    class SdlGpuRenderer final : public Rml::RenderInterface
    {
    public:
        SdlGpuRenderer(); // defined in the .cpp: the private types below are incomplete here
        ~SdlGpuRenderer() override;
        SdlGpuRenderer(const SdlGpuRenderer &) = delete;
        SdlGpuRenderer &operator=(const SdlGpuRenderer &) = delete;

        // Creates the pipelines and the sampler for render targets of colour format `targetFormat` (the swapchain's
        // format, or the format of the offscreen texture a test draws into). The device outlives the renderer.
        bool init(SDL_GPUDevice *device, SDL_GPUTextureFormat targetFormat);
        void shutdown();
        bool ready() const { return m_device != nullptr; }

        // See the comment at the top. `clearColour`, when given, clears the target first instead of loading it (the
        // demo's first pass). Draw calls made outside beginFrame/endFrame are ignored.
        void beginFrame(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, uint32_t width, uint32_t height,
                        const SDL_FColor *clearColour = nullptr);
        void endFrame();

        // Counters for the last endFrame (for tests and the cost measurement).
        struct FrameStats
        {
            uint32_t draws = 0;
            uint32_t geometries = 0;
            uint32_t vertices = 0;
            uint32_t indices = 0;
            uint32_t uniqueTextures = 0;
        };
        const FrameStats &lastFrame() const { return m_stats; }
        // Textures and geometries alive now.
        size_t textureCount() const { return m_textureCount; }
        size_t textureBytes() const { return m_textureBytes; }
        size_t geometryCount() const { return m_geometryCount; }

        // Rml::RenderInterface
        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
        void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
        void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
        Rml::TextureHandle LoadTexture(Rml::Vector2i &textureDimensions, const Rml::String &source) override;
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i sourceDimensions) override;
        void ReleaseTexture(Rml::TextureHandle texture) override;
        void EnableScissorRegion(bool enable) override;
        void SetScissorRegion(Rml::Rectanglei region) override;
        void SetTransform(const Rml::Matrix4f *transform) override;
        Rml::CompiledShaderHandle CompileShader(const Rml::String &name, const Rml::Dictionary &parameters) override;
        void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                          Rml::TextureHandle texture) override;
        void ReleaseShader(Rml::CompiledShaderHandle shader) override;

    private:
        struct Geometry;
        struct Gradient;
        struct Draw;
        struct Matrix;

        bool createPipelines(SDL_GPUTextureFormat targetFormat);
        void ensureFrameBuffers(uint32_t vertexBytes, uint32_t indexBytes);
        uint32_t currentMatrix();

        SDL_GPUDevice *m_device = nullptr;
        SDL_GPUGraphicsPipeline *m_colourPipeline = nullptr;
        SDL_GPUGraphicsPipeline *m_texturePipeline = nullptr;
        SDL_GPUGraphicsPipeline *m_gradientPipeline = nullptr;
        SDL_GPUSampler *m_sampler = nullptr;

        // The per-frame geometry store: one GPU vertex buffer and one index buffer, refilled every frame through a
        // transfer buffer (upload with cycle = true, so a frame still being drawn by the GPU is not overwritten).
        SDL_GPUBuffer *m_vertexBuffer = nullptr;
        SDL_GPUBuffer *m_indexBuffer = nullptr;
        SDL_GPUTransferBuffer *m_transferBuffer = nullptr;
        uint32_t m_vertexCapacity = 0;
        uint32_t m_indexCapacity = 0;
        uint32_t m_transferCapacity = 0;

        // Frame state
        bool m_inFrame = false;
        SDL_GPUCommandBuffer *m_cmd = nullptr;
        SDL_GPUTexture *m_target = nullptr;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        bool m_clear = false;
        SDL_FColor m_clearColour{};
        std::vector<Draw> m_draws;
        std::vector<Matrix> m_matrices;
        std::vector<Gradient> m_gradientUniforms; // one per gradient draw this frame
        std::unordered_map<SDL_GPUTexture *, uint32_t> m_textureSizes; // bytes per live texture, for the memory counter
        std::vector<std::unique_ptr<Geometry>> m_graveyard; // geometries released while a frame was open
        uint32_t m_frameStamp = 0;

        // Render state set by RmlUi between draws
        bool m_scissorOn = false;
        Rml::Rectanglei m_scissor;
        bool m_hasTransform = false;
        Rml::Matrix4f m_userTransform;
        Rml::Matrix4f m_projection;
        int64_t m_matrixIndex = -1; // index into m_matrices of the current matrix, -1 when it has to be added

        FrameStats m_stats;
        size_t m_textureCount = 0;
        size_t m_textureBytes = 0;
        size_t m_geometryCount = 0;
    };
}
