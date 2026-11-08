#include "ui_render_sdlgpu.h"

#include "gpu/gpu_shaders.h"
#include "shaders/compiled/ui_color_frag.h"
#include "shaders/compiled/ui_gradient_frag.h"
#include "shaders/compiled/ui_texture_frag.h"
#include "shaders/compiled/ui_vert.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Math.h>
#include <RmlUi/Core/Vertex.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ps2x::ui
{
    namespace
    {
        // Vertex layout the pipelines read: Rml::Vertex is {float2 position, RGBA8 premultiplied colour, float2 uv}.
        static_assert(sizeof(Rml::Vertex) == 20, "ui.vert.hlsl and the vertex attributes below assume a 20 byte Rml::Vertex");
        static_assert(sizeof(int) == 4, "RmlUi indices are drawn as 32-bit");

        // The vertex shader's uniform block (ui.vert.hlsl, "Place"): column-major matrix, then the translation.
        struct VertexUniform
        {
            float matrix[16];
            float translate[4];
        };

        // The gradient shader's uniform block (ui_gradient.frag.hlsl, "Gradient"). Everything is float4-sized.
        constexpr int kMaxStops = 16;
        struct GradientUniform
        {
            int32_t params[4]; // function, number of stops
            float pv[4];       // p.xy, v.zw
            float colours[kMaxStops][4];
            float positions[kMaxStops]; // read as StopPositions[4] (float4 each)
        };
        static_assert(sizeof(GradientUniform) == 16 + 16 + 16 * kMaxStops + 4 * kMaxStops, "GradientUniform layout");

        // Order must match the shader's defines (ui_gradient.frag.hlsl).
        enum GradientFunction : int32_t
        {
            kLinear = 0,
            kRadial = 1,
            kConic = 2,
            kRepeatingLinear = 3,
            kRepeatingRadial = 4,
            kRepeatingConic = 5,
        };

        constexpr float kDepthRange = 10000.0f; // RmlUi's own renderers use +-10000 for the orthographic depth range
    }

    struct SdlGpuRenderer::Geometry
    {
        // RmlUi guarantees these stay valid and unchanged until ReleaseGeometry, so they are not copied.
        Rml::Span<const Rml::Vertex> vertices;
        Rml::Span<const int> indices;
        uint32_t stamp = 0;      // the frame (m_frameStamp) whose buffers hold this geometry
        uint32_t baseVertex = 0; // first vertex in the frame's vertex buffer
        uint32_t firstIndex = 0; // first index in the frame's index buffer
    };

    struct SdlGpuRenderer::Gradient
    {
        GradientUniform uniform{};
    };

    struct SdlGpuRenderer::Matrix
    {
        float m[16];
    };

    struct SdlGpuRenderer::Draw
    {
        Geometry *geometry = nullptr;
        SDL_GPUTexture *texture = nullptr;
        float translate[2] = {0.0f, 0.0f};
        bool scissorOn = false;
        SDL_Rect scissor{};
        uint32_t matrix = 0;
        int32_t gradient = -1; // index into the frame's gradient uniforms, -1 for a plain draw
    };

    SdlGpuRenderer::SdlGpuRenderer() = default;

    SdlGpuRenderer::~SdlGpuRenderer()
    {
        shutdown();
    }

    bool SdlGpuRenderer::init(SDL_GPUDevice *device, SDL_GPUTextureFormat targetFormat)
    {
        shutdown();
        if (!device)
            return false;
        m_device = device;
        if (!createPipelines(targetFormat))
        {
            shutdown();
            return false;
        }

        SDL_GPUSamplerCreateInfo sampler = {};
        sampler.min_filter = SDL_GPU_FILTER_LINEAR;
        sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
        sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        // RmlUi tiles decorator images by running texture coordinates past 1.
        sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        m_sampler = SDL_CreateGPUSampler(m_device, &sampler);
        if (!m_sampler)
        {
            Rml::Log::Message(Rml::Log::LT_ERROR, "UI renderer: SDL_CreateGPUSampler failed: %s", SDL_GetError());
            shutdown();
            return false;
        }
        return true;
    }

    void SdlGpuRenderer::shutdown()
    {
        if (!m_device)
            return;
        m_graveyard.clear();
        m_draws.clear();
        m_inFrame = false;
        if (m_transferBuffer)
            SDL_ReleaseGPUTransferBuffer(m_device, m_transferBuffer);
        if (m_vertexBuffer)
            SDL_ReleaseGPUBuffer(m_device, m_vertexBuffer);
        if (m_indexBuffer)
            SDL_ReleaseGPUBuffer(m_device, m_indexBuffer);
        if (m_sampler)
            SDL_ReleaseGPUSampler(m_device, m_sampler);
        if (m_colourPipeline)
            SDL_ReleaseGPUGraphicsPipeline(m_device, m_colourPipeline);
        if (m_texturePipeline)
            SDL_ReleaseGPUGraphicsPipeline(m_device, m_texturePipeline);
        if (m_gradientPipeline)
            SDL_ReleaseGPUGraphicsPipeline(m_device, m_gradientPipeline);
        m_transferBuffer = nullptr;
        m_vertexBuffer = m_indexBuffer = nullptr;
        m_sampler = nullptr;
        m_colourPipeline = m_texturePipeline = m_gradientPipeline = nullptr;
        m_vertexCapacity = m_indexCapacity = m_transferCapacity = 0;
        m_device = nullptr;
    }

    bool SdlGpuRenderer::createPipelines(SDL_GPUTextureFormat targetFormat)
    {
        namespace shaders = ps2x::gpu::shaders;
        SDL_GPUShader *vert = ps2x::gpu::createShader(m_device, shaders::ui_vert);
        SDL_GPUShader *colour = ps2x::gpu::createShader(m_device, shaders::ui_color_frag);
        SDL_GPUShader *texture = ps2x::gpu::createShader(m_device, shaders::ui_texture_frag);
        SDL_GPUShader *gradient = ps2x::gpu::createShader(m_device, shaders::ui_gradient_frag);
        bool ok = vert && colour && texture && gradient;

        if (ok)
        {
            // Premultiplied alpha: the shaders output colour already multiplied by alpha, so the blend is
            // dst = src + dst * (1 - src.a) for colour and alpha alike.
            SDL_GPUColorTargetDescription target = {};
            target.format = targetFormat;
            target.blend_state.enable_blend = true;
            target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

            // The "location" is the number after TEXCOORD in ui.vert.hlsl's inputs (SDL_gpu.h lines 2754-2760).
            SDL_GPUVertexAttribute attributes[3] = {};
            attributes[0].location = 0;
            attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
            attributes[0].offset = static_cast<Uint32>(offsetof(Rml::Vertex, position));
            attributes[1].location = 1;
            attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
            attributes[1].offset = static_cast<Uint32>(offsetof(Rml::Vertex, colour));
            attributes[2].location = 2;
            attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
            attributes[2].offset = static_cast<Uint32>(offsetof(Rml::Vertex, tex_coord));

            SDL_GPUVertexBufferDescription buffer = {};
            buffer.slot = 0;
            buffer.pitch = sizeof(Rml::Vertex);
            buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

            SDL_GPUGraphicsPipelineCreateInfo info = {};
            info.vertex_shader = vert;
            info.vertex_input_state.vertex_buffer_descriptions = &buffer;
            info.vertex_input_state.num_vertex_buffers = 1;
            info.vertex_input_state.vertex_attributes = attributes;
            info.vertex_input_state.num_vertex_attributes = 3;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
            info.target_info.color_target_descriptions = &target;
            info.target_info.num_color_targets = 1;
            info.target_info.has_depth_stencil_target = false;

            info.fragment_shader = colour;
            m_colourPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
            info.fragment_shader = texture;
            m_texturePipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
            info.fragment_shader = gradient;
            m_gradientPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
            ok = m_colourPipeline && m_texturePipeline && m_gradientPipeline;
            if (!ok)
                Rml::Log::Message(Rml::Log::LT_ERROR, "UI renderer: SDL_CreateGPUGraphicsPipeline failed: %s", SDL_GetError());
        }

        if (vert)
            SDL_ReleaseGPUShader(m_device, vert);
        if (colour)
            SDL_ReleaseGPUShader(m_device, colour);
        if (texture)
            SDL_ReleaseGPUShader(m_device, texture);
        if (gradient)
            SDL_ReleaseGPUShader(m_device, gradient);
        return ok;
    }

    // ---------------------------------------------------------------------------------------------------------
    // Frames

    void SdlGpuRenderer::beginFrame(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, uint32_t width, uint32_t height,
                                    const SDL_FColor *clearColour)
    {
        if (!m_device || !cmd || !target || width == 0 || height == 0)
            return;
        m_inFrame = true;
        m_cmd = cmd;
        m_target = target;
        m_width = width;
        m_height = height;
        m_clear = clearColour != nullptr;
        if (clearColour)
            m_clearColour = *clearColour;
        m_draws.clear();
        m_matrices.clear();
        m_gradientUniforms.clear();

        // Window pixels (origin top-left, y down) to SDL_GPU clip space (x and y -1..1, y up; z 0..1). RmlUi's
        // ProjectOrtho gives a GL-style z of -1..1, and the clip volume here keeps only 0..1, so z is squeezed into
        // that half. Without it, a transform that pushes an element toward the viewer would be clipped away.
        m_projection = Rml::Matrix4f::Translate(0.0f, 0.0f, 0.5f) * Rml::Matrix4f::Scale(1.0f, 1.0f, 0.5f) *
                       Rml::Matrix4f::ProjectOrtho(0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f,
                                                   -kDepthRange, kDepthRange);
        m_hasTransform = false;
        m_matrixIndex = -1;
        m_scissorOn = false;
    }

    void SdlGpuRenderer::ensureFrameBuffers(uint32_t vertexBytes, uint32_t indexBytes)
    {
        auto grow = [](uint32_t have, uint32_t need) {
            uint32_t cap = std::max<uint32_t>(have, 64 * 1024);
            while (cap < need)
                cap *= 2;
            return cap;
        };
        if (!m_vertexBuffer || vertexBytes > m_vertexCapacity)
        {
            if (m_vertexBuffer)
                SDL_ReleaseGPUBuffer(m_device, m_vertexBuffer);
            m_vertexCapacity = grow(m_vertexCapacity, vertexBytes);
            SDL_GPUBufferCreateInfo info = {};
            info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
            info.size = m_vertexCapacity;
            m_vertexBuffer = SDL_CreateGPUBuffer(m_device, &info);
        }
        if (!m_indexBuffer || indexBytes > m_indexCapacity)
        {
            if (m_indexBuffer)
                SDL_ReleaseGPUBuffer(m_device, m_indexBuffer);
            m_indexCapacity = grow(m_indexCapacity, indexBytes);
            SDL_GPUBufferCreateInfo info = {};
            info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
            info.size = m_indexCapacity;
            m_indexBuffer = SDL_CreateGPUBuffer(m_device, &info);
        }
        const uint32_t transferNeed = vertexBytes + indexBytes;
        if (!m_transferBuffer || transferNeed > m_transferCapacity)
        {
            if (m_transferBuffer)
                SDL_ReleaseGPUTransferBuffer(m_device, m_transferBuffer);
            m_transferCapacity = grow(m_transferCapacity, transferNeed);
            SDL_GPUTransferBufferCreateInfo info = {};
            info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            info.size = m_transferCapacity;
            m_transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &info);
        }
    }

    void SdlGpuRenderer::endFrame()
    {
        if (!m_inFrame)
            return;
        m_inFrame = false;
        m_stats = FrameStats{};

        // Give every geometry used this frame a place in the shared vertex and index buffers.
        ++m_frameStamp;
        std::vector<Geometry *> used;
        uint32_t vertexBytes = 0;
        uint32_t indexBytes = 0;
        for (const Draw &draw : m_draws)
        {
            Geometry *g = draw.geometry;
            if (g->stamp == m_frameStamp)
                continue;
            g->stamp = m_frameStamp;
            g->baseVertex = vertexBytes / sizeof(Rml::Vertex);
            g->firstIndex = indexBytes / sizeof(int);
            vertexBytes += static_cast<uint32_t>(g->vertices.size() * sizeof(Rml::Vertex));
            indexBytes += static_cast<uint32_t>(g->indices.size() * sizeof(int));
            used.push_back(g);
        }

        bool uploaded = false;
        if (!used.empty())
        {
            ensureFrameBuffers(vertexBytes, indexBytes);
            if (m_vertexBuffer && m_indexBuffer && m_transferBuffer)
            {
                // cycle = true: if the GPU is still reading last frame's contents, SDL hands out fresh storage.
                auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(m_device, m_transferBuffer, true));
                if (mapped)
                {
                    for (const Geometry *g : used)
                    {
                        std::memcpy(mapped + static_cast<size_t>(g->baseVertex) * sizeof(Rml::Vertex), g->vertices.data(),
                                    g->vertices.size() * sizeof(Rml::Vertex));
                        std::memcpy(mapped + vertexBytes + static_cast<size_t>(g->firstIndex) * sizeof(int), g->indices.data(),
                                    g->indices.size() * sizeof(int));
                    }
                    SDL_UnmapGPUTransferBuffer(m_device, m_transferBuffer);

                    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(m_cmd);
                    if (copy)
                    {
                        SDL_GPUTransferBufferLocation src = {m_transferBuffer, 0};
                        SDL_GPUBufferRegion dst = {m_vertexBuffer, 0, vertexBytes};
                        SDL_UploadToGPUBuffer(copy, &src, &dst, true);
                        src.offset = vertexBytes;
                        dst = {m_indexBuffer, 0, indexBytes};
                        SDL_UploadToGPUBuffer(copy, &src, &dst, true);
                        SDL_EndGPUCopyPass(copy);
                        uploaded = true;
                    }
                }
            }
            if (!uploaded)
                Rml::Log::Message(Rml::Log::LT_ERROR, "UI renderer: uploading geometry failed: %s", SDL_GetError());
        }

        if ((uploaded && !m_draws.empty()) || m_clear)
        {
            SDL_GPUColorTargetInfo colour = {};
            colour.texture = m_target;
            colour.load_op = m_clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            colour.store_op = SDL_GPU_STOREOP_STORE;
            colour.clear_color = m_clearColour;
            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(m_cmd, &colour, 1, nullptr);
            if (!pass)
            {
                Rml::Log::Message(Rml::Log::LT_ERROR, "UI renderer: SDL_BeginGPURenderPass failed: %s", SDL_GetError());
            }
            else
            {
                if (uploaded)
                {
                    SDL_GPUBufferBinding vb = {m_vertexBuffer, 0};
                    SDL_GPUBufferBinding ib = {m_indexBuffer, 0};
                    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
                    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);

                    SDL_GPUGraphicsPipeline *boundPipeline = nullptr;
                    SDL_GPUTexture *boundTexture = nullptr;
                    std::vector<SDL_GPUTexture *> textures;
                    for (const Draw &draw : m_draws)
                    {
                        const Geometry *g = draw.geometry;
                        if (g->indices.empty())
                            continue;

                        SDL_Rect scissor = {0, 0, static_cast<int>(m_width), static_cast<int>(m_height)};
                        if (draw.scissorOn)
                        {
                            // SDL_GPU (like Vulkan) wants the scissor inside the target.
                            const int x0 = std::clamp(draw.scissor.x, 0, static_cast<int>(m_width));
                            const int y0 = std::clamp(draw.scissor.y, 0, static_cast<int>(m_height));
                            const int x1 = std::clamp(draw.scissor.x + draw.scissor.w, 0, static_cast<int>(m_width));
                            const int y1 = std::clamp(draw.scissor.y + draw.scissor.h, 0, static_cast<int>(m_height));
                            scissor = {x0, y0, x1 - x0, y1 - y0};
                            if (scissor.w <= 0 || scissor.h <= 0)
                                continue;
                        }

                        SDL_GPUGraphicsPipeline *pipeline =
                            draw.gradient >= 0 ? m_gradientPipeline : (draw.texture ? m_texturePipeline : m_colourPipeline);
                        if (pipeline != boundPipeline)
                        {
                            SDL_BindGPUGraphicsPipeline(pass, pipeline);
                            boundPipeline = pipeline;
                        }
                        if (draw.texture && draw.gradient < 0 && draw.texture != boundTexture)
                        {
                            SDL_GPUTextureSamplerBinding binding = {draw.texture, m_sampler};
                            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
                            boundTexture = draw.texture;
                            if (std::find(textures.begin(), textures.end(), draw.texture) == textures.end())
                                textures.push_back(draw.texture);
                        }
                        SDL_SetGPUScissor(pass, &scissor);

                        VertexUniform vu;
                        std::memcpy(vu.matrix, m_matrices[draw.matrix].m, sizeof(vu.matrix));
                        vu.translate[0] = draw.translate[0];
                        vu.translate[1] = draw.translate[1];
                        vu.translate[2] = vu.translate[3] = 0.0f;
                        SDL_PushGPUVertexUniformData(m_cmd, 0, &vu, sizeof(vu));
                        if (draw.gradient >= 0)
                        {
                            const GradientUniform &gu = m_gradientUniforms[static_cast<size_t>(draw.gradient)].uniform;
                            SDL_PushGPUFragmentUniformData(m_cmd, 0, &gu, sizeof(gu));
                        }

                        SDL_DrawGPUIndexedPrimitives(pass, static_cast<Uint32>(g->indices.size()), 1, g->firstIndex,
                                                     static_cast<Sint32>(g->baseVertex), 0);
                        ++m_stats.draws;
                    }
                    m_stats.uniqueTextures = static_cast<uint32_t>(textures.size());
                }
                SDL_EndGPURenderPass(pass);
            }
        }

        m_stats.geometries = static_cast<uint32_t>(used.size());
        m_stats.vertices = vertexBytes / sizeof(Rml::Vertex);
        m_stats.indices = indexBytes / sizeof(int);
        m_draws.clear();
        m_graveyard.clear();
        m_cmd = nullptr;
        m_target = nullptr;
    }

    // ---------------------------------------------------------------------------------------------------------
    // Rml::RenderInterface: geometry

    Rml::CompiledGeometryHandle SdlGpuRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
    {
        auto *g = new Geometry();
        g->vertices = vertices;
        g->indices = indices;
        ++m_geometryCount;
        return reinterpret_cast<Rml::CompiledGeometryHandle>(g);
    }

    void SdlGpuRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
    {
        auto *g = reinterpret_cast<Geometry *>(geometry);
        if (!g)
            return;
        --m_geometryCount;
        if (m_inFrame)
            m_graveyard.emplace_back(g); // a draw in this frame's list may still point at it
        else
            delete g;
    }

    uint32_t SdlGpuRenderer::currentMatrix()
    {
        if (m_matrixIndex < 0)
        {
            Matrix m;
            const Rml::Matrix4f full = m_hasTransform ? Rml::Matrix4f(m_projection * m_userTransform) : m_projection;
            std::memcpy(m.m, full.data(), sizeof(m.m));
            m_matrices.push_back(m);
            m_matrixIndex = static_cast<int64_t>(m_matrices.size()) - 1;
        }
        return static_cast<uint32_t>(m_matrixIndex);
    }

    void SdlGpuRenderer::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture)
    {
        if (!m_inFrame || !geometry)
            return;
        Draw draw;
        draw.geometry = reinterpret_cast<Geometry *>(geometry);
        draw.texture = reinterpret_cast<SDL_GPUTexture *>(texture);
        draw.translate[0] = translation.x;
        draw.translate[1] = translation.y;
        draw.scissorOn = m_scissorOn;
        draw.scissor = {m_scissor.Left(), m_scissor.Top(), m_scissor.Width(), m_scissor.Height()};
        draw.matrix = currentMatrix();
        m_draws.push_back(draw);
    }

    void SdlGpuRenderer::EnableScissorRegion(bool enable)
    {
        m_scissorOn = enable;
    }

    void SdlGpuRenderer::SetScissorRegion(Rml::Rectanglei region)
    {
        m_scissor = region;
    }

    void SdlGpuRenderer::SetTransform(const Rml::Matrix4f *transform)
    {
        m_hasTransform = transform != nullptr;
        if (transform)
            m_userTransform = *transform;
        m_matrixIndex = -1;
    }

    // ---------------------------------------------------------------------------------------------------------
    // Textures

    Rml::TextureHandle SdlGpuRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions)
    {
        if (!m_device || dimensions.x <= 0 || dimensions.y <= 0)
            return 0;
        const uint32_t bytes = static_cast<uint32_t>(dimensions.x) * static_cast<uint32_t>(dimensions.y) * 4u;
        if (source.size() < bytes)
            return 0;

        SDL_GPUTextureCreateInfo textureInfo = {};
        textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
        textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; // RmlUi's bytes are R, G, B, premultiplied A
        textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        textureInfo.width = static_cast<Uint32>(dimensions.x);
        textureInfo.height = static_cast<Uint32>(dimensions.y);
        textureInfo.layer_count_or_depth = 1;
        textureInfo.num_levels = 1;
        SDL_GPUTexture *texture = SDL_CreateGPUTexture(m_device, &textureInfo);
        if (!texture)
        {
            Rml::Log::Message(Rml::Log::LT_ERROR, "UI renderer: SDL_CreateGPUTexture(%dx%d) failed: %s", dimensions.x, dimensions.y,
                              SDL_GetError());
            return 0;
        }

        SDL_GPUTransferBufferCreateInfo transferInfo = {};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = bytes;
        SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
        void *mapped = transfer ? SDL_MapGPUTransferBuffer(m_device, transfer, false) : nullptr;
        if (!mapped)
        {
            Rml::Log::Message(Rml::Log::LT_ERROR, "UI renderer: texture upload buffer failed: %s", SDL_GetError());
            if (transfer)
                SDL_ReleaseGPUTransferBuffer(m_device, transfer);
            SDL_ReleaseGPUTexture(m_device, texture);
            return 0;
        }
        std::memcpy(mapped, source.data(), bytes);
        SDL_UnmapGPUTransferBuffer(m_device, transfer);

        // Own command buffer: RmlUi creates textures at any time, including while a frame's list is being built.
        // It is submitted now, so it runs before the frame's own command buffer, which is submitted later.
        bool ok = false;
        if (SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device))
        {
            if (SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd))
            {
                SDL_GPUTextureTransferInfo src = {};
                src.transfer_buffer = transfer;
                src.pixels_per_row = static_cast<Uint32>(dimensions.x);
                src.rows_per_layer = static_cast<Uint32>(dimensions.y);
                SDL_GPUTextureRegion dst = {};
                dst.texture = texture;
                dst.w = static_cast<Uint32>(dimensions.x);
                dst.h = static_cast<Uint32>(dimensions.y);
                dst.d = 1;
                SDL_UploadToGPUTexture(copy, &src, &dst, false);
                SDL_EndGPUCopyPass(copy);
                ok = SDL_SubmitGPUCommandBuffer(cmd);
            }
            else
            {
                SDL_CancelGPUCommandBuffer(cmd);
            }
        }
        SDL_ReleaseGPUTransferBuffer(m_device, transfer);
        if (!ok)
        {
            Rml::Log::Message(Rml::Log::LT_ERROR, "UI renderer: texture upload failed: %s", SDL_GetError());
            SDL_ReleaseGPUTexture(m_device, texture);
            return 0;
        }
        ++m_textureCount;
        m_textureBytes += bytes;
        m_textureSizes[texture] = bytes;
        return reinterpret_cast<Rml::TextureHandle>(texture);
    }

    Rml::TextureHandle SdlGpuRenderer::LoadTexture(Rml::Vector2i &textureDimensions, const Rml::String &source)
    {
        // RmlUi has no image decoders; the renderer provides them. SDL 3.4 reads BMP and PNG itself (no SDL3_image).
        Rml::FileInterface *files = Rml::GetFileInterface();
        if (!files)
            return 0;
        Rml::FileHandle file = files->Open(source);
        if (!file)
        {
            Rml::Log::Message(Rml::Log::LT_WARNING, "UI renderer: cannot open image '%s'", source.c_str());
            return 0;
        }
        files->Seek(file, 0, SEEK_END);
        const size_t size = files->Tell(file);
        files->Seek(file, 0, SEEK_SET);
        std::vector<uint8_t> bytes(size);
        files->Read(bytes.data(), size, file);
        files->Close(file);

        SDL_IOStream *io = SDL_IOFromConstMem(bytes.data(), bytes.size());
        SDL_Surface *surface = io ? SDL_LoadSurface_IO(io, true) : nullptr;
        if (!surface)
        {
            Rml::Log::Message(Rml::Log::LT_WARNING, "UI renderer: cannot decode image '%s' (BMP and PNG only): %s", source.c_str(),
                              SDL_GetError());
            return 0;
        }
        SDL_Surface *rgba = surface;
        if (surface->format != SDL_PIXELFORMAT_RGBA32)
        {
            rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(surface);
            if (!rgba)
                return 0;
        }

        // Premultiply alpha, which the shaders and the blend expect, into a tightly packed copy.
        const int w = rgba->w;
        const int h = rgba->h;
        std::vector<Rml::byte> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        for (int y = 0; y < h; ++y)
        {
            const uint8_t *row = static_cast<const uint8_t *>(rgba->pixels) + static_cast<size_t>(y) * static_cast<size_t>(rgba->pitch);
            Rml::byte *out = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
            for (int x = 0; x < w; ++x)
            {
                const int a = row[x * 4 + 3];
                out[x * 4 + 0] = static_cast<Rml::byte>(row[x * 4 + 0] * a / 255);
                out[x * 4 + 1] = static_cast<Rml::byte>(row[x * 4 + 1] * a / 255);
                out[x * 4 + 2] = static_cast<Rml::byte>(row[x * 4 + 2] * a / 255);
                out[x * 4 + 3] = static_cast<Rml::byte>(a);
            }
        }
        SDL_DestroySurface(rgba);
        textureDimensions = {w, h};
        return GenerateTexture(Rml::Span<const Rml::byte>(pixels.data(), pixels.size()), textureDimensions);
    }

    void SdlGpuRenderer::ReleaseTexture(Rml::TextureHandle texture)
    {
        if (!texture || !m_device)
            return;
        // SDL keeps a texture alive while a command buffer that used it is unfinished, so releasing here is safe
        // even if this frame's list still names it.
        auto *gpuTexture = reinterpret_cast<SDL_GPUTexture *>(texture);
        SDL_ReleaseGPUTexture(m_device, gpuTexture);
        --m_textureCount;
        if (auto it = m_textureSizes.find(gpuTexture); it != m_textureSizes.end())
        {
            m_textureBytes -= it->second;
            m_textureSizes.erase(it);
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Gradient shaders

    Rml::CompiledShaderHandle SdlGpuRenderer::CompileShader(const Rml::String &name, const Rml::Dictionary &parameters)
    {
        const bool linear = name == "linear-gradient";
        const bool radial = name == "radial-gradient";
        const bool conic = name == "conic-gradient";
        if (!linear && !radial && !conic)
        {
            Rml::Log::Message(Rml::Log::LT_WARNING, "UI renderer: unsupported shader '%s'", name.c_str());
            return 0;
        }

        auto it = parameters.find("color_stop_list");
        if (it == parameters.end() || it->second.GetType() != Rml::Variant::COLORSTOPLIST)
            return 0;
        const Rml::ColorStopList &stops = it->second.GetReference<Rml::ColorStopList>();
        const int count = std::min(static_cast<int>(stops.size()), kMaxStops);
        if (count < 1)
            return 0;

        auto *g = new Gradient();
        GradientUniform &u = g->uniform;
        const bool repeating = Rml::Get(parameters, "repeating", false);
        Rml::Vector2f p(0.0f);
        Rml::Vector2f v(0.0f);
        int32_t function = kLinear;
        if (linear)
        {
            function = repeating ? kRepeatingLinear : kLinear;
            p = Rml::Get(parameters, "p0", Rml::Vector2f(0.0f));
            v = Rml::Get(parameters, "p1", Rml::Vector2f(0.0f)) - p;
        }
        else if (radial)
        {
            function = repeating ? kRepeatingRadial : kRadial;
            p = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
            v = Rml::Vector2f(1.0f) / Rml::Get(parameters, "radius", Rml::Vector2f(1.0f));
        }
        else
        {
            function = repeating ? kRepeatingConic : kConic;
            p = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
            const float angle = Rml::Get(parameters, "angle", 0.0f);
            v = {Rml::Math::Cos(angle), Rml::Math::Sin(angle)};
        }
        u.params[0] = function;
        u.params[1] = count;
        u.pv[0] = p.x;
        u.pv[1] = p.y;
        u.pv[2] = v.x;
        u.pv[3] = v.y;
        for (int i = 0; i < count; ++i)
        {
            const Rml::ColorStop &stop = stops[static_cast<size_t>(i)];
            for (int c = 0; c < 4; ++c)
                u.colours[i][c] = static_cast<float>(stop.color[static_cast<size_t>(c)]) / 255.0f;
            u.positions[i] = stop.position.number;
        }
        return reinterpret_cast<Rml::CompiledShaderHandle>(g);
    }

    void SdlGpuRenderer::RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                                      Rml::TextureHandle /*texture*/)
    {
        if (!m_inFrame || !shader || !geometry)
            return;
        Draw draw;
        draw.geometry = reinterpret_cast<Geometry *>(geometry);
        draw.translate[0] = translation.x;
        draw.translate[1] = translation.y;
        draw.scissorOn = m_scissorOn;
        draw.scissor = {m_scissor.Left(), m_scissor.Top(), m_scissor.Width(), m_scissor.Height()};
        draw.matrix = currentMatrix();
        m_gradientUniforms.push_back(*reinterpret_cast<Gradient *>(shader));
        draw.gradient = static_cast<int32_t>(m_gradientUniforms.size()) - 1;
        m_draws.push_back(draw);
    }

    void SdlGpuRenderer::ReleaseShader(Rml::CompiledShaderHandle shader)
    {
        delete reinterpret_cast<Gradient *>(shader);
    }
}
