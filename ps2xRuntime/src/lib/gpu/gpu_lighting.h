#pragma once

// Lights for the match scene, part one (task lt2-L0a; the design is docs/hd-lighting-tier2.md in the game's repository).
//
// A light needs to know, for a point on the screen, where that point is in the arena and which way its surface faces. The GPU
// renderer only receives triangles that the game has already projected onto the screen. This file holds the three things that
// get the arena back, with no SDL and no GPU in it, so that all of it can be unit tested:
//
//   1. The camera of the frame. The game's code tells the runtime the matrix it projected the scene with (CameraChannel, fed
//      through ps2x_gs::sceneCamera and ps2x_gs::sceneKickBegin / sceneKickEnd, which the game's overrides call).
//   2. Undoing the projection (Unprojector). Every scene vertex arrives with its screen x and y and with Q = 1 / w, the
//      reciprocal of its distance term. With the camera matrix that is three equations in the three unknowns of the world
//      position; the Z of the vertex is not used (the game biases it by layer, so it is no distance).
//   3. The light types and how one light adds up at a point (Light, Look, lightAt): the reference the shaders will follow.
//
// Nothing in the renderer uses this yet (the backend starts to in milestone L0b).
//
// ---- How a camera stays with its frame
// The game builds a frame's drawing commands into a list while it runs the frame, and at the end of the frame hands the list to
// the graphics chip in one go (the "kick", game function 0x12E328, called from the frame-end function 0x136DB0). In this
// runtime the kick is synchronous: the write to the DMA register walks the list, the GS frontend decodes it and calls the
// renderer's Submit for every primitive before the write returns, all on the thread that made the kick. So:
//   - the game's camera function posts the matrix when it has rebuilt it (several times a frame is fine, the last one wins);
//   - the kick is bracketed by kickBegin / kickEnd. kickBegin takes the posted camera if it belongs to this frame (same frame
//     buffer, frame counter equal or one more) and makes it the active camera of the calling thread;
//   - the renderer asks activeCamera() while it handles a primitive. It gets a camera only on the kicking thread and only
//     between kickBegin and kickEnd, which is exactly the set of primitives of the frame that camera drew;
//   - kickEnd drops both the active and the posted camera, so a camera can never be used for a later frame.
// No guess about field boundaries or draw order is involved, and a frame without a camera (menus) has no active camera.

#include <cstdint>
#include <string>
#include <vector>

namespace ps2x_lighting
{
    // ---- the camera of a frame
    struct Camera
    {
        // The game's matrix, four rows of four: (X, Y, Z, W) = (x, y, z, 1) * M, with a point as a row vector, so m[r * 4 + c]
        // is the coefficient of world axis r (row 3: the constant) in output c. Screen x = X / W and y = Y / W, in GS units
        // (pixels, with the game's 2048 centre still in them).
        float m[16] = {};
        uint32_t gameFrame = 0; // the game's frame counter when the matrix was built
        uint32_t buffer = 0;    // which of the game's two frame buffers the frame is drawn into (0 or 1)
        uint64_t serial = 0;    // 1, 2, 3, ... in posting order
    };

    struct ChannelStats
    {
        uint64_t posted = 0;         // cameras posted by the game
        uint64_t kicks = 0;          // kicks bracketed
        uint64_t kicksWithCamera = 0;
        uint64_t replaced = 0;       // a posted camera replaced by a later one before any kick (several rebuilds in one frame)
        uint64_t stale = 0;          // a posted camera that did not belong to the kick that followed (dropped, not used)
        uint64_t kicksWithPackets = 0; // kicks during which the GS received at least one packet (told by kickEnd)
    };

    // Thread-safe. One instance serves the game (channel()); tests make their own.
    class CameraChannel
    {
    public:
        CameraChannel();
        ~CameraChannel();
        CameraChannel(const CameraChannel &) = delete;
        CameraChannel &operator=(const CameraChannel &) = delete;

        // The game thread, when the game has rebuilt its camera matrix.
        void post(const float m[16], uint32_t gameFrame, uint32_t buffer);
        // The game thread, around the hand-over of the frame's drawing commands. `packets` is how many GIF packets the runtime
        // took in during the kick (for the trace and the statistics; it proves the kick is synchronous).
        void kickBegin(uint32_t gameFrame, uint32_t buffer);
        void kickEnd(uint64_t packets);
        // The renderer, while it handles a primitive: the camera that primitive was projected with, if there is one.
        bool activeCamera(Camera &out) const;

        ChannelStats stats() const;
        // One CSV row per post and per kick into `path` (empty: stop). For proofs; off unless asked for.
        bool traceTo(const std::string &path);

    private:
        struct Impl;
        Impl *m_impl;
    };

    // ---- the camera side file of a GS recording (task lt2-L0b)
    // A GS recording (docs/formats/gs-recording.md) holds the primitives but not the camera they were projected with. When the
    // game records a clip it also writes "<clip>.camera", a text file that says, by the number of the Submit record (0 for the
    // first Submit of the clip), where a camera becomes active and where it stops:
    //     PS2XCAM 1
    //     on <submit> <game frame> <frame buffer> <16 numbers: the matrix>
    //     off <submit>
    // The replayer plays those rows into a channel just before the Submit they name, so the renderer finds the same camera
    // active for the same primitives as it did in the game. The recording format itself is unchanged (version 1).
    struct CameraFileRow
    {
        uint64_t submit = 0;
        bool on = false;
        uint32_t gameFrame = 0, buffer = 0;
        float m[16] = {};
    };

    class CameraFileWriter
    {
    public:
        ~CameraFileWriter();
        bool open(const std::string &path);
        bool isOpen() const { return m_file != nullptr; }
        // Before Submit number `submit` is recorded: writes a row if what `channel` has active for this thread changed.
        void note(uint64_t submit, const CameraChannel &channel);
        void close();

    private:
        void *m_file = nullptr; // FILE *
        uint64_t m_serial = 0;  // the active camera at the last note (0: none)
    };

    bool loadCameraFile(const std::string &path, std::vector<CameraFileRow> &rows, std::string *error = nullptr);

    class CameraFilePlayer
    {
    public:
        explicit CameraFilePlayer(std::vector<CameraFileRow> rows) : m_rows(std::move(rows)) {}
        // Call before playing Submit number `submit` (0, 1, 2, ... in file order), on the thread that will call the renderer.
        void beforeSubmit(uint64_t submit, CameraChannel &channel);
        // After the last record: ends a camera that is still active.
        void finish(CameraChannel &channel);
        size_t rows() const { return m_rows.size(); }
        uint64_t camerasPlayed() const { return m_played; }

    private:
        std::vector<CameraFileRow> m_rows;
        size_t m_next = 0;
        bool m_active = false;
        uint64_t m_played = 0;
    };

    // For the game's recorder (GSCpuBackend::Submit, with its mutex held): Submit number `submit` of the clip at `clipPath` is
    // about to be recorded. Opens "<clipPath>.camera" on the first call for a clip.
    void recordingSubmit(const std::string &clipPath, uint64_t submit);

    // For the replayer (it declares these itself): load a side file and play it into the game's channel around its Submit records.
    bool replayCamerasLoad(const std::string &path, std::string *error);
    void replayCamerasBeforeSubmit(uint64_t submit);
    void replayCamerasFinish();
    uint64_t replayCamerasPlayed();

    // The game's channel. The first use reads PS2X_LIGHT_TRACE=<file> (the trace above) and PS2X_LIGHT_STATS=1 (a line on
    // stderr every 600 kicks).
    CameraChannel &channel();

    // ---- undoing the projection
    struct Unprojector
    {
        bool valid = false;
        double inverse[3][3] = {}; // of the 3 x 3 system below
        double constant[3] = {};   // the constants of X, Y and W (matrix row 3, columns 0, 1 and 3)
        double eye[3] = {};        // the centre of projection, in world units

        // False (valid == false) for a matrix that cannot be inverted.
        static Unprojector from(const float m[16]);

        // x, y: the vertex's screen position in GS units as the packet gives it (the 12.4 value / 16, before OFX/OFY is taken
        // off). q: the vertex's Q. False for a q of 0 or an invalid unprojector.
        bool worldAt(float x, float y, float q, float out[3]) const;
        bool worldAt(float x, float y, float q, double out[3]) const;
    };

    // The unit normal of triangle a, b, c (world positions), turned so that it faces the eye. The game draws only the side of a
    // surface that faces the camera, and code-built strips alternate their winding, so "towards the eye" is the rule for all.
    // False for a triangle with no area.
    bool faceNormal(const float a[3], const float b[3], const float c[3], const double eye[3], float out[3]);

    // ---- lights
    enum class LightType : uint8_t
    {
        Point,
        Spot
    };

    // How much a surface's direction matters.
    enum class Facing : uint8_t
    {
        Lambert,     // max(N.L, 0): ordinary surfaces
        Half,        // (N.L * 0.5 + 0.5)^2: softer, for cloth and the crowd
        PositionOnly // 1: wrestler cards and ropes, whose own normal is sideways
    };

    struct Light
    {
        LightType type = LightType::Point;
        float position[3] = {0.0f, 0.0f, 0.0f}; // world units: the ring is +-400, the mat is at y 100
        float target[3] = {0.0f, 0.0f, 0.0f};   // Spot: the point it aims at
        float colour[3] = {1.0f, 1.0f, 1.0f};
        float strength = 1.0f;
        float radius = 1000.0f;   // no light beyond this distance
        float innerDegrees = 20.0f; // Spot: full strength inside this half angle
        float outerDegrees = 30.0f; // Spot: nothing outside this half angle
    };

    // A look: what multiplies the game's own colours. Ambient 1 and no lights leaves the picture as the game drew it.
    struct Look
    {
        float ambient[3] = {1.0f, 1.0f, 1.0f};
        std::vector<Light> lights;
    };

    // What one light adds at a point (not multiplied by the surface colour): colour x strength x falloff x cone x facing.
    void lightAt(const Light &light, const float position[3], const float normal[3], Facing facing, float out[3]);
    // The whole look at a point: ambient plus every light.
    void lookAt(const Look &look, const float position[3], const float normal[3], Facing facing, float out[3]);

    // ---- which rule a primitive gets (task lt2-L1). One table, so that later steps (materials by texture, L3) extend it in one place.
    // What the renderer knows about a scene primitive when it decides (GSGpuBackend::Impl::submit).
    struct PrimitiveKind
    {
        bool strip = false;     // a triangle strip (the game draws wrestler parts, their shadows and ring-side sprites as strips;
                                // models, the mat and the ropes are triangle lists)
        bool textured = false;
        uint32_t texW = 0, texH = 0; // the declared texture size
        uint32_t texPsm = 0;         // its GS pixel format (0 is 32-bit colour)
        bool flatBlack = false;      // all three corners are black: the wrestlers' shadows (black, alpha 64)
    };

    struct ObjectRule
    {
        const char *name;   // for logs and the debug panel
        bool lit;           // false: the primitive is drawn as the game draws it and leaves the light buffer alone
        Facing facing;      // how its direction counts when it is lit
    };

    // The rules, in the order they are tried:
    //   shadow     a flat black strip. Not lit: it keeps darkening what is under it, and the light under it stays (decision 5).
    //   card       any other strip: a wrestler part or a ring-side sprite, an upright card. By position only.
    //   rope       a triangle list with a 32 x 32 32-bit texture: flat ribbons whose normal is sideways. By position only.
    //   surface    every other triangle list: models (hall, crowd, posts, apron) and the mat. By its face normal.
    const ObjectRule &ruleFor(const PrimitiveKind &kind);

    // ---- the look for now (task lt2-L1): ambient and one spotlight above the ring centre, pointing down, from the environment.
    //   PS2X_LIGHT=1                    lights on (anything else, or unset: off)
    //   PS2X_LIGHT_AMBIENT=0.6          or r,g,b: what the game's colours are multiplied by before any light (1 = as the game drew them)
    //   PS2X_LIGHT_SPOT_INTENSITY=1.5   0 turns the spotlight off
    //   PS2X_LIGHT_SPOT_COLOUR=1,0.96,0.88
    //   PS2X_LIGHT_SPOT_CONE=22,34      inner and outer half angle, degrees
    //   PS2X_LIGHT_SPOT_HEIGHT=900      how far above the floor (the mat is at 100)
    //   PS2X_LIGHT_SPOT_RADIUS=2000     no light beyond this distance
    // Settings, a panel and look files replace this in L2. Returns whether the lights are on; `look` gets the values either way.
    bool lookFromEnvironment(Look &look);

    // The uniform block of the lit shaders (shaders/gs_light.hlsli, cbuffer Light): ambient and the look's first spotlight.
    struct LightUniforms
    {
        float ambient[4];
        float spotPosition[4]; // xyz, radius
        float spotAxis[4];     // unit direction, strength
        float spotColour[4];
        float spotCone[4];     // cos(outer), cos(inner), the facing rule (0 Lambert, 1 half, 2 position only), unused
    };
    LightUniforms lightUniforms(const Look &look, Facing facing);
}

// What the game's overrides call (they declare these themselves, as they do for ps2x_gs::widePadRight: no header that
// generated code includes knows about them).
namespace ps2x_gs
{
    void sceneCamera(const float *matrix16, uint32_t gameFrame, uint32_t buffer);
    void sceneKickBegin(uint32_t gameFrame, uint32_t buffer);
    void sceneKickEnd(uint64_t packets);
}
