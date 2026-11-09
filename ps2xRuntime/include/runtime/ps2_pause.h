#pragma once

// Pausing the guest from the host (the UI overlay's pause).
//
// While the settings overlay is open the game must not move on: the guest (the PS2's CPU, which runs on the game
// thread) stops at its next vertical blank, the sound goes quiet, and the host keeps showing the last picture. This
// header is the whole interface between the two sides. It has no SDL and no game code in it, so the scheduler (which
// is part of the runtime, not generated code) and the host can both include it.
//
//   main thread:  setGuestPaused(true) / setGuestPaused(false)
//   game thread:  EeScheduler calls waitWhileGuestPaused() when a vertical blank is due; it returns at once when not
//                 paused, else blocks until the pause ends (or the game is stopping) and returns how long it waited.
//   audio thread: guestPaused() decides whether the sound chip is mixed or silence is played.
//
// Time that passes while paused must not count as game time, or the guest would run a burst of catch-up frames when
// it resumes. The scheduler moves its host-time deadlines forward by the blocked time; guestPausedTotalNs() is the
// running total, for the clock that keeps guest time from falling behind host time.
//
// Everything is header-only (inline variables), so a build that never pauses pays for one atomic read.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace ps2x_host
{
    struct GuestPauseState
    {
        std::atomic<bool> paused{false};
        std::atomic<uint64_t> totalPausedNs{0}; // time the game thread has spent blocked in waitWhileGuestPaused
        std::atomic<uint64_t> blockCount{0};
        std::mutex mutex;
        std::condition_variable wake;
    };

    inline GuestPauseState &guestPauseState()
    {
        static GuestPauseState state;
        return state;
    }

    inline bool guestPaused() noexcept
    {
        return guestPauseState().paused.load(std::memory_order_acquire);
    }

    inline void setGuestPaused(bool paused)
    {
        GuestPauseState &state = guestPauseState();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.paused.store(paused, std::memory_order_release);
        }
        state.wake.notify_all();
    }

    inline uint64_t guestPausedTotalNs() noexcept
    {
        return guestPauseState().totalPausedNs.load(std::memory_order_acquire);
    }

    inline uint64_t guestPauseBlockCount() noexcept
    {
        return guestPauseState().blockCount.load(std::memory_order_acquire);
    }

    // Game thread. Blocks while the guest is paused; `stopping` is polled about every 20 ms so a quit request is
    // not stuck behind a pause. Returns the nanoseconds spent blocked (0 when it did not block).
    template <typename StopFn>
    inline uint64_t waitWhileGuestPaused(StopFn &&stopping)
    {
        GuestPauseState &state = guestPauseState();
        if (!state.paused.load(std::memory_order_acquire))
            return 0;
        const auto start = std::chrono::steady_clock::now();
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            while (state.paused.load(std::memory_order_acquire) && !stopping())
                state.wake.wait_for(lock, std::chrono::milliseconds(20));
        }
        const auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
        const uint64_t ns = static_cast<uint64_t>(waited.count());
        state.totalPausedNs.fetch_add(ns, std::memory_order_acq_rel);
        state.blockCount.fetch_add(1, std::memory_order_acq_rel);
        return ns;
    }
}
