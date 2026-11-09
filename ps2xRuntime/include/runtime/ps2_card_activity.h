#pragma once

// What the memory-card stubs tell the host about writes (for the settings overlay's Quit Game button).
//
// The game writes a save as a few files, each one an open, some writes and a close (Kernel/Stubs/MemoryCard.cpp).
// A file opened for writing is a working copy (<name>.mctmp) that only replaces the real file when the game closes it,
// so stopping the game at any moment never damages a save that is already on the card. What it can do is lose the save
// that was being written. The host cannot let the game finish while the overlay is open (the game is paused), so it
// asks here whether a write is under way, or has just been, and keeps Quit Game greyed while the answer is yes.
//
// Header-only, no SDL and no game code, like ps2_pause.h: the stubs (game thread) and the host (main thread) both
// include it.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ps2x_host
{
    struct CardActivityState
    {
        std::atomic<uint32_t> writeHandlesOpen{0}; // files open for writing now: their working copies are not yet swapped in
        std::atomic<uint64_t> lastWriteNs{0};      // steady clock (ns since its epoch) at the last command that changed a card; 0 = none yet
        std::atomic<uint64_t> writeCommands{0};    // how many such commands there have been (for tests)
    };

    inline CardActivityState &cardActivityState()
    {
        static CardActivityState state;
        return state;
    }

    inline uint64_t cardSteadyNowNs() noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // Stubs: a command that changes a card has just run (open for writing, a write, closing a file that was open for
    // writing, mkdir, delete, rename, format).
    inline void cardNoteWrite() noexcept
    {
        CardActivityState &state = cardActivityState();
        state.lastWriteNs.store(cardSteadyNowNs(), std::memory_order_release);
        state.writeCommands.fetch_add(1, std::memory_order_acq_rel);
    }

    // Stubs: the number of files now open for writing, after every change to the list of open files.
    inline void cardSetWriteHandlesOpen(uint32_t count) noexcept
    {
        cardActivityState().writeHandlesOpen.store(count, std::memory_order_release);
    }

    struct CardWriteStatus
    {
        uint32_t writeHandlesOpen = 0;
        bool anyWriteYet = false;
        uint64_t msSinceLastWrite = 0; // meaningful when anyWriteYet
    };

    // A card write younger than this keeps Quit Game dimmed: one save is three files written within about 17 ms, with gaps
    // of a few ms between them in which no file is open.
    constexpr uint64_t kCardQuietMs = 500;

    // Quit Game's rule: "" when the game may be stopped now, else the sentence for the help line (one line high, so short).
    // `status` must be read while the game thread is standing still (guestBlocked() in ps2_pause.h), or it can change under it.
    inline const char *cardQuitBlockReason(const CardWriteStatus &status) noexcept
    {
        if (status.writeHandlesOpen > 0)
            return "The game is saving. Close this menu, let it finish, then quit.";
        if (status.anyWriteYet && status.msSinceLastWrite < kCardQuietMs)
            return "The game has just saved. Close this menu, wait a moment, then quit.";
        return "";
    }

    // Host: any thread.
    inline CardWriteStatus cardWriteStatus() noexcept
    {
        CardActivityState &state = cardActivityState();
        CardWriteStatus status;
        status.writeHandlesOpen = state.writeHandlesOpen.load(std::memory_order_acquire);
        const uint64_t last = state.lastWriteNs.load(std::memory_order_acquire);
        if (last != 0)
        {
            const uint64_t now = cardSteadyNowNs();
            status.anyWriteYet = true;
            status.msSinceLastWrite = now > last ? (now - last) / 1'000'000ull : 0;
        }
        return status;
    }
}
