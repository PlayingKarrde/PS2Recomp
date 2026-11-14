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
}

// What the game's overrides call (they declare these themselves, as they do for ps2x_gs::widePadRight: no header that
// generated code includes knows about them).
namespace ps2x_gs
{
    void sceneCamera(const float *matrix16, uint32_t gameFrame, uint32_t buffer);
    void sceneKickBegin(uint32_t gameFrame, uint32_t buffer);
    void sceneKickEnd(uint64_t packets);
}
