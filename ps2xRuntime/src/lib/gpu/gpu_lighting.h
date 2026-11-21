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

#include <atomic>
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
        bool on = true;             // a light that is off adds nothing (the debug panel's switch; kept in the look file)
    };

    // ---- materials by texture (task lt2-L3). The object rules above decide by what a primitive is; a material decides by which
    // texture it draws with, and wins over the rule. The texture is named by its key, the k3_i key of the texture tools
    // (docs/formats/fprmod.md): 16 hex digits, the same in every run. A small table is built in (builtInMaterials); a look file adds
    // entries or replaces built-in ones with lines like
    //     material 0123456789abcdef unlit
    //     material 0123456789abcdef normal strength 0.4
    //     material 0123456789abcdef glow 1.5
    enum class MaterialKind : uint8_t
    {
        Keep,         // not lit at all: drawn as the game draws it, and the light under it stays (what shadows get)
        Unlit,        // shown as the game drew it whatever the lights do (the light at its pixels becomes 1)
        PositionOnly, // lit by where it is
        FaceNormal,   // lit by its face normal (what surfaces get)
        Soft,         // lit by its face normal with the softer rule (Facing::Half)
        Glow          // a light of its own: shown at 1 + glow times its colour, whatever the lights do (a screen, a lamp)
    };

    struct Material
    {
        MaterialKind kind = MaterialKind::FaceNormal;
        float strength = 1.0f; // PositionOnly, FaceNormal, Soft: how much of the lighting it takes; 0 is the same as Unlit
        float glow = 0.0f;     // Glow: how far above its own colour
    };

    struct MaterialEntry
    {
        uint64_t key = 0;
        Material material;
        std::string note; // what the texture is, for people
    };
    constexpr size_t kMaxMaterials = 256;


    // ---- wrestler shape (task lt3-T1; docs/hd-lighting-tier3.md in the game's repository). A wrestler is flat pictures ("cards"), so
    // the lights can only tint him by where he stands. With these values above 0 the renderer also takes the outline of the whole
    // wrestler on the screen, lets the surface lean outward near it as a rounded body would, and lights that (wrestlerShapeAt below).
    // The built-in values, which a look file without these lines gets too:
    constexpr float kWrestlerShape = 1.0f;   // how much of the shaped light replaces today's tint by position (0 = today's picture)
    constexpr float kWrestlerRim = 0.8f;     // how strong the extra edge of light is on the side of the outline that faces a light
    constexpr float kWrestlerOutline = 2.4f; // how wide the rounded edge is, in texels of the wrestler's own pictures (the blur's sigma)
    constexpr float kWrestlerLean = 0.92f;   // how far the surface leans at the very outline (1 would be edge-on)
    constexpr float kWrestlerWrap = 0.5f;    // how far a light reaches past the edge of what faces it

    // ---- generated shape for wrestlers (task lt3-T3). Every wrestler part picture ("tile") gets a normal map made from the picture
    // itself the first time it is drawn (generateTileNormals below): a rounded bulge inward from the tile's cut-out, and a little of
    // the painted light and dark taken as height. The card shader reads it, turns it by the way the tile lies on the screen
    // (rotated, mirrored, upside down) and the shape pass adds it to the lean of the outline (wrestlerTileLean, wrestlerShapeAt).
    // Where a tile's edge lies well inside the wrestler (a joint, a part tucked under another) the bulge is faded out, so that
    // joints show no crease. The built-in values, which a look file without these lines gets too:
    constexpr float kWrestlerNormals = 0.6f; // how much of the generated map's lean is added (0 = the outline alone, T1's picture exactly)
    constexpr float kWrestlerPaint = 0.35f;  // how much of the painted light and dark counts as height
    constexpr float kWrestlerCrease = 1.0f;  // how far the bulge is faded where a tile's edge lies inside the wrestler (0 = not at all)
    constexpr bool kWrestlerSmooth = false;  // false: the map is read per texel (the light steps with the pixel art); true: smoothly
    constexpr float kTileBulgeRadius = 5.0f; // the bulge reaches this many texels in from the tile's cut-out; flat beyond
    constexpr float kTilePaintRange = 2.0f;  // the stored paint slope is clamped to this (height units per texel)
    constexpr float kCreaseStart = 1.0f;     // the bulge starts to fade when the wrestler's outline is this many texels further away than
    constexpr float kCreaseEnd = 3.0f;       // the tile's own edge, and is gone at this many
    constexpr float kWrestlerLeanMost = 0.97f; // the outline's lean and the map's together never tip the surface further than this
    constexpr uint32_t kTileNormalMaxSide = 128; // textures larger than this get no map (they are not body tiles)

    // A look: what multiplies the game's own colours. Ambient 1 and no lights leaves the picture as the game drew it.
    struct Look
    {
        float ambient[3] = {1.0f, 1.0f, 1.0f};
        std::vector<Light> lights;
        std::vector<MaterialEntry> materials; // per texture (below); the built-in table applies as well
        // Wrestler shape (above). Look file lines "wrestler_shape <n>", "wrestler_rim <n>", "wrestler_outline <n>".
        float wrestlerShape = kWrestlerShape;     // 0 to 1
        float wrestlerRim = kWrestlerRim;         // 0 to 4
        float wrestlerOutline = kWrestlerOutline; // 0.5 to 8 texels
        // Generated shape (above). Look file lines "wrestler_normals <n>", "wrestler_paint <n>", "wrestler_crease <n>", "wrestler_smooth 0|1".
        float wrestlerNormals = kWrestlerNormals; // 0 to 1.5
        float wrestlerPaint = kWrestlerPaint;     // 0 to 2
        float wrestlerCrease = kWrestlerCrease;   // 0 to 1
        bool wrestlerSmooth = kWrestlerSmooth;
    };

    // True when the look asks for wrestler shape at all. With both strengths at 0 the renderer does none of the work and the lit
    // picture is exactly the one of tier 2.
    inline bool wrestlerShapeWanted(const Look &look)
    {
        return look.wrestlerShape > 0.0f || look.wrestlerRim > 0.0f;
    }

    // True when the look asks for the generated normal maps as well (task lt3-T3). They only change the shaped light, so they need
    // wrestler_shape above 0. When false, no map is made and the wrestlers are drawn and lit exactly as T1 does it.
    inline bool wrestlerNormalsWanted(const Look &look)
    {
        return look.wrestlerShape > 0.0f && look.wrestlerNormals > 0.0f;
    }

    // What one light adds at a point (not multiplied by the surface colour): colour x strength x falloff x cone x facing.
    void lightAt(const Light &light, const float position[3], const float normal[3], Facing facing, float out[3]);
    // The whole look at a point: ambient plus every light.
    void lookAt(const Look &look, const float position[3], const float normal[3], Facing facing, float out[3]);

    // What the shape pass adds to the light of one wrestler pixel (the reference for shaders/postfx_rim.frag.hlsl; not multiplied by
    // the pixel's share). `position` is the pixel's place in the arena and `eye` the camera's; `outX`, `outY` say which way the
    // wrestler's outline lies on the screen (a unit vector, y down the screen; 0, 0 when there is none) and `lean` how far the surface
    // tips that way (0 well inside the wrestler, kWrestlerLean on his outline). `rightSign` is +1 when cross(up, view direction)
    // points to the right of the screen and -1 when to the left (the renderer works it out from the camera).
    //   N       the camera's view of a flat-on surface, tipped by `lean` towards (outX, outY) in the screen's plane
    //   shaped  ambient + sum of light x wrap(N.L) x gain, wrap(x) = clamp((x + 1/2) / (3/2)); the gain is 1 / wrap of a flat-on
    //           surface under the look's first light that is on, so that surface keeps exactly today's brightness
    //   rim     sum of light x max(N2.L, 0) x lean^2, N2 the outward direction lying in the screen's plane
    //   out     wrestlerShape x (shaped - today's light by position) + wrestlerRim x rim
    // (task lt3-T3) `tileLean`, when given, is what the generated normal map adds at the pixel (wrestlerTileLean below; x to the right
    // of the screen, y down it): N is then tipped by (outX, outY) x lean + tileLean, held to kWrestlerLeanMost. The rim is the
    // outline's alone. With no tileLean, or (0, 0), the result is T1's.
    void wrestlerShapeAt(const Look &look, const float position[3], const double eye[3], float outX, float outY, float lean, float rightSign, float out[3],
                         const float *tileLean = nullptr);
    // What the generated map of a wrestler tile adds to the lean of the surface at one pixel (the reference for the same lines of
    // shaders/postfx_rim.hlsli). `bulge` is the map's rounded-edge normal (its x and y, already turned to the screen: x right, y
    // down), `paint` the slope of the painted light and dark, `blurredMask` the blurred wrestler mask at the pixel (1/2 on the
    // outline, 1 well inside) and `outline` the blur's width in wrestler texels (Look::wrestlerOutline).
    //   dTile   how far the pixel is from its own tile's cut-out: kTileBulgeRadius x (1 - |bulge|)
    //   dOut    how far it is from the wrestler's outline, from the blurred mask: outline x the inverse of the normal distribution
    //   fade    1 - wrestlerCrease x smoothstep(kCreaseStart, kCreaseEnd, dOut - dTile): the tile's edge is inside the wrestler
    //   slope   bulge as a slope x fade + wrestlerPaint x paint;  out = wrestlerNormals x slope / sqrt(1 + |slope|^2)
    // (task lt3-T2) `edgeKept` is 1 where the shape pass found, just past the part's edge, a part of the same wrestler that is not
    // joined to this one in the body (partsJoined, from the game's tag): the fade is then not applied. 0 gives the rule above.
    void wrestlerTileLean(const Look &look, const float bulge[2], const float paint[2], float blurredMask, float out[2], float edgeKept = 0.0f);
    // The normal map of one wrestler tile (task lt3-T3). `rgba` is the tile as the renderer decodes it (w x h texels, r | g << 8 |
    // b << 16 | a << 24, alpha in the GS's scale: 128 is opaque); a texel is part of the figure when its alpha is at least
    // `alphaRef`. Everything outside the w x h texels counts as empty. `out` gets w x h x 4 bytes, per texel:
    //   0, 1   the bulge's normal, x and y in the texture's own axes (+x right, +y down): 128 + 127 x value
    //   2, 3   the lean the painted light and dark gives, x and y (minus the slope of its height): 128 + 127 x value / kTilePaintRange
    // The bulge: height sqrt(R^2 - (R - d)^2) for a texel d texels inside the cut-out (d = distance to the nearest empty texel's
    // centre - 1/2, held to R = kTileBulgeRadius), 0 outside, smoothed over 3 x 3 texels; its normal is (-dh/dx, -dh/dy, 1) made
    // unit length. The paint: the square root of the texel's linear brightness x R as a height, smoothed twice over the 3 x 3
    // texels that are part of the figure; its slope is taken only between texels of the figure, so the cut-out adds no cliff.
    void generateTileNormals(const uint32_t *rgba, uint32_t w, uint32_t h, uint32_t alphaRef, uint8_t *out);
    // PS2X_LIGHT_SHAPE=<shape>[,<rim>[,<outline>]] (a test aid): fixes those values whatever the look says. True when it is set.
    bool wrestlerShapeFromEnvironment(Look &look);
    // PS2X_LIGHT_NORMALS=<weight>[,<paint>[,<crease>[,<smooth 0|1>]]] (a test aid): the same for the generated shape.
    bool wrestlerNormalsFromEnvironment(Look &look);

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
        bool depthWriteOff = false;  // drawn without writing depth: the game's effects (billboards from aikoh/all.eff)
    };

    struct ObjectRule
    {
        const char *name;   // for logs and the debug panel
        bool lit;           // false: the primitive is drawn as the game draws it and leaves the light buffer alone
        Facing facing;      // how its direction counts when it is lit
    };

    // The rules, in the order they are tried:
    //   effect     anything drawn without writing depth: the effect billboards. Shown as the game drew them (they are light, or
    //              dust and sweat the game already shaded), blended into the light as they are into the picture.
    //   shadow     a flat black strip. Not lit: it keeps darkening what is under it, and the light under it stays (decision 5).
    //   card       any other strip: a wrestler part or a ring-side sprite, an upright card. By position only.
    //   rope       a triangle list with a 32 x 32 32-bit texture: flat ribbons whose normal is sideways. By position only.
    //   surface    every other triangle list: models (hall, crowd, posts, apron) and the mat. By its face normal.
    const ObjectRule &ruleFor(const PrimitiveKind &kind);

    // ---- materials: the table and the choice for a primitive (the types are above, with the look)
    const std::vector<MaterialEntry> &builtInMaterials();
    // The material of a texture key in a look: the look's own entry, else the built-in one, else null.
    const Material *materialFor(const Look &look, uint64_t key);
    const char *materialKindName(MaterialKind kind);

    // What the renderer does with one primitive: the material of its texture if it has one, else the object rule.
    struct LightChoice
    {
        bool lit = false;     // false: a plain draw that leaves the light buffer alone
        uint8_t mode = 0;     // 0 Lambert, 1 half, 2 position only (the Facing values), 3 a fixed light of 1 + glow
        float strength = 1.0f;
        float glow = 0.0f;
        const char *name = ""; // the rule's or the material kind's name, for logs
    };
    LightChoice chooseLight(const PrimitiveKind &kind, const Material *material);

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

    // The uniform block of the lit shaders (shaders/gs_light.hlsli, cbuffer Light): ambient and up to kMaxLights lights that are on.
    constexpr int kMaxLights = 8;
    struct LightUniforms
    {
        float ambient[4];                // rgb, the number of lights that follow
        float rule[4];                   // x: the draw's mode (0 Lambert, 1 half, 2 position only, 3 a fixed light of 1 + glow), y: 1 = draw the
                                         // markers ("show lights"), z: how much of the lighting the draw takes (1 = all), w: its glow
                                         // (a wrestler card drawn with shape, mode 2: w is its mask channel, 1 to 4; gs_card.hlsli)
        float position[kMaxLights][4];   // xyz, range
        float axis[kMaxLights][4];       // spot: the unit direction it points in; w: intensity
        float colour[kMaxLights][4];     // rgb, w: 1 spot, 0 point
        float cone[kMaxLights][4];       // cos(outer), cos(inner), w: the light's number in the look (for the marker's colour)
    };
    LightUniforms lightUniforms(const Look &look, Facing facing, bool showLights = false);

    // ---- the look as text (task lt2-L2). One file per arena; the units are the game's: the ring is 800 x 800 around (0, 0), the mat
    // is at height 100, the post tops at 255, the hall reaches about 2000 from the centre.
    //     look 1
    //     ambient 0.6 0.6 0.6
    //     light spot on
    //       position 0 900 0
    //       aim 0 100 0
    //       colour 1 0.97 0.9
    //       intensity 0.9
    //       cone 24 38
    //       range 2000
    //     wrestler_shape 1          (optional, with wrestler_rim and wrestler_outline: the shape of the wrestlers, see Look)
    //     wrestler_normals 0.6      (optional, with wrestler_paint, wrestler_crease and wrestler_smooth 0|1: the generated shape, see Look)
    // `#` starts a comment. A light is `light spot|point [on|off]` and the lines after it; every field is optional. Numbers out of range
    // are clamped. Anything else (no `look 1` line, an unknown word, a missing or broken number, more than 8 lights) makes the whole
    // text bad: parseLook returns false with a reason and leaves `look` alone.
    std::string formatLook(const Look &look);
    bool parseLook(const std::string &text, Look &look, std::string *error = nullptr);
    bool loadLookFile(const std::string &path, Look &look, std::string *error = nullptr);
    bool saveLookFile(const std::string &path, const Look &look, std::string *error = nullptr);

    // The built-in look: the hall a little down and one soft warm spotlight over the ring.
    Look defaultLook();
    // Starting points for the debug panel: "centre spot" (the default), "four corner spots", "two colours", "house lights".
    const std::vector<std::string> &presetNames();
    Look presetLook(const std::string &name);

    // ---- the current look (thread-safe; the renderer reads it at each VBlank, the debug panel edits it)
    // Whether the lights are wanted: the setting `light` (environment PS2X_LIGHT), read again when a setting changes.
    bool lightsWanted();
    // Counts every change of the current look or of "show lights"; the renderer copies the look again when it differs.
    uint64_t lookGeneration();
    Look currentLook();
    void setCurrentLook(const Look &look);
    bool showLights();
    void setShowLights(bool show);
    // The arena being played (the game tells: ps2x_gs::sceneArena, from the match loader 0x17DFA0), -1 before the first match.
    // A change of arena loads that arena's user look if there is one (a bad file: the built-in look and a log line), else the built-in.
    int currentArena();
    void setArena(int arena);
    // Where the user's look for an arena is kept: <the settings file's folder>/lights/arenaNN.look. Empty when there is no settings file.
    std::string userLookPath(int arena);
    // The folder the user looks live in; for tests and tools (default: beside the settings file).
    void setLookFolder(const std::string &folder);
    // The debug panel's buttons. Save writes the current look for the current arena (arena 0 when none is known yet).
    bool saveUserLook(std::string *message);
    // Reset: the built-in look. Reload: the user's file for the current arena again (the built-in if there is none).
    void resetLook();
    bool reloadLook(std::string *message);
    // Where the current look came from, for the panel: "built-in", a path, "the environment", "edited".
    std::string lookSource();

    // ---- the tag on wrestler part quads (task lt3-T2; docs/hd-lighting-tier3.md in the game's repository, "Built so far: T2").
    // The renderer cannot tell from a part quad which wrestler it belongs to (two wrestlers in a hold stand in one plane) or which
    // body part it is. The game knows both, and says so in four bytes the picture never uses: the fog byte of each of the quad's four
    // corners (fog is never switched on; without the tag each byte is the low byte of the corner's w + 1). The game's side is
    // src/overrides/fpwr_sprites.cpp, which calls ps2x_gs::partTag below; the renderer reads the tag back per triangle.
    //
    // The four bytes, corner 0 to corner 3 of the strip:
    //   byte 0   a check of bytes 1 and 2 (partTagCheckFirst)
    //   byte 1   bits 0-3 the wrestler's number (0 to 8 in the game's own table; 8 is the referee), bits 4-7 the part class
    //   byte 2   bits 0-4 the part's order (the wrestler's first part quad of the frame is 0, the next 1 ...; 31 means 31 or later),
    //            bit 5 a flag: the figure is the referee, bits 6-7 "salt" (any value; see below)
    //   byte 3   another check of bytes 1 and 2 (partTagCheckLast)
    // A quad is two triangles, corners 0-1-2 and 1-2-3, and the renderer gets one triangle at a time: both hold bytes 1 and 2 and
    // one check byte, so each can be read alone (the first as check, data, data; the second as data, data, check).
    //
    // Telling a tag from the original bytes: the renderer works out the byte the game would have written from the corner's Q
    // (1 / w). A triangle whose three bytes are all within 2 of that is not tagged. The game's side picks the salt so that neither
    // triangle of a tagged quad can look like that, and so that the second triangle cannot be mistaken for a first one.
    struct PartTag
    {
        uint8_t wrestler = 0;  // 0 to 15
        uint8_t partClass = 0; // kPartHead ... kPartObject
        uint8_t order = 0;     // 0 to 31
        bool referee = false;
    };
    // The part classes: the game's own nine groups of body tiles (by image number; 0x142440 uses them for the wrestler's build),
    // and one more for an image that is not a body tile. The names are ours, from pictures (hd-lighting-tier3.md).
    enum PartClass : uint32_t
    {
        kPartClass0 = 0,
        kPartClassCount = 9, // 0 to 8 are body tiles
        kPartObject = 9,     // an image from the 512-wide sheet (image number 0x23E and up)
    };
    uint8_t partTagCheckFirst(uint8_t data1, uint8_t data2);
    uint8_t partTagCheckLast(uint8_t data1, uint8_t data2);
    // The game's side. `original` are the four fog bytes the game would write; `out` gets the tag's. False (and `out` = `original`)
    // when no salt gives bytes that are safe to tell from the original; the tests show that cannot happen.
    bool encodePartTag(const PartTag &tag, const uint8_t original[4], uint8_t out[4]);
    // The renderer's side: the three fog bytes and Q values of one strip triangle, in the strip's order. False: not tagged.
    bool decodePartTag(const uint8_t fog[3], const float q[3], PartTag &tag);
    // True when two part classes are joined in the body (head and chest, thigh and shin ...) or are the same class: where one's
    // rounded end lies over the other there is a joint, not a limb in front of the body. kPartsJoined in shaders/postfx_rim.hlsli
    // is the same table.
    bool partsJoined(uint32_t classA, uint32_t classB);

    // The colour of light number `index`'s markers ("show lights"): the same eight colours as shaders/gs_light.hlsli, MarkerColour.
    void markerColour(int index, float rgb[3]);

    // What the renderer did with the lights, for the panel (written by the GS thread).
    struct LightStatus
    {
        std::atomic<bool> live{false};          // the lights are being applied
        std::atomic<bool> unavailable{false};   // wanted, but the renderer cannot (no half-float targets, not the progressive direct path)
        std::atomic<uint32_t> litLastField{0};  // lit primitives in the last field copied out
        std::atomic<uint64_t> fieldsLit{0};
    };
    LightStatus &lightStatus();
}

// What the game's overrides call (they declare these themselves, as they do for ps2x_gs::widePadRight: no header that
// generated code includes knows about them).
namespace ps2x_gs
{
    void sceneCamera(const float *matrix16, uint32_t gameFrame, uint32_t buffer);
    void sceneKickBegin(uint32_t gameFrame, uint32_t buffer);
    void sceneKickEnd(uint64_t packets);
    // The match loader is about to load arena `arena` (0x17DFA0): the look of that arena becomes the current one.
    void sceneArena(int arena);
    // The tag of a wrestler part quad (task lt3-T2; ps2x_lighting::encodePartTag): `original` are the four fog bytes the game would
    // write for the quad's corners, `out` gets the bytes to write instead. `flags` bit 0: the figure is the referee.
    bool partTag(uint32_t wrestler, uint32_t partClass, uint32_t order, uint32_t flags, const uint8_t original[4], uint8_t out[4]);
    // The reverse, for the game's own check of what it wrote (corner bytes and Q of one triangle). False: not a tag.
    bool partTagRead(const uint8_t fog[3], const float q[3], uint32_t &wrestler, uint32_t &partClass, uint32_t &order, uint32_t &flags);
}
