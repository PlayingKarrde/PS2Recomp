#pragma once

// Screen effects for the GPU renderer (task fx1; design and results in docs/hd-lighting.md, "Tier 1").
//
// What they are: bloom, colour grading, tilt-shift blur and a vignette, applied to the 3D picture of a match and nothing else.
// They are built in and off by default; the look (a preset and every value) is a setting, tuned live in the debug panel.
//
// How they are applied (gs_gpu_backend.cpp calls into this file in three places):
//   1. The game draws a match field as: clear, the 3D scene, then its 2D overlay (the clock, the pause panel, popups, names, fades).
//      The overlay is told apart by its Z: every scene primitive has a Z below 65536 and every overlay primitive a Z of 65536 or
//      more (docs/hd-lighting.md, "The boundary"). At the first overlay primitive of a field the scene is complete, and the backend
//      runs the passes below on the frame target there and then, into a texture of its own that has the size of the frame target.
//   2. Every overlay primitive of the field is then drawn a second time, into that texture, with the game's own pipeline (blend
//      equation, depth test, alpha test, texture): the overlay lands on the processed scene exactly as it landed on the original,
//      whatever its blend mode, and nothing of it is ever mixed with an effect. The game's own frame target gets the overlay as
//      always.
//   3. When the field is copied out for presentation, that texture is copied in place of the frame target's pixels.
//   The game's frame targets are never written by any pass here, so nothing the game later reads from them can see an effect.
// A field with no overlay at all (the introductions before the clock appears) is processed at the field copy instead. Fields that
// are not match scenes (menus, select, results, movies) are copied exactly as before, and with the effects off none of this code
// runs: the output is bit-identical.
//
// Depth is not used. The game's Z carries a per-mesh layer bias and is no distance (docs/formats/fpr-model.md), so the tilt-shift
// blur depends on the screen row only.
//
// Threads: the parameters are written from the UI and the debug panel and read by the GS thread once per field (`current()`);
// the Chain is used by the GS thread only.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct SDL_GPUCommandBuffer;
struct SDL_GPUDevice;
struct SDL_GPUTexture;

namespace ps2x_postfx
{
    // Every number the look is made of. Neutral values leave the picture as it is.
    struct Params
    {
        bool enabled = false;
        // bloom: what is brighter than the threshold (with a soft knee of that fraction of it) glows
        float bloomThreshold = 0.75f;
        float bloomKnee = 0.5f;
        float bloomIntensity = 0.0f;
        // colour grading
        float exposure = 0.0f;    // stops
        float contrast = 1.0f;
        float saturation = 1.0f;
        float temperature = 0.0f; // -1 cool, 1 warm
        float lift = 0.0f;
        float gamma = 1.0f;
        float gain = 1.0f;
        // tilt-shift: rows near the centre stay sharp; the picture blurs above and below by the screen row only
        float tiltCentre = 0.55f;   // 0..1 down the picture
        float tiltWidth = 0.35f;    // the sharp band, as a fraction of the picture's height
        float tiltFalloff = 0.30f;  // how far the blur takes to reach full strength, as a fraction of the height
        float tiltStrength = 0.0f;  // blur radius at full blur, in GS pixels
        // vignette
        float vignetteStrength = 0.0f;
        float vignetteRadius = 0.6f;   // 1 is the corners
        float vignetteSoftness = 0.6f;
    };

    // One row of the table the settings, presets and the debug panel's sliders share.
    struct Value
    {
        const char *name;  // "bloom_intensity": the setting is "fx_" + name, the environment variable PS2X_FX_ + the upper case name
        const char *label; // as the panel shows it
        float min, max, step;
        float Params::*member;
    };
    const std::vector<Value> &values();

    enum class Preset
    {
        Off,
        Subtle,
        Strong
    };
    const char *presetName(Preset preset);            // "off", "subtle", "strong"
    bool parsePreset(const std::string &text, Preset &out);
    // The values of a preset (enabled is false for Off).
    Params presetParams(Preset preset);

    // The parameters as the settings say (fx, fx_preset and any fx_<name> overrides, each from its environment variable first, then the
    // settings store) ...
    Params fromSettings();
    // ... and the live ones the GS thread uses: the settings', until the debug panel sets its own with setLive(); a later change of a
    // setting (the overlay, or fx_* set from code) takes the settings back. Any thread; cheap (one atomic load when nothing changed).
    Params current();
    void setLive(const Params &params);
    // The debug panel's "Save": writes every value that differs from the preset into the fx_<name> settings and saves the file.
    // Returns the number of settings written.
    int saveToSettings(const Params &params);
    // The environment variable name of a value ("PS2X_FX_BLOOM_INTENSITY").
    std::string envNameOf(const std::string &valueName);

    // What the effects did, for the debug panel (written by the GS thread, read from any thread).
    struct Status
    {
        std::atomic<uint64_t> fieldsWithEffects{0}; // fields run through the chain
        std::atomic<uint64_t> fieldsPlain{0};       // fields copied as before while the effects were on (menus, results, movies)
        std::atomic<int> lastMode{0};               // the last field: 0 plain, 1 effects with the overlay drawn over the processed scene, 2 effects, no overlay in the field
        std::atomic<bool> unavailable{false};       // asked for, but the renderer cannot do it (weave fields, no direct path, chain failed)
    };
    Status &status();

    // ---- the GPU side (GS thread only) ----

    // What one field needs.
    struct Job
    {
        SDL_GPUTexture *field = nullptr;     // the frame target at the end of the scene (a field with no overlay: as it is)
        uint32_t x = 0, y = 0, w = 0, h = 0; // the picture in it, in texels
        // Where the finished picture goes: a texture of the frame target's size (the picture at the same x, y), or null for a texture
        // of the chain's own that is just the picture's size (valid until the next call).
        SDL_GPUTexture *output = nullptr;
        float aspect = 4.0f / 3.0f;          // width over height as displayed
        float scale = 1.0f;                  // target pixels per GS pixel (the internal resolution)
        Params params;
    };

    class Chain
    {
    public:
        explicit Chain(SDL_GPUDevice *device);
        ~Chain();
        Chain(const Chain &) = delete;
        Chain &operator=(const Chain &) = delete;

        // False when a shader or a pipeline could not be made (the reason is logged); the backend then leaves the effects off.
        bool ready() const { return m_ready; }

        // Records the passes into `commandBuffer` (outside any pass) and returns the texture that holds the finished picture, or nullptr:
        // job.output (the picture at x, y), or a texture of the chain's own (the picture in its top-left w x h texels, valid until the next
        // call). `passLockHeld` says the caller already holds the validation-only render pass lock (gs_gpu_threads.h).
        SDL_GPUTexture *record(SDL_GPUCommandBuffer *commandBuffer, const Job &job, bool passLockHeld = false);

        // For measuring (PS2X_FX_TIME): runs the chain again in a command buffer of its own on an idle GPU, waits for it and returns the
        // time in microseconds (one submit and wait included), or a negative number if it could not be measured.
        double timeOnce(const Job &job);
        // The same wait with nothing recorded, to tell how much of timeOnce is the submit and wait itself.
        double timeEmpty();

        struct Stats
        {
            uint64_t fields = 0;        // fields the chain was recorded for
            uint64_t passes = 0;        // render passes in all of them
            uint64_t textureBytes = 0;  // GPU memory of the chain's own textures now
            uint32_t pictureW = 0, pictureH = 0;
        };
        Stats stats() const { return m_stats; }

    private:
        struct Impl;
        Impl *m_impl = nullptr;
        bool m_ready = false;
        Stats m_stats;
    };
}
