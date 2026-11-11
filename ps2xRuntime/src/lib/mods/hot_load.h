// Load Now: applying data-pack changes to the running game without a restart (task h1; docs/mod-layer.md "Hot load",
// docs/formats/fprmod.md "Load Now and Restart Now"; the feasibility study is bringup.md "h0").
//
// What a hot load is: the game's own start-up loader (the call at 0x23F420 that reads the memory card at every start) is run again,
// from a safe place, on the card the data packs now describe. Everything the game derives from its save is rebuilt by that loader
// in place (task h0 showed the result is byte for byte what a restart gives). This file is the host's half: when to offer it, what
// to refuse, what to do in which order, and how to check the result. The game's half (the wrappers that run the loader) is in
// src/overrides/fpwr_overrides.cpp, and the glue to the manager, the profile card and the guest is ps2_host_mods.cpp.
//
// The rules, in the order the code applies them:
//   - It is offered only while the game stands at a safe point: the title screen's "PRESS START" or the main menu waiting for a
//     choice. The editors keep working copies they write back later and a match holds the data live, so anywhere else it is refused,
//     never queued for later. The game thread stamps each pass through a safe point (noteSafePoint); the host decides once, when the
//     game has stopped for the overlay (guestStopped), whether the last stamp is fresh.
//   - The player confirms first: whatever changed in the game since the last save is lost, as with the game's own Data > Load.
//   - The request waits (at most kRequestTimeoutNs) for the next pass through a safe point. There, on the game thread, in one step:
//     the card is looked at (a file open for writing refuses), the profile card is rebuilt from the data packs that are on now and
//     checked (any problem refuses), the card root is switched if it changes, and the loader is started. The card is never switched
//     without loading: the next save in the game would otherwise write the old data over the pack (h0 probe 17).
//   - When the loader returns, the game's memory is compared with the save it was given (the roster data and the edit slots); only
//     then the manager's session is moved forward for the data packs (the other kinds still need a restart).
//
// Header only and free of SDL and game code: the unit tests drive the Controller with fake steps and fake clocks, and the host
// (ps2_host_mods.cpp) binds the real ones.
#pragma once

#include "fpr_save.h"
#include "mod_manager.h"
#include "profile.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ps2x::mods::hotload
{
    // A safe point is fresh when the game stopped (the overlay opened) no more than this long after the last pass through it. The
    // game passes through a safe point once per frame, so this is about fifteen frames.
    constexpr uint64_t kFreshNs = 250'000'000ull;
    // How long a confirmed request waits for the game to reach a safe point.
    constexpr uint64_t kRequestTimeoutNs = 4'000'000'000ull;

    // The two screens that are safe, and where they are in the save.
    enum class Where : int
    {
        None = 0,
        Title = 1, // "PRESS START"
        Menu = 2,  // the main menu waiting for a choice
    };

    // The parts of the game's memory a load rebuilds from the save, and where they sit in the save file (docs/formats/edit-save.md).
    constexpr size_t kRosterOffset = 0x111E0;
    constexpr size_t kRosterSize = 0x2E1B8;
    constexpr size_t kSlotsOffset = 0x3F398;
    constexpr size_t kSlotCount = 500;
    constexpr size_t kSlotSize = 0x44C;

    // The game's memory after a load, as the host sees it (pointers into the guest's RAM block; the roster data and the edit slots).
    struct GuestView
    {
        uint8_t *roster = nullptr;
        size_t rosterSize = 0;
        uint8_t *slots = nullptr;
        size_t slotsSize = 0;
    };

    struct Verdict
    {
        bool ok = false;
        size_t scrubbedSlots = 0; // unused slots that held leftovers of an earlier wrestler and were zeroed (a restart has them zero)
        std::string detail;       // when not ok: what differs, in a few words
    };

    // Compares the game's memory with the save the game was given. The roster data must be identical. A used edit slot (its first
    // two bytes non-zero) must be identical; for an unused one the game's loader only clears the flag and leaves the rest of the
    // slot as it was, so leftovers are zeroed here, which is how a restart finds them. Writes to the game's memory only for that.
    inline Verdict verifyGuest(const std::vector<uint8_t> &save, GuestView &guest)
    {
        Verdict v;
        if (save.size() != fpr::kSaveSize)
        {
            v.detail = "the save has the wrong size";
            return v;
        }
        if (!guest.roster || guest.rosterSize != kRosterSize || !guest.slots || guest.slotsSize != kSlotCount * kSlotSize)
        {
            v.detail = "the game's data could not be found in memory";
            return v;
        }
        const uint8_t *wantRoster = save.data() + kRosterOffset;
        if (std::memcmp(wantRoster, guest.roster, kRosterSize) != 0)
        {
            size_t first = 0, count = 0;
            for (size_t i = 0; i < kRosterSize; ++i)
                if (wantRoster[i] != guest.roster[i])
                {
                    if (count == 0)
                        first = i;
                    ++count;
                }
            v.detail = "the roster data differs in " + std::to_string(count) + " bytes (the first at " + std::to_string(first) + ")";
            return v;
        }
        size_t wrongSlots = 0, firstWrong = 0;
        for (size_t k = 0; k < kSlotCount; ++k)
        {
            const uint8_t *want = save.data() + kSlotsOffset + k * kSlotSize;
            uint8_t *have = guest.slots + k * kSlotSize;
            const bool wantUsed = want[0] != 0 || want[1] != 0;
            const bool haveUsed = have[0] != 0 || have[1] != 0;
            if (wantUsed != haveUsed)
            {
                if (wrongSlots++ == 0)
                    firstWrong = k;
                continue;
            }
            if (wantUsed)
            {
                if (std::memcmp(want, have, kSlotSize) != 0 && wrongSlots++ == 0)
                    firstWrong = k;
                continue;
            }
            bool leftovers = false;
            for (size_t i = 0; i < kSlotSize && !leftovers; ++i)
                leftovers = have[i] != 0;
            if (leftovers)
            {
                std::memset(have, 0, kSlotSize);
                ++v.scrubbedSlots;
            }
        }
        if (wrongSlots)
        {
            v.detail = std::to_string(wrongSlots) + " edit slots differ (the first is slot " + std::to_string(firstWrong) + ")";
            return v;
        }
        v.ok = true;
        return v;
    }

    // ---- the plan: which card, and what the game is about to load -------------------------------------------------------------

    struct Plan
    {
        std::filesystem::path root0; // the card root the game must read from now on (slot 1)
        bool useProfile = false;     // true: the profile card; false: the player's own card
        std::vector<uint8_t> save;   // the save the game is about to load, as read from root0 just before the load
        std::string line;            // the Mods tab's "Memory card: ..." line for the new state
        std::vector<profile::PackReport> packs; // one per data pack, in the order of `data.ids`
        std::vector<std::string> log;
        Manager::DataLoad data;      // the data packs this plan was built from (and what the session will say is loaded)
        double milliseconds = 0;
    };

    struct PlanInput
    {
        Manager *manager = nullptr;
        std::filesystem::path profileDir; // <pref folder>/profile
        std::filesystem::path baseCard0;  // the player's own card, slot 1 (as the runtime had it before any redirect)
        std::filesystem::path baseCard1;
        // The pack as the profile builder wants it (the host reads the pack's files through the manager; ps2_host_mods.cpp).
        std::function<profile::PackInput(const std::string &id)> readPack;
        std::function<bool(fpr::CrcTable &, std::string *)> loadCrcTable;
        // Anything else the profile builder takes that is not a data pack (the host fills what a session start fills: ps2_host_mods.cpp).
        std::function<void(profile::Config &)> extendConfig;
        int failpoint = 0; // tests: forwarded to the profile builder
    };

    struct PlanResult
    {
        bool ok = false;
        std::string refusal; // one sentence for the player when not ok
        Plan plan;
    };

    namespace detail
    {
        inline bool readBytes(const std::filesystem::path &path, std::vector<uint8_t> &out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            in.seekg(0, std::ios::end);
            const std::streamoff size = in.tellg();
            if (size < 0)
                return false;
            in.seekg(0, std::ios::beg);
            out.assign(static_cast<size_t>(size), 0);
            if (size > 0)
                in.read(reinterpret_cast<char *>(out.data()), size);
            return static_cast<bool>(in) || size == 0;
        }

        inline std::string u8(const std::filesystem::path &path)
        {
            const auto text = path.u8string();
            return std::string(text.begin(), text.end());
        }
    }

    // Decides the card the game plays from with the data packs that are on now, builds the profile card when it is the one, and checks
    // that the save the game will read is good. Nothing here touches the game or the card root; the profile folder is rewritten only
    // when it is going to be used, and a refusal after that point is not possible (the last check is before the profile is built).
    inline PlanResult makePlan(const PlanInput &in)
    {
        PlanResult result;
        auto refuse = [&](const std::string &why) {
            result.ok = false;
            result.refusal = why;
            return result;
        };
        if (!in.manager)
            return refuse("There is no mod manager.");
        if (in.profileDir.empty())
            return refuse("Data packs need the preference folder, which this run does not have.");

        // The checksum table first: the save about to be loaded is checked with it whichever card it comes from.
        fpr::CrcTable table{};
        {
            std::string why;
            if (!in.loadCrcTable || !in.loadCrcTable(table, &why))
                return refuse("The game's checksum table could not be read" + (why.empty() ? std::string(".") : " (" + why + ")."));
        }

        Plan plan;
        plan.data = in.manager->liveDataPacks();
        profile::Config config;
        config.profileDir = in.profileDir;
        config.baseCard0 = in.baseCard0;
        config.baseCard1 = in.baseCard1;
        config.crcTable = &table;
        config.failpoint = in.failpoint;
        if (in.extendConfig)
            in.extendConfig(config);
        for (const std::string &id : plan.data.ids)
        {
            if (in.readPack)
            {
                config.packs.push_back(in.readPack(id));
                continue;
            }
            profile::PackInput bare;
            bare.id = id;
            bare.name = id;
            config.packs.push_back(std::move(bare));
        }

        // A decision that does not touch the disk comes first: is the base card going to be used, and can it be loaded?
        const profile::CardPlan wanted = profile::planCard(in.profileDir, config.packs.size());
        if (!wanted.useProfile)
        {
            const std::filesystem::path save = in.baseCard0 / fpr::kSaveName / fpr::kSaveName;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(save, ec))
                return refuse("Your own memory card has no save to load. Restart the game to go back to it.");
            if (!detail::readBytes(save, plan.save) || plan.save.size() != fpr::kSaveSize)
                return refuse("The save on your own memory card is not a Fire Pro Wrestling Returns save, so the game would not load it.");
            const fpr::Integrity integrity = fpr::checkIntegrity(plan.save, table);
            if (!integrity.allOk())
                return refuse("The save on your own memory card does not pass the game's own checks, so the game would not load it.");
            if (!integrity.markersOk)
                return refuse("The save on your own memory card is not from this version of the game.");
            plan.useProfile = false;
            plan.root0 = in.baseCard0;
            plan.line = wanted.why == profile::CardWhy::ChoseOwnCard ? profile::ownCardChoiceLine() : profile::baseCardLine();
            for (const std::string &id : plan.data.ids)
            {
                profile::PackReport report;
                report.id = id;
                plan.packs.push_back(std::move(report));
            }
            result.ok = true;
            result.plan = std::move(plan);
            return result;
        }

        const profile::Selection selection = profile::select(config);
        plan.log = selection.log;
        plan.milliseconds = selection.result.milliseconds;
        if (!selection.useProfile)
            return refuse("The profile card could not be chosen."); // select() agreed with planCard a moment ago; not expected
        const profile::Result &built = selection.result;
        if (!built.problem.empty())
            return refuse(built.problem);
        if (!built.saveFound)
            return refuse("There is no save to load yet. Save once in the game, then try again.");
        const std::filesystem::path save = built.root0 / fpr::kSaveName / fpr::kSaveName;
        if (!detail::readBytes(save, plan.save) || plan.save.size() != fpr::kSaveSize)
            return refuse("The save on the profile card could not be read.");
        const fpr::Integrity integrity = fpr::checkIntegrity(plan.save, table);
        if (!integrity.allOk() || !integrity.markersOk)
            return refuse("The save on the profile card does not pass the game's own checks, so the game would not load it.");
        plan.useProfile = true;
        plan.root0 = built.root0;
        plan.line = selection.line;
        plan.packs = built.packs;
        result.ok = true;
        result.plan = std::move(plan);
        return result;
    }

    // ---- the request and the state around it -----------------------------------------------------------------------------------

    class Controller
    {
    public:
        enum class State : int
        {
            Idle,      // nothing asked
            Requested, // confirmed; waiting for the game to reach a safe point
            Loading,   // the loader is running
            Loaded,    // the last request succeeded
            Refused,   // the last request did not start; message() says why
            Failed,    // the loader ran but the game's memory does not match the card
        };

        // What the Mods tab needs: whether to show Load Now, whether it works now, and the words.
        struct Offer
        {
            bool shown = false;   // the Load Now button is on the tab
            bool enabled = false; // and can be used right now
            bool unsaved = false; // the game says it holds changes not yet saved
            std::string note;     // the line beside the buttons ("" for none)
            std::string help;     // the button's help line: what it does, or why it cannot be used now
            std::string question; // the body of "Load now?"
        };

        struct Hooks
        {
            // "" when the card is quiet, else why not (a file is open for writing).
            std::function<std::string()> cardBusy;
            // Builds and checks what is to be loaded. False with `why` when the load must be refused.
            std::function<bool(Plan &, std::string &)> plan;
            // Points the game at the plan's card root. Called only when the loader starts right after.
            std::function<void(const Plan &)> switchCard;
            // After the loader returned and the memory matched: the session moves forward, notes and card line are updated.
            std::function<void(const Plan &)> commit;
        };

        // ---- game thread ----

        // A pass through a safe point. Cheap: stores only.
        void noteSafePoint(Where where, bool unsaved, uint64_t nowNs) noexcept
        {
            m_unsaved.store(unsaved, std::memory_order_relaxed);
            m_where.store(static_cast<int>(where), std::memory_order_relaxed);
            m_lastSafeNs.store(nowNs, std::memory_order_release);
        }
        // True while a confirmed request is waiting: the wrappers' whole cost when nothing is pending is this one load.
        bool requested() const noexcept { return m_state.load(std::memory_order_acquire) == static_cast<int>(State::Requested); }

        // At a safe point with requested() true. Runs the steps; true: the loader is to be started right now.
        bool begin(Where where, uint64_t nowNs, const Hooks &hooks)
        {
            if (!requested())
                return false;
            uint64_t askedNs;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                askedNs = m_requestedNs;
            }
            if (nowNs > askedNs && nowNs - askedNs > kRequestTimeoutNs)
            {
                finish(State::Refused, "Load Now did not start: the game took too long to reach the title screen or the main menu.");
                return false;
            }
            if (hooks.cardBusy)
            {
                const std::string busy = hooks.cardBusy();
                if (!busy.empty())
                {
                    finish(State::Refused, "Load Now did not start: " + busy);
                    return false;
                }
            }
            auto plan = std::make_unique<Plan>();
            std::string why;
            if (!hooks.plan || !hooks.plan(*plan, why))
            {
                finish(State::Refused, "Load Now did not start: " + (why.empty() ? std::string("the memory card could not be prepared.") : why));
                return false;
            }
            if (hooks.switchCard)
                hooks.switchCard(*plan);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_plan = std::move(plan);
                m_loadWhere = where;
                m_message.clear();
            }
            m_state.store(static_cast<int>(State::Loading), std::memory_order_release);
            return true;
        }

        // After the loader returned. `guest` is the game's memory; it may be written (leftovers in unused edit slots are zeroed).
        void end(GuestView &guest, const Hooks &hooks)
        {
            if (m_state.load(std::memory_order_acquire) != static_cast<int>(State::Loading))
                return;
            std::unique_ptr<Plan> plan;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                plan = std::move(m_plan);
            }
            if (!plan)
            {
                finish(State::Failed, "Load Now ran, but its plan was lost. Restart the game to be sure.");
                return;
            }
            const Verdict verdict = verifyGuest(plan->save, guest);
            if (!verdict.ok)
            {
                finish(State::Failed, "The game loaded the memory card, but its data does not match it (" + verdict.detail + "). Restart the game to be sure.");
                return;
            }
            if (hooks.commit)
                hooks.commit(*plan);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_scrubbed = verdict.scrubbedSlots;
            m_message = "Loaded.";
            m_resultPending = false; // success is not announced by reopening the menu: the game shows its own "Load Completed."
            m_state.store(static_cast<int>(State::Loaded), std::memory_order_release);
        }

        // ---- the UI thread ----

        // The game has stopped for the overlay: is it at a safe point? Decided once here, from the last stamp.
        void guestStopped(uint64_t nowNs)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const uint64_t last = m_lastSafeNs.load(std::memory_order_acquire);
            m_stopped = true;
            m_atSafePoint = last != 0 && nowNs >= last && nowNs - last <= kFreshNs;
            m_unsavedAtStop = m_atSafePoint && m_unsaved.load(std::memory_order_relaxed);
            m_whereAtStop = m_atSafePoint ? static_cast<Where>(m_where.load(std::memory_order_relaxed)) : Where::None;
        }
        // The overlay closed: the decision above no longer holds.
        void guestResumed()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopped = false;
            m_atSafePoint = false;
        }

        // `pending`: there is something for a load to apply (data packs changed, or the player chose Back to my card).
        Offer offer(bool pending) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            Offer o;
            const State s = static_cast<State>(m_state.load(std::memory_order_acquire));
            if (s == State::Requested || s == State::Loading)
            {
                o.note = s == State::Requested ? "Waiting for the game to load the memory card..." : "The game is loading the memory card...";
                return o;
            }
            if (!pending)
            {
                if (s == State::Loaded)
                    o.note = m_message;
                return o;
            }
            o.shown = true;
            o.unsaved = m_unsavedAtStop;
            o.question = m_unsavedAtStop ? "The game has changes you have not saved yet, and they will be lost. The memory card is loaded again, "
                                           "as if you had restarted."
                                         : "Anything you changed since you last saved in the game will be lost.";
            if (!m_stopped)
            {
                o.help = "Waiting for the game to pause.";
            }
            else if (!m_atSafePoint)
            {
                o.help = "Go to the title screen or the main menu.";
            }
            else
            {
                o.enabled = true;
                o.help = "Load the memory card again now, with the data packs as they are. Asks first.";
            }
            if (s == State::Refused || s == State::Failed)
                o.note = m_message;
            else
                o.note = "Data pack changes are waiting. Load Now applies them in the running game, or restart.";
            return o;
        }

        // The player confirmed. `pending` as above. Returns the sentence for the status line; ok() tells whether a request was made.
        struct Requested
        {
            bool ok = false;
            std::string message;
        };
        Requested request(bool pending, uint64_t nowNs)
        {
            Requested r;
            std::lock_guard<std::mutex> lock(m_mutex);
            const State s = static_cast<State>(m_state.load(std::memory_order_acquire));
            if (s == State::Requested || s == State::Loading)
            {
                r.message = "A load is already under way.";
                return r;
            }
            if (!pending)
            {
                r.message = "There is nothing to load.";
                return r;
            }
            if (!m_stopped)
            {
                r.message = "Waiting for the game to pause. Try again in a moment.";
                return r;
            }
            if (!m_atSafePoint)
            {
                r.message = "Go to the title screen or the main menu first.";
                return r;
            }
            m_requestedNs = nowNs;
            m_message.clear();
            m_resultPending = false;
            m_state.store(static_cast<int>(State::Requested), std::memory_order_release);
            r.ok = true;
            r.message = "Loading the memory card. The game shows its own \"Load Completed.\" when it is done.";
            return r;
        }

        // Called often by the host: a request that no safe point took in time is refused. True when that happened now.
        bool expire(uint64_t nowNs)
        {
            if (!requested())
                return false;
            uint64_t asked;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                asked = m_requestedNs;
            }
            if (nowNs <= asked || nowNs - asked <= kRequestTimeoutNs)
                return false;
            return finish(State::Refused, "Load Now did not start: the game was not at the title screen or the main menu.");
        }

        // A refusal or failure the player has not been told about (the menu was closed when it happened), once.
        bool takeResult(std::string &message)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_resultPending)
                return false;
            m_resultPending = false;
            message = m_message;
            return true;
        }

        State state() const { return static_cast<State>(m_state.load(std::memory_order_acquire)); }
        std::string message() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_message;
        }
        size_t scrubbedSlots() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_scrubbed;
        }
        Where whereLoaded() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_loadWhere;
        }
        // Forgets a finished result (the pending set changed, or a test starts over).
        void reset()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state.store(static_cast<int>(State::Idle), std::memory_order_release);
            m_message.clear();
            m_plan.reset();
            m_resultPending = false;
        }

    private:
        bool finish(State state, const std::string &message)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_state.load(std::memory_order_acquire) != static_cast<int>(State::Requested) &&
                m_state.load(std::memory_order_acquire) != static_cast<int>(State::Loading))
                return false;
            m_message = message;
            m_resultPending = true;
            m_plan.reset();
            m_state.store(static_cast<int>(state), std::memory_order_release);
            return true;
        }

        mutable std::mutex m_mutex;
        std::atomic<int> m_state{static_cast<int>(State::Idle)};
        std::atomic<uint64_t> m_lastSafeNs{0};
        std::atomic<int> m_where{0};
        std::atomic<bool> m_unsaved{false};
        // under the mutex
        bool m_stopped = false;
        bool m_atSafePoint = false;
        bool m_unsavedAtStop = false;
        Where m_whereAtStop = Where::None;
        Where m_loadWhere = Where::None;
        uint64_t m_requestedNs = 0;
        std::string m_message;
        bool m_resultPending = false;
        size_t m_scrubbed = 0;
        std::unique_ptr<Plan> m_plan;
    };
}
