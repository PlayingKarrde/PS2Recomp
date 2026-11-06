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
//
// What the guard costs, and where it is held (task g5):
//    SDL 3.4.18 runs the defragmentation (and the other clean-ups) only inside the submit of a command buffer that
//    asked for a swapchain texture (SDL_gpu_vulkan.c, VULKAN_Submit: performCleanups = swapchainRequested), so it is
//    the host's window command buffer that defragments, never the GS thread's. The host used to take the guard before
//    acquiring that command buffer and keep it across SDL_WaitAndAcquireGPUSwapchainTexture, which blocks until the
//    display has freed an image: on a driver that offers only FIFO/vsync that is up to a frame, and the GS thread,
//    waiting for the guard to record its next flush, waited with it. The window command buffer now acquires the
//    swapchain texture first, outside the guard, and takes the guard (Lock()) only once it is about to record
//    commands that use textures. Nothing it did between acquiring the command buffer and taking the guard touches a
//    texture or buffer that the other thread could release or the defragmentation could move, so the reasons above
//    still hold. PS2X_GPU_ACQUIRE_FIRST=0 restores the old order, for comparing. PS2X_GPU_GUARD_STATS=1 prints, once
//    a second, how long each thread waited for the guard and held it (the host prints the line).

#include <atomic>
#include <chrono>
#include <cstdint>
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

    // ---- opt-in measurement of the guard (PS2X_GPU_GUARD_STATS=1; the host switches it on and prints it)
    // Two kinds of thread take the guard: the GS thread (flushes, releases) and the host's main thread (the window
    // command buffer). The thread that creates the presenter marks itself as the main thread.
    inline std::atomic<bool> &defragGuardStatsEnabled()
    {
        static std::atomic<bool> on{false};
        return on;
    }
    inline bool &guardThreadIsMain() // thread_local: set once on the host's main thread
    {
        static thread_local bool main = false;
        return main;
    }
    struct GuardRoleStats
    {
        std::atomic<uint64_t> acquisitions{0}; // outermost acquisitions (a recursive one inside a window is not counted)
        std::atomic<uint64_t> waits{0};        // ... that had to wait for the other thread
        std::atomic<uint64_t> waitNs{0};
        std::atomic<uint64_t> waitMaxNs{0};
        std::atomic<uint64_t> holdNs{0};
        std::atomic<uint64_t> holdMaxNs{0};
    };
    inline GuardRoleStats &guardStats(bool mainThread)
    {
        static GuardRoleStats gs, main;
        return mainThread ? main : gs;
    }
    inline uint64_t guardNowNs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    inline void guardMax(std::atomic<uint64_t> &slot, uint64_t value)
    {
        uint64_t seen = slot.load(std::memory_order_relaxed);
        while (value > seen && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed))
        {
        }
    }

    // The Vulkan defragmentation guard: a recursive mutex held from before the first command that uses a texture
    // or buffer to the end of the submit, and around every release. `lockNow = false` makes a guard that is not
    // held yet: the host acquires its command buffer and the swapchain texture first and calls Lock() afterwards.
    // Does nothing unless the host turned it on (the Vulkan driver). Unlocks at the end of its scope, or at Unlock().
    class DefragGuard
    {
    public:
        explicit DefragGuard(bool lockNow = true)
        {
            if (lockNow)
                Lock();
        }
        ~DefragGuard() { Unlock(); }
        DefragGuard(const DefragGuard &) = delete;
        DefragGuard &operator=(const DefragGuard &) = delete;

        void Lock()
        {
            if (m_held || !defragGuardEnabled().load(std::memory_order_relaxed))
                return;
            std::recursive_mutex &mutex = defragMutex();
            if (!defragGuardStatsEnabled().load(std::memory_order_relaxed))
            {
                mutex.lock();
                m_held = true;
                return;
            }
            // Measuring: note how long this acquisition waited, and (outermost only) how long it holds the guard.
            m_main = guardThreadIsMain();
            GuardRoleStats &s = guardStats(m_main);
            const uint64_t t0 = guardNowNs();
            const bool free = mutex.try_lock();
            if (!free)
                mutex.lock();
            const uint64_t t1 = guardNowNs();
            m_held = true;
            m_outermost = depthRef()++ == 0;
            m_inDepth = true;
            if (m_outermost)
            {
                s.acquisitions.fetch_add(1, std::memory_order_relaxed);
                if (!free)
                {
                    s.waits.fetch_add(1, std::memory_order_relaxed);
                    s.waitNs.fetch_add(t1 - t0, std::memory_order_relaxed);
                    guardMax(s.waitMaxNs, t1 - t0);
                }
                m_acquiredNs = t1;
            }
        }
        void Unlock()
        {
            if (!m_held)
                return;
            if (m_inDepth)
            {
                --depthRef();
                if (m_outermost)
                {
                    GuardRoleStats &s = guardStats(m_main);
                    const uint64_t held = guardNowNs() - m_acquiredNs;
                    s.holdNs.fetch_add(held, std::memory_order_relaxed);
                    guardMax(s.holdMaxNs, held);
                }
            }
            defragMutex().unlock();
            m_held = m_inDepth = m_outermost = false;
        }

    private:
        static int &depthRef()
        {
            static thread_local int depth = 0;
            return depth;
        }
        bool m_held = false, m_main = false, m_outermost = false, m_inDepth = false;
        uint64_t m_acquiredNs = 0;
    };
} // namespace ps2x_gs
