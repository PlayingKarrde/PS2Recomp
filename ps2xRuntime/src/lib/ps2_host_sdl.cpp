// The desktop host layer on SDL3: window, picture, keyboard and controller input, audio output.
// See include/ps2_host_backend.h for what each function promises.
//
// Presenting the picture: by default through SDL_GPU (ps2_host_gpu.cpp). SDL_Renderer stays as a
// fallback, used automatically when no SDL_GPU device can be created or the window cannot be claimed
// for it (machines without Vulkan, Direct3D 12 or Metal). PS2X_PRESENT=gpu or PS2X_PRESENT=renderer
// forces one of them; with "gpu" forced, a failure to set it up is an error, not a fallback.
//
// Threads:
//  - The main thread owns the window and the renderer (or the GPU device). It runs pumpEvents(),
//    updatePicture(), drawPicture() and presentAndPace() once per host frame.
//  - The game thread (the PS2's CPU) reads the pad while it runs. It never calls SDL for input: the main
//    thread copies keyboard and controller state into atomics, and the game thread reads those. (With the GS GPU
//    renderer it does use the SDL_GPU device for drawing; the window and swapchain stay on the main thread.)
//  - SDL's audio thread calls audioCallback(), which asks the SPU2 emulation for samples.
//
// Short presses: a key that goes down and up between two reads by the game would be invisible
// to a program that only looks at "is it down now". Each key-down event therefore also sets a
// "latch" for every reader, which the reader's next read consumes. The same is done for
// controller buttons. A latch is only good for kLatchMaxNs: a tap made while the game is not
// reading its pad at all (a loading screen) is dropped, as it would be on a real pad, instead of
// arriving on the next screen.

#include "ps2_host_backend.h"
#include "ps2_host_gpu.h"

#include <SDL3/SDL.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <X11/Xlib.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace ps2x_host
{
    namespace
    {
        constexpr size_t kKeyCount = static_cast<size_t>(Key::Count);
        constexpr size_t kButtonCount = static_cast<size_t>(PadButton::Count);

        // ---- Keyboard ----

        constexpr std::array<SDL_Scancode, kKeyCount> kScancodeOfKey = {
            SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
            SDL_SCANCODE_W, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D,
            SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_Z, SDL_SCANCODE_V,
            SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_SPACE, SDL_SCANCODE_ESCAPE,
            SDL_SCANCODE_RETURN, SDL_SCANCODE_TAB, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT,
            SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL, SDL_SCANCODE_1, SDL_SCANCODE_3,
            SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_1, SDL_SCANCODE_F1};

        std::array<uint8_t, SDL_SCANCODE_COUNT> makeKeyOfScancode()
        {
            std::array<uint8_t, SDL_SCANCODE_COUNT> table{};
            table.fill(0xFFu);
            for (size_t key = 0; key < kKeyCount; ++key)
                table[kScancodeOfKey[key]] = static_cast<uint8_t>(key);
            return table;
        }
        const std::array<uint8_t, SDL_SCANCODE_COUNT> kKeyOfScancode = makeKeyOfScancode();

        // How long a press stays latched for a reader that has not looked yet. Several game frames
        // (the game reads its pad every frame or two, and a host frame is 16.7 ms), but far less
        // than a loading screen.
        constexpr uint64_t kLatchMaxNs = 100'000'000ull;

        std::array<std::atomic<uint8_t>, kKeyCount> g_keyHeld{};
        // When the key last went down and no one has read it since, in SDL_GetTicksNS() time; 0 = nothing latched.
        std::array<std::array<std::atomic<uint64_t>, kKeyCount>, ReaderCount> g_keyLatchNs{};

        // True if `stamp` (from a latch array) holds a press that is still fresh; clears it either way.
        bool takeLatch(std::atomic<uint64_t> &stamp)
        {
            const uint64_t when = stamp.exchange(0, std::memory_order_relaxed);
            return when != 0 && SDL_GetTicksNS() - when <= kLatchMaxNs;
        }
        std::atomic<bool> g_keyboardCaptured{false};

        // ---- Controllers ----

        // PS2 buttons from SDL's positional names: south = Cross, east = Circle, west = Square, north = Triangle.
        int padButtonFromSdl(Uint8 button)
        {
            switch (button)
            {
            case SDL_GAMEPAD_BUTTON_DPAD_UP:
                return static_cast<int>(PadButton::Up);
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                return static_cast<int>(PadButton::Down);
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                return static_cast<int>(PadButton::Left);
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                return static_cast<int>(PadButton::Right);
            case SDL_GAMEPAD_BUTTON_SOUTH:
                return static_cast<int>(PadButton::Cross);
            case SDL_GAMEPAD_BUTTON_EAST:
                return static_cast<int>(PadButton::Circle);
            case SDL_GAMEPAD_BUTTON_WEST:
                return static_cast<int>(PadButton::Square);
            case SDL_GAMEPAD_BUTTON_NORTH:
                return static_cast<int>(PadButton::Triangle);
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
                return static_cast<int>(PadButton::L1);
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                return static_cast<int>(PadButton::R1);
            case SDL_GAMEPAD_BUTTON_LEFT_STICK:
                return static_cast<int>(PadButton::L3);
            case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
                return static_cast<int>(PadButton::R3);
            case SDL_GAMEPAD_BUTTON_START:
                return static_cast<int>(PadButton::Start);
            case SDL_GAMEPAD_BUTTON_BACK:
                return static_cast<int>(PadButton::Select);
            default:
                return -1;
            }
        }

        // The old build counted a trigger as pressed above 0.1 on GLFW's -1..1 scale, which is a
        // bit past half way. SDL's triggers run 0..32767.
        constexpr int kTriggerThreshold = 18000;

        struct PadShared
        {
            std::atomic<uint32_t> held{0};
            std::array<std::array<std::atomic<uint64_t>, kButtonCount>, ReaderCount> latchNs{};
            std::array<std::atomic<int16_t>, 6> axis{};
            std::atomic<bool> present{false};
        };
        PadShared g_pad;

        struct OpenPad
        {
            SDL_JoystickID id;
            SDL_Gamepad *gamepad;
        };
        std::vector<OpenPad> g_openPads; // main thread only
        SDL_JoystickID g_activePad = 0;  // main thread only
        std::mutex g_padNameMutex;
        std::string g_padName;

        void setPadButton(int button, bool down)
        {
            if (button < 0)
                return;
            const uint32_t bit = 1u << button;
            if (down)
            {
                // Only a new press latches: a trigger reports "down" on every movement while it is past the threshold.
                const uint32_t before = g_pad.held.fetch_or(bit, std::memory_order_relaxed);
                if ((before & bit) == 0)
                {
                    const uint64_t now = std::max<uint64_t>(SDL_GetTicksNS(), 1u);
                    for (auto &latch : g_pad.latchNs)
                        latch[static_cast<size_t>(button)].store(now, std::memory_order_relaxed);
                }
            }
            else
            {
                g_pad.held.fetch_and(~bit, std::memory_order_relaxed);
            }
        }

        void clearPadState()
        {
            g_pad.held.store(0);
            for (auto &latch : g_pad.latchNs)
                for (auto &stamp : latch)
                    stamp.store(0);
            for (auto &axis : g_pad.axis)
                axis.store(0);
        }

        void activatePad(const OpenPad &pad)
        {
            clearPadState();
            g_activePad = pad.id;
            g_pad.present.store(true);
            const char *name = SDL_GetGamepadName(pad.gamepad);
            {
                std::lock_guard<std::mutex> lock(g_padNameMutex);
                g_padName = name ? name : "";
            }
            std::printf("[host] controller in use: \"%s\" (SDL instance %u)\n", name ? name : "?", static_cast<unsigned>(pad.id));
            std::fflush(stdout);
        }

        void padAdded(SDL_JoystickID id)
        {
            for (const OpenPad &pad : g_openPads)
                if (pad.id == id)
                    return;
            SDL_Gamepad *gamepad = SDL_OpenGamepad(id);
            if (!gamepad)
            {
                std::printf("[host] could not open controller %u: %s\n", static_cast<unsigned>(id), SDL_GetError());
                std::fflush(stdout);
                return;
            }
            g_openPads.push_back({id, gamepad});
            if (g_activePad == 0)
                activatePad(g_openPads.back());
        }

        void padRemoved(SDL_JoystickID id)
        {
            for (size_t i = 0; i < g_openPads.size(); ++i)
            {
                if (g_openPads[i].id != id)
                    continue;
                SDL_CloseGamepad(g_openPads[i].gamepad);
                g_openPads.erase(g_openPads.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
            if (g_activePad == id)
            {
                g_activePad = 0;
                clearPadState();
                g_pad.present.store(false);
                {
                    std::lock_guard<std::mutex> lock(g_padNameMutex);
                    g_padName.clear();
                }
                std::printf("[host] controller %u removed\n", static_cast<unsigned>(id));
                std::fflush(stdout);
                if (!g_openPads.empty())
                    activatePad(g_openPads.front());
            }
        }

        // ---- Opt-in counters for the short-press latch (PS2X_INPUT_STATS=1) ----

        std::atomic<uint64_t> g_statKeyDowns{0};
        std::atomic<uint64_t> g_statLatchOnlyReads{0}; // reads that saw a press only through the latch

        // ---- Window and picture ----

        struct Video
        {
            SDL_Window *window = nullptr;
            // Exactly one of these presents the picture: the SDL_GPU presenter (gpu), or the SDL_Renderer
            // fallback (renderer and picture).
            bool gpu = false;
            SDL_Renderer *renderer = nullptr;
            SDL_Texture *picture = nullptr;
            bool sdlVideoStarted = false;
            bool padSubsystemStarted = false;
        } g_video;

        // ---- Opt-in timing of the main thread's presentation work (PS2X_PRESENT_STATS=1) ----
        //
        // Three phases per host frame, summed over one second and printed by reportStats():
        //   update  - updatePicture(): getting the CPU picture to the presenter (once per game frame, not per host frame)
        //   draw    - drawPicture(): clear and draw (SDL_Renderer only queues commands; the GPU presenter only
        //             records the box)
        //   present - presentAndPace() without the pacing wait: everything else the presenter does (the GPU
        //             presenter's upload, render pass and submit; SDL_Renderer's flush and present)
        // The debug panel's own drawing happens between draw and present and is not in the numbers.

        struct PhaseTimer
        {
            uint64_t totalNs = 0;
            uint64_t maxNs = 0;
            uint32_t count = 0;
            void add(uint64_t ns)
            {
                totalNs += ns;
                maxNs = std::max(maxNs, ns);
                ++count;
            }
            void clear() { *this = PhaseTimer{}; }
        };
        struct PresentStats
        {
            bool on = false;
            PhaseTimer update, draw, present;
        } g_presentStats;

        // Adds the time spent in its scope to a PhaseTimer, if the stats are on.
        class ScopedPhase
        {
        public:
            explicit ScopedPhase(PhaseTimer &timer) : m_timer(timer), m_start(g_presentStats.on ? SDL_GetTicksNS() : 0) {}
            ~ScopedPhase()
            {
                if (g_presentStats.on)
                    m_timer.add(SDL_GetTicksNS() - m_start);
            }
            ScopedPhase(const ScopedPhase &) = delete;
            ScopedPhase &operator=(const ScopedPhase &) = delete;

        private:
            PhaseTimer &m_timer;
            uint64_t m_start;
        };

        // ---- Opt-in hooks for comparing presenters on identical pictures ----
        //
        // Nearly every game screen moves, so two runs never show the same picture, and the SDL_GPU and
        // SDL_Renderer presenters cannot be compared screenshot against screenshot. These two variables let
        // one real picture be captured and then shown by either presenter in a later run:
        //   PS2X_PRESENT_DUMP=<file>   when the file <file>.go exists, write the picture being shown (the
        //                              top-left srcWidth x srcHeight part of the picture texture, its shape,
        //                              and all pixels of the texture) to <file>, and delete <file>.go
        //   PS2X_PRESENT_FRAME=<file>  ignore the game's pictures and show the one in <file> instead (640 x 512 files only)
        // The file is: "PS2XPIC1", then little-endian uint32 srcWidth, srcHeight, float aspect, uint32 width, height
        // (640, 512; with the GS GPU renderer at an internal resolution above 1 that times the scale), then width x height x 4
        // RGBA bytes. Nothing else uses them.

        struct PictureHooks
        {
            std::string dumpPath;
            std::vector<uint8_t> lastPixels;
            int lastWidth = 0, lastHeight = 0;
            float lastAspect = 0.0f;
            bool havePixels = false;
            // GPU source (path B): the dump is written after the next present(), from the picture texture.
            bool dumpPending = false;
            int pendingWidth = 0, pendingHeight = 0;
            float pendingAspect = 0.0f;

            bool fixed = false;
            std::vector<uint8_t> fixedPixels;
            int fixedWidth = 0, fixedHeight = 0;
            float fixedAspect = 0.0f;
        } g_pictureHooks;

        constexpr char kPictureFileMagic[8] = {'P', 'S', '2', 'X', 'P', 'I', 'C', '1'};

        void loadPictureHooks()
        {
            if (const char *dump = std::getenv("PS2X_PRESENT_DUMP"))
                g_pictureHooks.dumpPath = dump;
            const char *frame = std::getenv("PS2X_PRESENT_FRAME");
            if (!frame || !frame[0])
                return;
            std::FILE *file = std::fopen(frame, "rb");
            char magic[8] = {};
            uint32_t header[5] = {};
            const size_t bytes = static_cast<size_t>(kPictureWidth) * kPictureHeight * 4u;
            std::vector<uint8_t> pixels(bytes);
            const bool ok = file && std::fread(magic, 1, 8, file) == 8 && std::memcmp(magic, kPictureFileMagic, 8) == 0 &&
                            std::fread(header, sizeof(uint32_t), 5, file) == 5 && header[3] == kPictureWidth &&
                            header[4] == kPictureHeight && std::fread(pixels.data(), 1, bytes, file) == bytes;
            if (file)
                std::fclose(file);
            if (!ok)
            {
                std::fprintf(stderr, "[host] PS2X_PRESENT_FRAME=%s is not a usable picture file, ignored\n", frame);
                return;
            }
            g_pictureHooks.fixedPixels = std::move(pixels);
            g_pictureHooks.fixedWidth = static_cast<int>(header[0]);
            g_pictureHooks.fixedHeight = static_cast<int>(header[1]);
            std::memcpy(&g_pictureHooks.fixedAspect, &header[2], sizeof(float));
            g_pictureHooks.fixed = true;
            std::printf("[host] showing the fixed picture %s (%dx%d, aspect %.4f) instead of the game's\n",
                        frame, g_pictureHooks.fixedWidth, g_pictureHooks.fixedHeight,
                        static_cast<double>(g_pictureHooks.fixedAspect));
        }

        // Writes the picture file described above.
        void writePictureFile(int srcWidth, int srcHeight, float aspect, const std::vector<uint8_t> &pixels)
        {
            std::FILE *file = std::fopen(g_pictureHooks.dumpPath.c_str(), "wb");
            if (!file)
                return;
            // The texture the picture sits in: kPictureWidth x kPictureHeight, or with the GS GPU renderer at a higher internal
            // resolution that times its scale (the pixels are then that many rows of that many pixels).
            int textureWidth = kPictureWidth, textureHeight = kPictureHeight;
            gpu_present::pictureTextureSize(textureWidth, textureHeight);
            const uint32_t header[5] = {static_cast<uint32_t>(srcWidth), static_cast<uint32_t>(srcHeight), 0u,
                                        static_cast<uint32_t>(textureWidth), static_cast<uint32_t>(textureHeight)};
            uint32_t withAspect[5];
            std::memcpy(withAspect, header, sizeof(withAspect));
            std::memcpy(&withAspect[2], &aspect, sizeof(float));
            std::fwrite(kPictureFileMagic, 1, 8, file);
            std::fwrite(withAspect, sizeof(uint32_t), 5, file);
            std::fwrite(pixels.data(), 1, pixels.size(), file);
            std::fclose(file);
            std::printf("[host] picture written to %s (%dx%d)\n", g_pictureHooks.dumpPath.c_str(), srcWidth, srcHeight);
            std::fflush(stdout);
        }

        // Called from drawPicture(): writes the dump if its trigger file is there. With the GS GPU renderer's
        // direct path there are no CPU pixels: the dump is then written after this frame's present(), by
        // writePendingDump(), from a download of the picture texture (opt-in, so the wait is fine).
        void maybeDumpPicture(int srcWidth, int srcHeight, float aspect)
        {
            if (g_pictureHooks.dumpPath.empty())
                return;
            const bool fromGpu = gpu_present::hasPictureSource();
            if (!fromGpu && !g_pictureHooks.havePixels)
                return;
            const std::string trigger = g_pictureHooks.dumpPath + ".go";
            std::FILE *probe = std::fopen(trigger.c_str(), "rb");
            if (!probe)
                return;
            std::fclose(probe);
            std::remove(trigger.c_str());
            if (fromGpu)
            {
                g_pictureHooks.dumpPending = true;
                g_pictureHooks.pendingWidth = srcWidth;
                g_pictureHooks.pendingHeight = srcHeight;
                g_pictureHooks.pendingAspect = aspect;
                return;
            }
            writePictureFile(srcWidth, srcHeight, aspect, g_pictureHooks.lastPixels);
        }

        void writePendingDump()
        {
            if (!g_pictureHooks.dumpPending)
                return;
            g_pictureHooks.dumpPending = false;
            std::vector<uint8_t> pixels;
            if (!gpu_present::downloadPicture(pixels))
            {
                std::fprintf(stderr, "[host] could not download the picture for PS2X_PRESENT_DUMP\n");
                return;
            }
            writePictureFile(g_pictureHooks.pendingWidth, g_pictureHooks.pendingHeight, g_pictureHooks.pendingAspect, pixels);
        }
        std::atomic<bool> g_quitRequested{false};
        std::atomic<void (*)(const void *)> g_eventHook{nullptr};

        // ---- Audio ----

        struct Audio
        {
            SDL_AudioStream *stream = nullptr;
            AudioMixFn mix = nullptr;
            void *mixUser = nullptr;
            bool started = false;
            bool statsOn = false;
            std::atomic<uint64_t> sumSquares{0};
            std::atomic<uint64_t> frames{0};
            std::atomic<uint32_t> peak{0};
            std::atomic<uint32_t> callbacks{0};
            std::atomic<uint32_t> maxRequestBytes{0};
            std::atomic<uint32_t> minRequestBytes{0xFFFFFFFFu};
            std::atomic<uint32_t> maxQueuedBytes{0};
            std::atomic<uint32_t> minQueuedBytes{0xFFFFFFFFu};
            // How much audio is kept queued ahead of the device, on top of the device's own buffer.
            int queueTargetBytes = 0;
            float gain = 1.0f;
        } g_audio;

        // The old build's audio device ran with a 1440-frame period (30 ms at 48 kHz, from its log).
        // Total buffering here is kept near that: the device's period plus a queue of our own.
        constexpr int kTotalBufferFrames = 1440;

        void SDLCALL audioCallback(void *, SDL_AudioStream *stream, int additionalAmount, int totalAmount)
        {
            constexpr int kChunkFrames = 1024;
            alignas(16) int16_t buffer[kChunkFrames * 2];
            // The device is about to take totalAmount bytes. To leave queueTargetBytes behind afterwards,
            // add enough to cover what it takes and what is missing from the queue.
            const int queued = SDL_GetAudioStreamQueued(stream);
            const int wanted = std::max(additionalAmount, g_audio.queueTargetBytes + totalAmount - queued);
            if (g_audio.statsOn)
            {
                g_audio.callbacks.fetch_add(1, std::memory_order_relaxed);
                const uint32_t request = static_cast<uint32_t>(std::max(totalAmount, 0));
                uint32_t seen = g_audio.maxRequestBytes.load(std::memory_order_relaxed);
                while (request > seen && !g_audio.maxRequestBytes.compare_exchange_weak(seen, request)) {}
                seen = g_audio.minRequestBytes.load(std::memory_order_relaxed);
                while (request < seen && !g_audio.minRequestBytes.compare_exchange_weak(seen, request)) {}
                const uint32_t depth = static_cast<uint32_t>(std::max(queued, 0));
                seen = g_audio.maxQueuedBytes.load(std::memory_order_relaxed);
                while (depth > seen && !g_audio.maxQueuedBytes.compare_exchange_weak(seen, depth)) {}
                seen = g_audio.minQueuedBytes.load(std::memory_order_relaxed);
                while (depth < seen && !g_audio.minQueuedBytes.compare_exchange_weak(seen, depth)) {}
            }
            if (wanted <= 0)
                return;
            int remainingFrames = wanted / static_cast<int>(2 * sizeof(int16_t));
            while (remainingFrames > 0)
            {
                const int frames = std::min(remainingFrames, kChunkFrames);
                if (g_audio.mix)
                    g_audio.mix(g_audio.mixUser, buffer, static_cast<unsigned>(frames));
                else
                    std::memset(buffer, 0, static_cast<size_t>(frames) * 2u * sizeof(int16_t));
                if (g_audio.statsOn)
                {
                    uint64_t sum = 0;
                    uint32_t peak = 0;
                    for (int i = 0; i < frames * 2; ++i)
                    {
                        const int32_t sample = buffer[i];
                        const uint32_t magnitude = static_cast<uint32_t>(sample < 0 ? -sample : sample);
                        peak = std::max(peak, magnitude);
                        sum += static_cast<uint64_t>(static_cast<int64_t>(sample) * sample);
                    }
                    g_audio.sumSquares.fetch_add(sum, std::memory_order_relaxed);
                    g_audio.frames.fetch_add(static_cast<uint64_t>(frames), std::memory_order_relaxed);
                    uint32_t seen = g_audio.peak.load(std::memory_order_relaxed);
                    while (peak > seen && !g_audio.peak.compare_exchange_weak(seen, peak)) {}
                }
                SDL_PutAudioStreamData(stream, buffer, frames * 2 * static_cast<int>(sizeof(int16_t)));
                remainingFrames -= frames;
            }
        }

        // Called from the main thread once per host frame; prints at most one line a second.
        void reportStats()
        {
            const bool audio = g_audio.statsOn;
            static const bool input = []()
            {
                const char *value = std::getenv("PS2X_INPUT_STATS");
                return value && value[0] == '1';
            }();
            const bool presenting = g_presentStats.on;
            if (!audio && !input && !presenting)
                return;
            static uint64_t lastMs = SDL_GetTicks();
            const uint64_t now = SDL_GetTicks();
            if (now - lastMs < 1000u)
                return;
            const double seconds = static_cast<double>(now - lastMs) / 1000.0;
            lastMs = now;
            if (audio)
            {
                const uint64_t frames = g_audio.frames.exchange(0);
                const uint64_t sum = g_audio.sumSquares.exchange(0);
                const uint32_t peak = g_audio.peak.exchange(0);
                const uint32_t callbacks = g_audio.callbacks.exchange(0);
                const uint32_t maxRequest = g_audio.maxRequestBytes.exchange(0);
                const uint32_t minRequest = g_audio.minRequestBytes.exchange(0xFFFFFFFFu);
                const uint32_t maxQueued = g_audio.maxQueuedBytes.exchange(0);
                const uint32_t minQueued = g_audio.minQueuedBytes.exchange(0xFFFFFFFFu);
                const double rms = frames ? std::sqrt(static_cast<double>(sum) / (static_cast<double>(frames) * 2.0)) : 0.0;
                std::printf("[audio:stats] t=%.1fs frames=%llu peak=%u rms=%.0f callbacks=%u device_pull_frames=%u..%u queued_frames=%u..%u\n",
                            static_cast<double>(now) / 1000.0, static_cast<unsigned long long>(frames), peak, rms, callbacks,
                            minRequest == 0xFFFFFFFFu ? 0u : minRequest / 4u, maxRequest / 4u,
                            minQueued == 0xFFFFFFFFu ? 0u : minQueued / 4u, maxQueued / 4u);
            }
            if (presenting)
            {
                const PhaseTimer update = g_presentStats.update;
                const PhaseTimer draw = g_presentStats.draw;
                const PhaseTimer present = g_presentStats.present;
                g_presentStats.update.clear();
                g_presentStats.draw.clear();
                g_presentStats.present.clear();
                auto avgMs = [](const PhaseTimer &t) { return t.count ? static_cast<double>(t.totalNs) / t.count / 1e6 : 0.0; };
                auto maxMs = [](const PhaseTimer &t) { return static_cast<double>(t.maxNs) / 1e6; };
                const double perFrameMs = present.count ? static_cast<double>(update.totalNs + draw.totalNs + present.totalNs) / present.count / 1e6 : 0.0;
                std::printf("[present:stats] t=%.1fs presenter=%s host_frames=%u pictures=%u update_ms avg=%.3f max=%.3f "
                            "draw_ms avg=%.3f max=%.3f present_ms avg=%.3f max=%.3f main_thread_ms_per_frame=%.3f (over %.1fs)\n",
                            static_cast<double>(now) / 1000.0, g_video.gpu ? "gpu" : "renderer", present.count, update.count,
                            avgMs(update), maxMs(update), avgMs(draw), maxMs(draw), avgMs(present), maxMs(present), perFrameMs, seconds);
            }
            if (input)
            {
                std::printf("[input:stats] t=%.1fs key_downs=%llu latch_only_reads=%llu (over %.1fs)\n",
                            static_cast<double>(now) / 1000.0,
                            static_cast<unsigned long long>(g_statKeyDowns.exchange(0)),
                            static_cast<unsigned long long>(g_statLatchOnlyReads.exchange(0)), seconds);
            }
            std::fflush(stdout);
        }

        void pace()
        {
            static uint64_t deadline = 0;
            constexpr uint64_t kPeriodNs = 1000000000ull / 60ull;
            const uint64_t now = SDL_GetTicksNS();
            if (deadline == 0)
            {
                deadline = now;
                return;
            }
            deadline += kPeriodNs;
            if (now < deadline)
                SDL_DelayPrecise(deadline - now);
            else if (now > deadline + kPeriodNs)
                deadline = now; // more than a frame behind: don't try to catch up
        }

#if defined(__linux__) && !defined(__ANDROID__)
        // tools/drive_game.py sends key events with XSendEvent, and leaves their time field at 0. SDL's X11
        // code takes a key release followed (anywhere in its queue) by a press of the same key with a similar
        // time for auto-repeat, and drops the release. When the window thread falls behind by half a second the
        // driver's release and next press are both waiting, so the two presses fuse into one. Real keyboards
        // stamp their events, so only synthetic key events (send_event) get a time here: the moment SDL reads
        // them. The press still waiting in the queue then looks far apart in time from this release.
        bool SDLCALL stampSyntheticKeys(void *, XEvent *xevent)
        {
            if ((xevent->type == KeyPress || xevent->type == KeyRelease) && xevent->xkey.send_event)
                xevent->xkey.time = static_cast<Time>(SDL_GetTicks() + 1u);
            return true;
        }
#endif

        void handleEvent(const SDL_Event &event)
        {
            switch (event.type)
            {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                g_quitRequested.store(true);
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            {
                if (event.key.scancode >= SDL_SCANCODE_COUNT)
                    break;
                const uint8_t key = kKeyOfScancode[event.key.scancode];
                if (key == 0xFFu)
                    break;
                if (event.key.down)
                {
                    g_keyHeld[key].store(1, std::memory_order_relaxed);
                    if (!event.key.repeat)
                    {
                        g_statKeyDowns.fetch_add(1, std::memory_order_relaxed);
                        const uint64_t now = std::max<uint64_t>(SDL_GetTicksNS(), 1u);
                        for (auto &latch : g_keyLatchNs)
                            latch[key].store(now, std::memory_order_relaxed);
                    }
                }
                else
                {
                    g_keyHeld[key].store(0, std::memory_order_relaxed);
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_ADDED:
                padAdded(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                padRemoved(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (event.gbutton.which == g_activePad)
                    setPadButton(padButtonFromSdl(event.gbutton.button), event.gbutton.down);
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (event.gaxis.which == g_activePad)
                {
                    const int16_t value = event.gaxis.value;
                    switch (event.gaxis.axis)
                    {
                    case SDL_GAMEPAD_AXIS_LEFTX:
                        g_pad.axis[0].store(value);
                        break;
                    case SDL_GAMEPAD_AXIS_LEFTY:
                        g_pad.axis[1].store(value);
                        break;
                    case SDL_GAMEPAD_AXIS_RIGHTX:
                        g_pad.axis[2].store(value);
                        break;
                    case SDL_GAMEPAD_AXIS_RIGHTY:
                        g_pad.axis[3].store(value);
                        break;
                    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
                        g_pad.axis[4].store(value);
                        setPadButton(static_cast<int>(PadButton::L2), value > kTriggerThreshold);
                        break;
                    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
                        g_pad.axis[5].store(value);
                        setPadButton(static_cast<int>(PadButton::R2), value > kTriggerThreshold);
                        break;
                    default:
                        break;
                    }
                }
                break;
            default:
                break;
            }
        }
    } // namespace

    // ---------------------------------------------------------------- input

    bool keyDown(Key key, int reader)
    {
        const size_t index = static_cast<size_t>(key);
        if (index >= kKeyCount)
            return false;
        reader = std::clamp(reader, 0, static_cast<int>(ReaderCount) - 1);
        const bool latched = takeLatch(g_keyLatchNs[reader][index]);
        const bool held = g_keyHeld[index].load(std::memory_order_relaxed) != 0;
        if (reader <= ReaderPort1 && g_keyboardCaptured.load(std::memory_order_relaxed))
            return false;
        if (latched && !held)
            g_statLatchOnlyReads.fetch_add(1, std::memory_order_relaxed);
        return held || latched;
    }

    bool keyPressed(Key key, int reader)
    {
        const size_t index = static_cast<size_t>(key);
        if (index >= kKeyCount)
            return false;
        reader = std::clamp(reader, 0, static_cast<int>(ReaderCount) - 1);
        return takeLatch(g_keyLatchNs[reader][index]);
    }

    bool gamepadAvailable()
    {
        return g_pad.present.load(std::memory_order_relaxed);
    }

    bool gamepadButtonDown(PadButton button, int reader)
    {
        const size_t index = static_cast<size_t>(button);
        if (index >= kButtonCount)
            return false;
        reader = std::clamp(reader, 0, static_cast<int>(ReaderCount) - 1);
        const uint32_t bit = 1u << index;
        const bool latched = takeLatch(g_pad.latchNs[reader][index]);
        const bool held = (g_pad.held.load(std::memory_order_relaxed) & bit) != 0;
        return held || latched;
    }

    float gamepadAxis(PadAxis axis)
    {
        const size_t index = static_cast<size_t>(axis);
        if (index >= 4)
            return 0.0f;
        return std::clamp(static_cast<float>(g_pad.axis[index].load(std::memory_order_relaxed)) / 32767.0f, -1.0f, 1.0f);
    }

    std::string gamepadName()
    {
        std::lock_guard<std::mutex> lock(g_padNameMutex);
        return g_padName;
    }

    void setKeyboardCapturedByUi(bool captured)
    {
        g_keyboardCaptured.store(captured, std::memory_order_relaxed);
    }

    // ------------------------------------------------------- window and picture

    bool windowReady()
    {
        return g_video.window != nullptr && (g_video.gpu || g_video.renderer != nullptr);
    }

    bool createWindow(const char *title, int width, int height, ScaleMode scale)
    {
        if (windowReady())
            return true;

        // Game controllers keep working when the window is not in front, as they did with GLFW.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#if defined(__linux__)
        // Prefer X11 (through XWayland on a Wayland desktop): that is what the previous build used,
        // and tools/drive_game.py finds the window and sends keys over X11. SDL_VIDEO_DRIVER overrides it.
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11,wayland");
#endif
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            std::fprintf(stderr, "[host] SDL video failed to start: %s\n", SDL_GetError());
            return false;
        }
        g_video.sdlVideoStarted = true;
#if defined(__linux__) && !defined(__ANDROID__)
        SDL_SetX11EventHook(stampSyntheticKeys, nullptr);
#endif
        if (SDL_InitSubSystem(SDL_INIT_GAMEPAD))
            g_video.padSubsystemStarted = true;
        else
            std::fprintf(stderr, "[host] SDL gamepad support failed to start (the keyboard still works): %s\n", SDL_GetError());

        g_video.window = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE);
        if (!g_video.window)
        {
            std::fprintf(stderr, "[host] could not create the window: %s\n", SDL_GetError());
            destroyWindow();
            return false;
        }
        // Which presenter: SDL_GPU unless told otherwise, with SDL_Renderer as the fallback.
        const char *wanted = std::getenv("PS2X_PRESENT");
        const bool forceRenderer = wanted && std::strcmp(wanted, "renderer") == 0;
        const bool forceGpu = wanted && std::strcmp(wanted, "gpu") == 0;
        if (wanted && !forceRenderer && !forceGpu)
            std::fprintf(stderr, "[host] PS2X_PRESENT=%s is not gpu or renderer, ignored\n", wanted);
        if (!forceRenderer)
        {
            std::string failure;
            if (gpu_present::create(g_video.window, scale, failure))
            {
                g_video.gpu = true;
            }
            else if (forceGpu)
            {
                std::fprintf(stderr, "[host] PS2X_PRESENT=gpu but the SDL_GPU presenter could not start: %s\n", failure.c_str());
                destroyWindow();
                return false;
            }
            else
            {
                std::printf("[host] SDL_GPU is not available (%s); presenting through SDL_Renderer instead\n", failure.c_str());
                std::fflush(stdout);
            }
        }
        std::string presenterText;
        if (g_video.gpu)
        {
            presenterText = gpu_present::describe();
        }
        else
        {
            g_video.renderer = SDL_CreateRenderer(g_video.window, nullptr);
            if (!g_video.renderer)
            {
                std::fprintf(stderr, "[host] could not create the renderer: %s\n", SDL_GetError());
                destroyWindow();
                return false;
            }
            SDL_SetRenderVSync(g_video.renderer, 0); // the host loop paces itself at 60 Hz
            g_video.picture = SDL_CreateTexture(g_video.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                                kPictureWidth, kPictureHeight);
            if (!g_video.picture)
            {
                std::fprintf(stderr, "[host] could not create the picture texture: %s\n", SDL_GetError());
                destroyWindow();
                return false;
            }
            // The old build drew the picture with alpha blending over a black clear.
            SDL_SetTextureBlendMode(g_video.picture, SDL_BLENDMODE_BLEND);
            SDL_ScaleMode mode = SDL_SCALEMODE_PIXELART;
            if (scale == ScaleMode::Nearest)
                mode = SDL_SCALEMODE_NEAREST;
            else if (scale == ScaleMode::Bilinear)
                mode = SDL_SCALEMODE_LINEAR;
            if (!SDL_SetTextureScaleMode(g_video.picture, mode))
            {
                // Same fallback as the old shader: smooth scaling if the sharp mode is unavailable.
                std::fprintf(stderr, "[host] scale mode not available (%s), using bilinear\n", SDL_GetError());
                SDL_SetTextureScaleMode(g_video.picture, SDL_SCALEMODE_LINEAR);
            }
            presenterText = std::string("renderer ") + SDL_GetRendererName(g_video.renderer);
        }
        {
            const char *stats = std::getenv("PS2X_PRESENT_STATS");
            g_presentStats.on = stats && stats[0] == '1';
        }
        loadPictureHooks();

        int windowW = 0, windowH = 0, pixelW = 0, pixelH = 0;
        SDL_GetWindowSize(g_video.window, &windowW, &windowH);
        if (g_video.gpu)
            SDL_GetWindowSizeInPixels(g_video.window, &pixelW, &pixelH);
        else
            SDL_GetCurrentRenderOutputSize(g_video.renderer, &pixelW, &pixelH);
        std::printf("[host] SDL %d.%d.%d, video driver %s, presenter %s, window %dx%d (render output %dx%d), scale %s\n",
                    SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION, SDL_GetCurrentVideoDriver(),
                    presenterText.c_str(), windowW, windowH, pixelW, pixelH,
                    scale == ScaleMode::Sharp ? "sharp (SDL_SCALEMODE_PIXELART)" : (scale == ScaleMode::Nearest ? "nearest" : "bilinear"));
        std::fflush(stdout);
        return true;
    }

    void destroyWindow()
    {
        // The GPU device releases its claim on the window, so it goes first.
        if (g_video.gpu || gpu_present::active())
            gpu_present::destroy();
        if (g_video.picture)
            SDL_DestroyTexture(g_video.picture);
        if (g_video.renderer)
            SDL_DestroyRenderer(g_video.renderer);
        if (g_video.window)
            SDL_DestroyWindow(g_video.window);
        g_video.gpu = false;
        g_video.picture = nullptr;
        g_video.renderer = nullptr;
        g_video.window = nullptr;
        for (const OpenPad &pad : g_openPads)
            SDL_CloseGamepad(pad.gamepad);
        g_openPads.clear();
        g_activePad = 0;
        g_pad.present.store(false);
        if (g_video.padSubsystemStarted)
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        if (g_video.sdlVideoStarted)
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        g_video.padSubsystemStarted = false;
        g_video.sdlVideoStarted = false;
    }

    bool pumpEvents()
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (auto hook = g_eventHook.load())
                hook(&event);
            handleEvent(event);
        }
        return !g_quitRequested.load();
    }

    void updatePicture(const void *rgbaPixels, int pitchBytes)
    {
        if (g_pictureHooks.fixed)
        {
            rgbaPixels = g_pictureHooks.fixedPixels.data();
            pitchBytes = kPictureWidth * 4;
        }
        else if (!g_pictureHooks.dumpPath.empty() && rgbaPixels)
        {
            // Keep a copy of what is being shown, for maybeDumpPicture().
            g_pictureHooks.lastPixels.resize(static_cast<size_t>(kPictureWidth) * kPictureHeight * 4u);
            for (int y = 0; y < kPictureHeight; ++y)
                std::memcpy(g_pictureHooks.lastPixels.data() + static_cast<size_t>(y) * kPictureWidth * 4u,
                            static_cast<const uint8_t *>(rgbaPixels) + static_cast<size_t>(y) * static_cast<size_t>(pitchBytes),
                            static_cast<size_t>(kPictureWidth) * 4u);
            g_pictureHooks.havePixels = true;
        }
        ScopedPhase timing(g_presentStats.update);
        if (g_video.gpu)
            gpu_present::stagePicture(rgbaPixels, pitchBytes);
        else if (g_video.picture && rgbaPixels)
            SDL_UpdateTexture(g_video.picture, nullptr, rgbaPixels, pitchBytes);
    }

    void drawPicture(int srcWidth, int srcHeight, float aspect)
    {
        if (g_pictureHooks.fixed && !gpu_present::hasPictureSource())
        {
            srcWidth = g_pictureHooks.fixedWidth;
            srcHeight = g_pictureHooks.fixedHeight;
            aspect = g_pictureHooks.fixedAspect;
        }
        else
        {
            maybeDumpPicture(srcWidth, srcHeight, aspect);
        }
        ScopedPhase timing(g_presentStats.draw);
        if (g_video.gpu)
        {
            // The GPU presenter draws everything in presentAndPace(), after the debug panel has said what it
            // wants on top; here it only learns where the box goes.
            gpu_present::setPictureBox(srcWidth, srcHeight, aspect);
            return;
        }
        if (!g_video.renderer || !g_video.picture)
            return;
        SDL_SetRenderDrawColor(g_video.renderer, 0, 0, 0, 255);
        SDL_RenderClear(g_video.renderer);

        int outputW = 0, outputH = 0;
        SDL_GetCurrentRenderOutputSize(g_video.renderer, &outputW, &outputH);
        const float screenWidth = static_cast<float>(outputW);
        const float screenHeight = static_cast<float>(outputH);
        const float sourceWidth = static_cast<float>(std::max(1, srcWidth));
        const float sourceHeight = static_cast<float>(std::max(1, srcHeight));
        const float shape = aspect > 0.0f ? aspect : sourceWidth / sourceHeight;
        const float dstWidth = std::min(screenWidth, screenHeight * shape);
        const float dstHeight = dstWidth / shape;
        const SDL_FRect source{0.0f, 0.0f, sourceWidth, sourceHeight};
        const SDL_FRect target{(screenWidth - dstWidth) * 0.5f, (screenHeight - dstHeight) * 0.5f, dstWidth, dstHeight};
        SDL_RenderTexture(g_video.renderer, g_video.picture, &source, &target);
    }

    void presentAndPace()
    {
        {
            // Test hook (opt-in): PS2X_TEST_MAIN_STALL_MS=N makes the main thread sleep N ms every 120th host frame, to
            // show that nothing the game thread does waits for the main thread (the GS GPU renderer's direct path
            // hands pictures over without a wait; see gs_gpu_backend.cpp, "Field ring").
            static const int stallMs = []()
            {
                const char *value = std::getenv("PS2X_TEST_MAIN_STALL_MS");
                return value ? std::atoi(value) : 0;
            }();
            static uint64_t stallFrames = 0;
            if (stallMs > 0 && ++stallFrames % 120u == 0)
                SDL_Delay(static_cast<Uint32>(stallMs));
        }
        {
            ScopedPhase timing(g_presentStats.present);
            if (g_video.gpu)
                gpu_present::present();
            else if (g_video.renderer)
                SDL_RenderPresent(g_video.renderer);
        }
        writePendingDump();
        reportStats();
        pace();
    }

    void *nativeWindow()
    {
        return g_video.window;
    }

    void *nativeRenderer()
    {
        return g_video.renderer; // null while the SDL_GPU presenter is in use; see gpuDevice() in ps2_host_gpu.h
    }

    void setEventHook(void (*hook)(const void *))
    {
        g_eventHook.store(hook);
    }

    // ------------------------------------------------------------------- audio

    bool audioReady()
    {
        return g_audio.started;
    }

    bool startAudioStream(AudioMixFn mix, void *user)
    {
        if (g_audio.started)
            return true;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            std::fprintf(stderr, "[host] SDL audio failed to start (the game will be silent): %s\n", SDL_GetError());
            return false;
        }

        // Ask for the old build's 1440-frame period. Some backends (ALSA, PulseAudio) honour it; WASAPI
        // keeps to the engine's own period (10 ms here), and the queue below makes up the difference.
        // SDL_AUDIO_DEVICE_SAMPLE_FRAMES in the environment overrides this.
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1440");

        g_audio.mix = mix;
        g_audio.mixUser = user;
        const char *stats = std::getenv("PS2X_AUDIO_STATS");
        g_audio.statsOn = stats && stats[0] == '1';

        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_S16;
        spec.channels = 2;
        spec.freq = 48000;
        g_audio.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audioCallback, nullptr);
        if (!g_audio.stream)
        {
            std::fprintf(stderr, "[host] could not open an audio device (the game will be silent): %s\n", SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
        // The old build's audio library scaled every stereo stream by its centre-pan law,
        // 0.5 * 0.5 * (3 - 0.5 * 0.5) = 0.6875 (about -3.3 dB), and the SPU2 output reaches full scale
        // (peak 32766 in a match), so playing it unscaled would be 1.45 times louder than before.
        // PS2X_AUDIO_GAIN overrides the factor. The PS2X_AUDIO_STATS numbers are taken before it.
        float gain = 0.6875f;
        if (const char *env = std::getenv("PS2X_AUDIO_GAIN"))
            gain = std::clamp(static_cast<float>(std::atof(env)), 0.0f, 4.0f);
        SDL_SetAudioStreamGain(g_audio.stream, gain);
        g_audio.gain = gain;
        const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(g_audio.stream);
        SDL_AudioSpec deviceSpec{};
        int deviceFrames = 0;
        SDL_GetAudioDeviceFormat(device, &deviceSpec, &deviceFrames);
        const char *deviceName = SDL_GetAudioDeviceName(device);
        int totalFrames = kTotalBufferFrames;
        if (const char *env = std::getenv("PS2X_AUDIO_BUFFER_FRAMES"))
            totalFrames = std::clamp(std::atoi(env), 0, 48000);
        g_audio.queueTargetBytes = std::max(0, totalFrames - deviceFrames) * 2 * static_cast<int>(sizeof(int16_t));
        const int queueFrames = g_audio.queueTargetBytes / 4;
        std::printf("[host] audio: driver %s, device \"%s\", stream S16 2ch 48000 Hz, device format %d ch %d Hz, "
                    "device period %d frames (%.1f ms) + our queue %d frames = %d frames (%.1f ms) buffered, gain %.4f\n",
                    SDL_GetCurrentAudioDriver(), deviceName ? deviceName : "?", deviceSpec.channels, deviceSpec.freq,
                    deviceFrames, deviceSpec.freq ? 1000.0 * deviceFrames / deviceSpec.freq : 0.0, queueFrames,
                    deviceFrames + queueFrames, deviceSpec.freq ? 1000.0 * (deviceFrames + queueFrames) / deviceSpec.freq : 0.0,
                    static_cast<double>(gain));
        std::fflush(stdout);
        SDL_ResumeAudioStreamDevice(g_audio.stream);
        g_audio.started = true;
        return true;
    }

    void stopAudioStream()
    {
        if (!g_audio.started)
            return;
        g_audio.started = false;
        if (g_audio.stream)
        {
            SDL_DestroyAudioStream(g_audio.stream); // also stops the callback
            g_audio.stream = nullptr;
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
} // namespace ps2x_host
