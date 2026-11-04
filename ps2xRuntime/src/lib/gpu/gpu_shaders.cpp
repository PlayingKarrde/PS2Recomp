#include "gpu_shaders.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_log.h>

namespace ps2x::gpu
{
    SDL_GPUShader *createShader(SDL_GPUDevice *device, const CompiledShader &shader, const char **formatName)
    {
        if (!device)
        {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "createShader(%s): no GPU device", shader.source);
            return nullptr;
        }

        const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(device);

        const ShaderBlob *blob = nullptr;
        SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_INVALID;
        const char *name = nullptr;
        const char *entry = "main"; // SPIR-V and DXIL were compiled with entry point "main"
        if ((supported & SDL_GPU_SHADERFORMAT_SPIRV) && shader.spirv.size != 0)
        {
            blob = &shader.spirv;
            format = SDL_GPU_SHADERFORMAT_SPIRV;
            name = "SPIR-V";
        }
        else if ((supported & SDL_GPU_SHADERFORMAT_DXIL) && shader.dxil.size != 0)
        {
            blob = &shader.dxil;
            format = SDL_GPU_SHADERFORMAT_DXIL;
            name = "DXIL";
        }
        else if ((supported & SDL_GPU_SHADERFORMAT_MSL) && shader.msl.size != 0)
        {
            blob = &shader.msl;
            format = SDL_GPU_SHADERFORMAT_MSL;
            name = "MSL";
            entry = shader.mslEntry; // spirv-cross renamed "main"
        }
        else
        {
            SDL_LogError(SDL_LOG_CATEGORY_GPU,
                         "createShader(%s): the device accepts shader formats 0x%x, but the compiled shader only has SPIR-V, DXIL and MSL",
                         shader.source, static_cast<unsigned>(supported));
            return nullptr;
        }

        SDL_GPUShaderCreateInfo info = {};
        info.code_size = blob->size;
        info.code = blob->data;
        info.entrypoint = entry;
        info.format = format;
        info.stage = (shader.stage == ShaderStage::Vertex) ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
        info.num_samplers = shader.numSamplers;
        info.num_storage_textures = shader.numStorageTextures;
        info.num_storage_buffers = shader.numStorageBuffers;
        info.num_uniform_buffers = shader.numUniformBuffers;
        info.props = 0;

        SDL_GPUShader *created = SDL_CreateGPUShader(device, &info);
        if (!created)
        {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "createShader(%s): SDL_CreateGPUShader failed with %s: %s",
                         shader.source, name, SDL_GetError());
            return nullptr;
        }
        if (formatName)
            *formatName = name;
        return created;
    }
}
