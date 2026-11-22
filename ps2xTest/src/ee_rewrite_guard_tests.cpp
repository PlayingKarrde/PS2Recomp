// Unit tests of the rewrite guard (include/runtime/ee_rewrite_guard.h, EeScheduler.cpp). Task e1g; added to the test list by c2.
#include "MiniTest.h"
#include "ps2_runtime.h"
#include "runtime/ee_rewrite_guard.h"
#include "runtime/ee_scheduler.h"

#include <cstring>
#include <string>
#include <vector>

namespace
{
    struct GuardTrip
    {
    };
    std::string g_guardMessage;
    int g_guardTrips = 0;

    void guardStopHandler(const char *message)
    {
        g_guardMessage = message;
        ++g_guardTrips;
        throw GuardTrip{};
    }

    struct GuardEnv
    {
        std::vector<uint8_t> rdram;
        R5900Context ctx{};
        PS2Runtime runtime;
        GuardEnv() : rdram(PS2_RAM_SIZE, 0) { std::memset(&ctx, 0, sizeof(ctx)); }
        EeScheduler &start()
        {
            EeScheduler &ee = runtime.eeScheduler();
            ee.reset(rdram.data(), ctx);
            ee.bindMainContextForSyscall(ctx, rdram.data());
            ps2x_rewrite_guard::resetCounts();
            g_guardTrips = 0;
            g_guardMessage.clear();
            return ee;
        }
    };

    bool contains(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }

    // A higher-priority thread is made ready while the main thread runs: a pre-emption is requested.
    void requestPreemption(GuardEnv &env, EeScheduler &ee)
    {
        int old = 0;
        ee.changePriority(EeScheduler::kMainThreadId, 50, false, old);
        EeThreadCreateParams driver{};
        driver.entry = 0x00100000u;
        driver.stack = 0x1E000u;
        driver.stackSize = 0x1000u;
        driver.priority = 10;
        const int id = ee.createThread(driver);
        ee.startThread(id, 0u, env.ctx, false);
    }
}

void register_ee_rewrite_guard_tests()
{
    MiniTest::Case("EeRewriteGuard", [](TestCase &tc)
    {
        tc.Run("without a rewrite on the stack a blocking wait still transfers and the guard stays quiet", [](TestCase &t)
        {
            ps2x_rewrite_guard::setStopHandler(guardStopHandler);
            GuardEnv env;
            EeScheduler &ee = env.start();
            const int sema = ee.createSemaphore(0, 1, 0u, 0u);
            bool transferred = false;
            try
            {
                ee.waitSemaphore(sema);
            }
            catch (const EeDispatcherTransfer &)
            {
                transferred = true;
            }
            t.IsTrue(transferred, "WaitSema on an empty semaphore should throw EeDispatcherTransfer as before");
            t.Equals(g_guardTrips, 0, "the guard must not trip with no rewrite on the stack");
            t.IsTrue(!ps2x_rewrite_guard::active(), "the depth counter is zero");
        });

        tc.Run("WaitSema under nested rewrites stops the game and names the call, the guest pc and both rewrites", [](TestCase &t)
        {
            ps2x_rewrite_guard::setStopHandler(guardStopHandler);
            GuardEnv env;
            EeScheduler &ee = env.start();
            const int sema = ee.createSemaphore(0, 1, 0u, 0u);
            ee.thread(EeScheduler::kMainThreadId)->context.pc = 0x00196108u;
            bool tripped = false;
            try
            {
                ps2x_rewrite_guard::Scope outer(0x0016F140u, "applyMoveDamage");
                ps2x_rewrite_guard::Scope inner(0x00157640u, "crowdSound");
                t.Equals(ps2x_rewrite_guard::depth(), 2u, "two scopes raise the depth to 2");
                ee.waitSemaphore(sema);
            }
            catch (const GuardTrip &)
            {
                tripped = true;
            }
            t.IsTrue(tripped && g_guardTrips == 1, "the stop handler is called once");
            t.Equals(ps2x_rewrite_guard::depth(), 0u, "the scopes unwound with the exception");
            t.IsTrue(contains(g_guardMessage, "WaitSema on semaphore"), "the message names WaitSema");
            t.IsTrue(contains(g_guardMessage, "guest pc:   0x00196108"), "the message gives the guest pc");
            t.IsTrue(contains(g_guardMessage, "0x00157640 crowdSound <- 0x0016F140 applyMoveDamage"), "the message lists the rewrites, innermost first");
            t.Equals(ps2x_rewrite_guard::counts().requested[static_cast<int>(ps2x_rewrite_guard::Site::BlockCurrent)], 1ull, "one blockCurrent request counted");
        });

        tc.Run("SleepThread, ExitThread and a pushed callback under a rewrite stop the game too", [](TestCase &t)
        {
            ps2x_rewrite_guard::setStopHandler(guardStopHandler);
            {
                GuardEnv env;
                EeScheduler &ee = env.start();
                bool tripped = false;
                try
                {
                    ps2x_rewrite_guard::Scope s(0x00169BA0u, "tick");
                    ee.sleepCurrent();
                }
                catch (const GuardTrip &)
                {
                    tripped = true;
                }
                t.IsTrue(tripped && contains(g_guardMessage, "SleepThread"), "SleepThread trips the guard");
            }
            {
                GuardEnv env;
                EeScheduler &ee = env.start();
                bool tripped = false;
                try
                {
                    ps2x_rewrite_guard::Scope s(0x00169BA0u, "tick");
                    ee.exitCurrent(false);
                }
                catch (const GuardTrip &)
                {
                    tripped = true;
                }
                t.IsTrue(tripped && contains(g_guardMessage, "ExitThread"), "ExitThread trips the guard");
            }
            {
                GuardEnv env;
                EeScheduler &ee = env.start();
                bool tripped = false;
                try
                {
                    ps2x_rewrite_guard::Scope s(0x0016F140u, "applyMoveDamage");
                    GuestInvocation invocation;
                    invocation.kind = GuestInvocationKind::RpcCallback;
                    invocation.tag = 7;
                    invocation.context.pc = 0x00200000u;
                    ee.invokeCurrent(std::move(invocation));
                }
                catch (const GuardTrip &)
                {
                    tripped = true;
                }
                t.IsTrue(tripped && contains(g_guardMessage, "callback or handler"), "invokeCurrent trips the guard");
            }
        });

        tc.Run("a pre-emption that falls due under a rewrite is deferred to the next checkpoint, counted, not lost", [](TestCase &t)
        {
            ps2x_rewrite_guard::setStopHandler(guardStopHandler);
            GuardEnv env;
            EeScheduler &ee = env.start();
            requestPreemption(env, ee);
            bool threw = false;
            {
                ps2x_rewrite_guard::Scope s(0x0016F140u, "applyMoveDamage");
                try
                {
                    ee.transferIfRequested(false);
                }
                catch (const EeDispatcherTransfer &)
                {
                    threw = true;
                }
                t.IsTrue(!threw, "transferIfRequested under a rewrite does not throw");
                t.IsTrue(ee.checkpointDue(0), "the checkpoint flag is raised so the switch happens at the next checkpoint");
            }
            t.Equals(ps2x_rewrite_guard::counts().requested[static_cast<int>(ps2x_rewrite_guard::Site::Preemption)], 1ull, "one deferred pre-emption counted");
            try
            {
                ee.transferIfRequested(false);
            }
            catch (const EeDispatcherTransfer &)
            {
                threw = true;
            }
            t.IsTrue(threw, "with no rewrite on the stack the same pending pre-emption transfers: it was deferred, not lost");
        });

        tc.Run("the interrupt-safe form never transfers, so it is not counted under a rewrite", [](TestCase &t)
        {
            ps2x_rewrite_guard::setStopHandler(guardStopHandler);
            GuardEnv env;
            EeScheduler &ee = env.start();
            requestPreemption(env, ee);
            ps2x_rewrite_guard::Scope s(0x0016F140u, "applyMoveDamage");
            ee.transferIfRequested(true);
            t.Equals(ps2x_rewrite_guard::counts().requested[static_cast<int>(ps2x_rewrite_guard::Site::Preemption)], 0ull, "nothing counted");
        });
    });
}
