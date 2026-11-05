#pragma once

// Two places where SDL 3.4.18's SDL_GPU is not safe for the two threads that use the device in the game (the GS
// thread records the draws and the field copies; the main thread records the picture and the window pass).
// Both guards do nothing, and cost one relaxed atomic load, unless the host turns them on.
//
// 1. DebugPassLock (validation, PS2X_GPU_DEBUG=1).
//    SDL's debug layer checks every scissor and viewport against "the size of the render target", but it keeps
//    that size in the device, not in the command buffer. Each BeginGPURenderPass overwrites it, so two threads
//    recording render passes make each other's correct scissor fail with "Scissor rectangle size exceeds
//    current render target dimensions" (the window pass is 640 x 480, the GS targets have 512 rows). With
//    validation on, every render pass is recorded under one mutex.
//
// 2. DefragGuard (the Vulkan driver; on by default there, PS2X_GPU_DEFRAG_GUARD=0 turns it off).
//    The Vulkan driver moves textures to other memory blocks ("defragmentation") inside a submit, when texture
//    creation has left memory fragmented. It does so without regard to the other thread:
//      - SDL_ReleaseGPUTexture / SDL_ReleaseGPUBuffer free the client handle at once under a lock (disposeLock)
//        the defrag code does not take, and the defrag code reads that handle: a release on one thread during
//        the other thread's defragmenting submit reads and writes freed memory. Seen with the Vulkan validation
//        layer as vkCmdCopyImage calls with garbage mip level, layer and extent (SDL's defrag code is the only
//        other place that calls vkCmdCopyImage), followed by VK_ERROR_DEVICE_LOST.
//      - A command buffer another thread has recorded but not yet submitted may use a texture the defrag moves:
//        the defrag leaves the old image in the TRANSFER_SRC layout and the later submit finds it there (the
//        validation layer reports "expects ... SHADER_READ_ONLY_OPTIMAL ... current layout is TRANSFER_SRC_OPTIMAL",
//        also for the depth targets), and anything such a buffer draws into a moved target is lost.
//    So on Vulkan the two threads never overlap while one has a command buffer in the making or is submitting:
//    a DefragGuard is held from SDL_AcquireGPUCommandBuffer to the end of the SDL_Submit* call, and around every
//    SDL_ReleaseGPU* of a texture or buffer. It is a recursive mutex, so a release inside such a window is fine.
//    The GS thread's window is its flush (recording the draws, about a millisecond or two), the main thread's is
//    its present (recording the picture and the window pass, under half a millisecond); neither waits for the other
//    for longer than that, and nothing waits for the GPU inside a window except the swapchain acquire.
//
// Code inside a DebugPassLock must not take other locks. Lock order: DefragGuard first, then DebugPassLock.

#include <atomic>
#include <mutex>

namespace ps2x_gs
{
    inline std::atomic<bool> &debugPassSerialization()
    {
        static std::atomic<bool> on{false};
        return on;
    }

    inline std::atomic<bool> &defragGuardEnabled()
    {
        static std::atomic<bool> on{false};
        return on;
    }

    inline std::mutex &debugPassMutex()
    {
        static std::mutex m;
        return m;
    }

    inline std::recursive_mutex &defragMutex()
    {
        static std::recursive_mutex m;
        return m;
    }

    // Holds `mutex` while `enabled` is set. Unlocks at the end of its scope, or at Unlock().
    template <class MutexType, std::atomic<bool> &(*Enabled)(), MutexType &(*Mutex)()>
    class ConditionalLock
    {
    public:
        ConditionalLock()
        {
            if (Enabled().load(std::memory_order_relaxed))
                m_lock = std::unique_lock<MutexType>(Mutex());
        }
        void Unlock()
        {
            if (m_lock.owns_lock())
                m_lock.unlock();
        }

    private:
        std::unique_lock<MutexType> m_lock;
    };

    // Put it before SDL_BeginGPURenderPass.
    using DebugPassLock = ConditionalLock<std::mutex, debugPassSerialization, debugPassMutex>;
    // Put it before SDL_AcquireGPUCommandBuffer and call Unlock() after SDL_Submit*; also around SDL_ReleaseGPU*
    // (texture, buffer, transfer buffer).
    using DefragGuard = ConditionalLock<std::recursive_mutex, defragGuardEnabled, defragMutex>;
} // namespace ps2x_gs
