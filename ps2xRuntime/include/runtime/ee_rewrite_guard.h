#pragma once

// The rewrite guard (task e1g, 2026-11-21; study: baserom/out/tmp/d1_scheduler.md; Seth's rule: CLAUDE.md "Hand-written rewrites
// never sit above a wait").
//
// WHAT IT IS FOR
// The game runs as machine-translated code that can be abandoned at any call and resumed later from the function table, because
// everything it needs is in the guest registers and guest memory. A hand-written C++ rewrite keeps state on the host stack, so it
// cannot be resumed: if a kernel call under it parks or switches the thread (EeDispatcherTransfer, thrown by EeScheduler.cpp), the
// rewrite's frame is destroyed and the thread later resumes the ORIGINAL function in the middle, with a stack frame it never made.
// Nothing noticed that before. The rule is that a rewrite never calls game code that can wait; this guard enforces it.
//
// HOW IT WORKS
//   - A rewrite raises a depth counter while it runs (Scope below; fpwr_decomp.cpp does it around every bound rewrite). With
//     PS2X_DECOMP off nothing is bound, the counter is never raised and the scheduler behaves exactly as before.
//   - While the counter is not zero:
//       * a transfer that blocks (blockCurrent: WaitSema, SleepThread, WaitEventFlag, the frame wait, disc and MPEG waits;
//         invokeCurrent and invokeCurrentSequence: an RPC with a callback, a syscall override, an exit handler; exitCurrent:
//         ExitThread) is not carried out: the game is stopped with a message that names the kernel call, the guest pc and the
//         rewrites on the host stack, and the process exits with kExitCode;
//       * the pre-emption transfer in transferIfRequested (a kernel call woke a higher-priority thread, or the time slice ran
//         out) is deferred, the way the interrupt-safe forms already defer: the checkpoint flag is raised and the thread is
//         switched at the first checkpoint after the rewrite has returned to translated code.
//   - Every such request is counted per site. With PS2X_DECOMP_STATS=1 the counts are printed when the game ends (and the first
//     few deferred pre-emptions as they happen), followed by what the rewrite host adds (calls per rewrite) and a census of the
//     guest threads and semaphores (ids, priorities, states). A stop prints the same block before the process exits.
//
// This header is new and nothing the generated game code includes depends on it. The state lives in EeScheduler.cpp.
#include <cstdint>
#include <cstdio>

namespace ps2x_rewrite_guard
{
    // The process exit code when the guard stops the game (sysexits.h EX_SOFTWARE).
    constexpr int kExitCode = 70;

    // The places in EeScheduler.cpp where a thread gives way.
    enum class Site : uint8_t
    {
        BlockCurrent,          // blockCurrent: WaitSema, SleepThread, WaitEventFlag, vertical-blank, disc and MPEG waits. STOPS the game
        InvokeCurrent,         // invokeCurrent: a guest callback or handler is pushed on the thread. STOPS the game
        InvokeCurrentSequence, // invokeCurrentSequence: several of them. STOPS the game
        ExitCurrent,           // exitCurrent: ExitThread, ExitDeleteThread. STOPS the game
        Preemption,            // transferIfRequested: a pre-emption was due. DEFERRED, not stopped
        Count
    };
    constexpr int kSiteCount = static_cast<int>(Site::Count);

    // A rewrite is running at `address` (the game address it stands in for). `name` is a string literal for messages, or null.
    void enter(uint32_t address, const char *name = nullptr) noexcept;
    void leave() noexcept;

    // True while a rewrite is on the calling thread's host stack.
    bool active() noexcept;
    uint32_t depth() noexcept;

    // Raises the counter for the lifetime of the object; safe against exceptions.
    class Scope
    {
    public:
        explicit Scope(uint32_t address, const char *name = nullptr) noexcept { enter(address, name); }
        ~Scope() { leave(); }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
    };

    struct Counts
    {
        uint64_t requested[kSiteCount] = {}; // transfers requested while a rewrite was on the stack, by site
        uint64_t entries = 0;                // rewrite calls in all
        uint32_t maxDepth = 0;               // deepest nesting of rewrites
    };
    Counts counts() noexcept;
    void resetCounts() noexcept;
    const char *siteName(Site site) noexcept;

    // Printed when the stats are printed (PS2X_DECOMP_STATS=1), after the guard's own table: lets fpwr_decomp.cpp add what it knows
    // (calls per rewrite). Null for none.
    void setStatsHook(void (*hook)(std::FILE *out)) noexcept;

    // What happens when a blocking transfer is requested under a rewrite. The default prints the message to stdout and stderr and
    // calls std::_Exit(kExitCode). A test may install a handler that throws; if a handler returns, the transfer goes on as it
    // would have without the guard.
    using StopHandler = void (*)(const char *message);
    void setStopHandler(StopHandler handler) noexcept;
}
