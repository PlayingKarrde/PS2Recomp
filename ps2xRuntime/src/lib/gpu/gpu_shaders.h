// Loader for the precompiled shaders in ps2xRuntime/shaders/compiled/ (made by build_shaders.py).
//
// SDL_GPU wants a shader in the format of the driver it runs on: SPIR-V for Vulkan, DXIL for
// Direct3D 12, MSL source for Metal. Each compiled header carries all three in a CompiledShader.
// createShader() asks the device which formats it accepts (SDL_GetGPUShaderFormats), takes the first
// one it can use, and fills in SDL_GPUShaderCreateInfo, including the MSL entry-point name and the
// resource counts SDL cannot read from the bytecode.
//
// This header does not include SDL, so the generated headers that include it stay light.
// It is not included by anything the generated game code uses.
#pragma once

#include <cstddef>
#include <cstdint>

struct SDL_GPUDevice;
struct SDL_GPUShader;

namespace ps2x::gpu
{
    enum class ShaderStage : uint8_t
    {
        Vertex,
        Fragment,
    };

    struct ShaderBlob
    {
        const unsigned char *data;
        size_t size;
    };

    // The layout of this struct is written out by build_shaders.py; change both together.
    struct CompiledShader
    {
        const char *source; // e.g. "smoke.vert.hlsl", for log messages
        ShaderStage stage;
        ShaderBlob spirv;
        ShaderBlob dxil;
        ShaderBlob msl; // MSL source text, not NUL-terminated
        const char *mslEntry;
        uint32_t numSamplers;
        uint32_t numStorageTextures;
        uint32_t numStorageBuffers;
        uint32_t numUniformBuffers;
    };

    // Creates the shader in the first format the device accepts (SPIR-V, then DXIL, then MSL).
    // Returns nullptr and logs the reason (through SDL_Log) when no format fits or SDL refuses it.
    // The caller releases it with SDL_ReleaseGPUShader. When formatName is given it receives "SPIR-V",
    // "DXIL" or "MSL" (a string literal) for the format that was used.
    SDL_GPUShader *createShader(SDL_GPUDevice *device, const CompiledShader &shader, const char **formatName = nullptr);
}
