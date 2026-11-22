// Lights for the match scene, part one: the camera channel, undoing the projection, the light types. See gpu_lighting.h.
#include "gpu/gpu_lighting.h"

#include "ps2_settings.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ps2x_lighting
{
    // ================================================================================================================
    // The camera channel
    // ================================================================================================================
    struct CameraChannel::Impl
    {
        mutable std::mutex mutex;
        bool hasPosted = false;
        Camera posted{};
        bool hasActive = false;
        Camera active{};
        std::thread::id kicker{};
        uint32_t kickFrame = 0, kickBuffer = 0;
        uint64_t serial = 0;
        ChannelStats stats{};
        std::FILE *trace = nullptr;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

        long long microseconds() const
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        }
        static unsigned long long threadNumber()
        {
            return static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFFFu);
        }
    };

    CameraChannel::CameraChannel() : m_impl(new Impl) {}

    CameraChannel::~CameraChannel()
    {
        if (m_impl->trace)
            std::fclose(m_impl->trace);
        delete m_impl;
    }

    void CameraChannel::post(const float m[16], uint32_t gameFrame, uint32_t buffer)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->hasPosted)
            ++m_impl->stats.replaced;
        std::memcpy(m_impl->posted.m, m, sizeof(m_impl->posted.m));
        m_impl->posted.gameFrame = gameFrame;
        m_impl->posted.buffer = buffer;
        m_impl->posted.serial = ++m_impl->serial;
        m_impl->hasPosted = true;
        ++m_impl->stats.posted;
        if (m_impl->trace)
            std::fprintf(m_impl->trace, "camera,%lld,%llu,%u,%u,%llu,,,%.9g,%.9g,%.9g,%.9g\n", m_impl->microseconds(), Impl::threadNumber(), gameFrame,
                         buffer, static_cast<unsigned long long>(m_impl->posted.serial), static_cast<double>(m[3]), static_cast<double>(m[7]),
                         static_cast<double>(m[11]), static_cast<double>(m[15]));
    }

    void CameraChannel::kickBegin(uint32_t gameFrame, uint32_t buffer)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        ++m_impl->stats.kicks;
        m_impl->hasActive = false;
        m_impl->kicker = std::this_thread::get_id();
        m_impl->kickFrame = gameFrame;
        m_impl->kickBuffer = buffer;
        if (m_impl->hasPosted)
        {
            // The camera belongs to this kick when it was built for the same frame buffer during the frame being handed over.
            // The game's frame counter goes up by one inside the frame-end function, before or after the kick depending on a
            // mode word, so "the same frame" is a difference of 0 or 1.
            const uint32_t age = gameFrame - m_impl->posted.gameFrame;
            if (m_impl->posted.buffer == buffer && age <= 1u)
            {
                m_impl->active = m_impl->posted;
                m_impl->hasActive = true;
                ++m_impl->stats.kicksWithCamera;
            }
            else
            {
                ++m_impl->stats.stale;
            }
        }
    }

    void CameraChannel::kickEnd(uint64_t packets)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (packets != 0u)
            ++m_impl->stats.kicksWithPackets;
        if (m_impl->trace)
            std::fprintf(m_impl->trace, "kick,%lld,%llu,%u,%u,%llu,%d,%llu,,,,\n", m_impl->microseconds(), Impl::threadNumber(), m_impl->kickFrame,
                         m_impl->kickBuffer, static_cast<unsigned long long>(m_impl->hasActive ? m_impl->active.serial : 0u), m_impl->hasActive ? 1 : 0,
                         static_cast<unsigned long long>(packets));
        m_impl->hasActive = false;
        m_impl->hasPosted = false; // used or not, it was this frame's: it must never reach a later frame
        m_impl->kicker = std::thread::id{};
    }

    bool CameraChannel::activeCamera(Camera &out) const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->hasActive || m_impl->kicker != std::this_thread::get_id())
            return false;
        out = m_impl->active;
        return true;
    }

    ChannelStats CameraChannel::stats() const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->stats;
    }

    bool CameraChannel::traceTo(const std::string &path)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->trace)
        {
            std::fclose(m_impl->trace);
            m_impl->trace = nullptr;
        }
        if (path.empty())
            return true;
        m_impl->trace = std::fopen(path.c_str(), "w");
        if (!m_impl->trace)
            return false;
        std::setvbuf(m_impl->trace, nullptr, _IOLBF, 1024);
        std::fprintf(m_impl->trace, "event,microseconds,thread,game_frame,buffer,camera_serial,kick_has_camera,kick_packets,m3,m7,m11,m15\n");
        return true;
    }

    namespace
    {
        bool g_statsOn = false;
    }

    CameraChannel &channel()
    {
        static CameraChannel instance;
        static std::once_flag once;
        std::call_once(once, [] {
            if (const char *path = std::getenv("PS2X_LIGHT_TRACE"))
            {
                if (*path && !instance.traceTo(path))
                    std::fprintf(stderr, "[light] PS2X_LIGHT_TRACE: cannot write %s\n", path);
            }
            const char *flag = std::getenv("PS2X_LIGHT_STATS");
            g_statsOn = flag && *flag && std::strcmp(flag, "0") != 0;
        });
        return instance;
    }

    // ================================================================================================================
    // The camera side file of a GS recording
    // ================================================================================================================
    CameraFileWriter::~CameraFileWriter() { close(); }

    bool CameraFileWriter::open(const std::string &path)
    {
        close();
        std::FILE *f = std::fopen(path.c_str(), "w");
        if (!f)
            return false;
        std::setvbuf(f, nullptr, _IOLBF, 1024);
        std::fprintf(f, "PS2XCAM 1\n");
        m_file = f;
        m_serial = 0;
        return true;
    }

    void CameraFileWriter::note(uint64_t submit, const CameraChannel &channel)
    {
        if (!m_file)
            return;
        std::FILE *f = static_cast<std::FILE *>(m_file);
        Camera camera;
        if (channel.activeCamera(camera))
        {
            if (camera.serial == m_serial)
                return;
            m_serial = camera.serial;
            std::fprintf(f, "on %llu %u %u", static_cast<unsigned long long>(submit), camera.gameFrame, camera.buffer);
            for (float v : camera.m)
                std::fprintf(f, " %.9g", static_cast<double>(v));
            std::fprintf(f, "\n");
        }
        else if (m_serial != 0u)
        {
            m_serial = 0;
            std::fprintf(f, "off %llu\n", static_cast<unsigned long long>(submit));
        }
    }

    void CameraFileWriter::close()
    {
        if (m_file)
            std::fclose(static_cast<std::FILE *>(m_file));
        m_file = nullptr;
    }

    bool loadCameraFile(const std::string &path, std::vector<CameraFileRow> &rows, std::string *error)
    {
        rows.clear();
        std::FILE *f = std::fopen(path.c_str(), "r");
        if (!f)
        {
            if (error)
                *error = "cannot open " + path;
            return false;
        }
        char line[1024];
        bool ok = std::fgets(line, sizeof(line), f) != nullptr && std::strncmp(line, "PS2XCAM 1", 9) == 0;
        if (!ok && error)
            *error = path + ": not a camera side file (the first line must be PS2XCAM 1)";
        uint64_t last = 0;
        int number = 1;
        while (ok && std::fgets(line, sizeof(line), f))
        {
            ++number;
            if (line[0] == '\n' || line[0] == '\r' || line[0] == '#')
                continue;
            CameraFileRow row;
            unsigned long long submit = 0;
            int used = 0;
            if (std::sscanf(line, "off %llu", &submit) == 1)
            {
                row.on = false;
            }
            else if (std::sscanf(line, "on %llu %u %u%n", &submit, &row.gameFrame, &row.buffer, &used) == 3)
            {
                row.on = true;
                const char *at = line + used;
                for (int i = 0; i < 16 && ok; ++i)
                {
                    char *end = nullptr;
                    row.m[i] = std::strtof(at, &end);
                    ok = end != at;
                    at = end;
                }
            }
            else
                ok = false;
            row.submit = submit;
            ok = ok && (rows.empty() || submit >= last);
            last = submit;
            if (ok)
                rows.push_back(row);
            else if (error)
                *error = path + ": line " + std::to_string(number) + " is not a camera row in order";
        }
        std::fclose(f);
        if (!ok)
            rows.clear();
        return ok;
    }

    void CameraFilePlayer::beforeSubmit(uint64_t submit, CameraChannel &channel)
    {
        while (m_next < m_rows.size() && m_rows[m_next].submit <= submit)
        {
            const CameraFileRow &row = m_rows[m_next++];
            if (m_active)
                channel.kickEnd(0u);
            m_active = false;
            if (row.on)
            {
                channel.post(row.m, row.gameFrame, row.buffer);
                channel.kickBegin(row.gameFrame, row.buffer);
                m_active = true;
                ++m_played;
            }
        }
    }

    void CameraFilePlayer::finish(CameraChannel &channel)
    {
        if (m_active)
            channel.kickEnd(0u);
        m_active = false;
    }

    void recordingSubmit(const std::string &clipPath, uint64_t submit)
    {
        static CameraFileWriter writer;
        static std::string openFor;
        if (openFor != clipPath)
        {
            openFor = clipPath;
            if (!writer.open(clipPath + ".camera"))
                std::fprintf(stderr, "[light] cannot write the camera side file %s.camera\n", clipPath.c_str());
        }
        writer.note(submit, channel());
    }

    namespace
    {
        std::unique_ptr<CameraFilePlayer> g_replayCameras;
    }

    bool replayCamerasLoad(const std::string &path, std::string *error)
    {
        std::vector<CameraFileRow> rows;
        if (!loadCameraFile(path, rows, error))
            return false;
        g_replayCameras = std::make_unique<CameraFilePlayer>(std::move(rows));
        return true;
    }

    void replayCamerasBeforeSubmit(uint64_t submit)
    {
        if (g_replayCameras)
            g_replayCameras->beforeSubmit(submit, channel());
    }

    void replayCamerasFinish()
    {
        if (g_replayCameras)
            g_replayCameras->finish(channel());
    }

    uint64_t replayCamerasPlayed() { return g_replayCameras ? g_replayCameras->camerasPlayed() : 0u; }

    // ================================================================================================================
    // Undoing the projection
    // ================================================================================================================
    Unprojector Unprojector::from(const float m[16])
    {
        Unprojector u;
        // X = x m0 + y m4 + z m8 + m12,  Y = x m1 + y m5 + z m9 + m13,  W = x m3 + y m7 + z m11 + m15.
        const double a[3][3] = {{m[0], m[4], m[8]}, {m[1], m[5], m[9]}, {m[3], m[7], m[11]}};
        u.constant[0] = m[12];
        u.constant[1] = m[13];
        u.constant[2] = m[15];
        const double c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
        const double c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
        const double c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
        const double det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
        double scale = 0.0;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                scale = std::max(scale, std::fabs(a[r][c]));
        if (!(std::fabs(det) > 1e-12 * scale * scale * scale) || !std::isfinite(det))
            return u;
        const double inv = 1.0 / det;
        u.inverse[0][0] = c00 * inv;
        u.inverse[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * inv;
        u.inverse[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * inv;
        u.inverse[1][0] = c01 * inv;
        u.inverse[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * inv;
        u.inverse[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * inv;
        u.inverse[2][0] = c02 * inv;
        u.inverse[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * inv;
        u.inverse[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * inv;
        // The centre of projection is where X, Y and W are all 0.
        for (int r = 0; r < 3; ++r)
            u.eye[r] = -(u.inverse[r][0] * u.constant[0] + u.inverse[r][1] * u.constant[1] + u.inverse[r][2] * u.constant[2]);
        u.valid = true;
        return u;
    }

    bool Unprojector::worldAt(float x, float y, float q, double out[3]) const
    {
        if (!valid || q == 0.0f)
            return false;
        const double w = 1.0 / static_cast<double>(q);
        const double rhs[3] = {static_cast<double>(x) * w - constant[0], static_cast<double>(y) * w - constant[1], w - constant[2]};
        for (int r = 0; r < 3; ++r)
            out[r] = inverse[r][0] * rhs[0] + inverse[r][1] * rhs[1] + inverse[r][2] * rhs[2];
        return true;
    }

    bool Unprojector::worldAt(float x, float y, float q, float out[3]) const
    {
        double p[3];
        if (!worldAt(x, y, q, p))
            return false;
        for (int r = 0; r < 3; ++r)
            out[r] = static_cast<float>(p[r]);
        return true;
    }

    bool faceNormal(const float a[3], const float b[3], const float c[3], const double eye[3], float out[3])
    {
        const double e1[3] = {static_cast<double>(b[0]) - a[0], static_cast<double>(b[1]) - a[1], static_cast<double>(b[2]) - a[2]};
        const double e2[3] = {static_cast<double>(c[0]) - a[0], static_cast<double>(c[1]) - a[1], static_cast<double>(c[2]) - a[2]};
        double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const double edges = (e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]) * (e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2]);
        if (!(length > 0.0) || !(length * length > 1e-12 * edges))
            return false;
        const double toEye[3] = {eye[0] - a[0], eye[1] - a[1], eye[2] - a[2]};
        const double side = n[0] * toEye[0] + n[1] * toEye[1] + n[2] * toEye[2];
        const double sign = side < 0.0 ? -1.0 : 1.0;
        for (int r = 0; r < 3; ++r)
            out[r] = static_cast<float>(sign * n[r] / length);
        return true;
    }

    // ================================================================================================================
    // Lights
    // ================================================================================================================
    namespace
    {
        float smoothstep(float edge0, float edge1, float x)
        {
            if (edge1 <= edge0)
                return x >= edge1 ? 1.0f : 0.0f;
            const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }
    }

    void lightAt(const Light &light, const float position[3], const float normal[3], Facing facing, float out[3])
    {
        out[0] = out[1] = out[2] = 0.0f;
        if (!light.on)
            return;
        const float toLight[3] = {light.position[0] - position[0], light.position[1] - position[1], light.position[2] - position[2]};
        const float distance = std::sqrt(toLight[0] * toLight[0] + toLight[1] * toLight[1] + toLight[2] * toLight[2]);
        if (!(light.radius > 0.0f) || distance >= light.radius)
            return;
        // Falloff: 1 at the light, 0 at the radius, smooth at both ends: (1 - (d / r)^2)^2.
        const float ratio = distance / light.radius;
        const float fall = (1.0f - ratio * ratio) * (1.0f - ratio * ratio);
        const float l[3] = {distance > 0.0f ? toLight[0] / distance : 0.0f, distance > 0.0f ? toLight[1] / distance : 1.0f,
                            distance > 0.0f ? toLight[2] / distance : 0.0f};

        float cone = 1.0f;
        if (light.type == LightType::Spot)
        {
            float axis[3] = {light.target[0] - light.position[0], light.target[1] - light.position[1], light.target[2] - light.position[2]};
            const float axisLength = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
            if (!(axisLength > 0.0f))
                return;
            // The angle between the spot's axis and the direction from the light to the point.
            const float cosAngle = -(axis[0] * l[0] + axis[1] * l[1] + axis[2] * l[2]) / axisLength;
            const float degrees = 3.14159265358979f / 180.0f;
            cone = smoothstep(std::cos(light.outerDegrees * degrees), std::cos(light.innerDegrees * degrees), cosAngle);
        }

        float face = 1.0f;
        if (facing != Facing::PositionOnly)
        {
            const float nl = normal[0] * l[0] + normal[1] * l[1] + normal[2] * l[2];
            if (facing == Facing::Lambert)
                face = std::max(nl, 0.0f);
            else
                face = (nl * 0.5f + 0.5f) * (nl * 0.5f + 0.5f);
        }
        const float amount = light.strength * fall * cone * face;
        for (int i = 0; i < 3; ++i)
            out[i] = light.colour[i] * amount;
    }

    void lookAt(const Look &look, const float position[3], const float normal[3], Facing facing, float out[3])
    {
        for (int i = 0; i < 3; ++i)
            out[i] = look.ambient[i];
        for (const Light &light : look.lights)
        {
            float add[3];
            lightAt(light, position, normal, facing, add);
            for (int i = 0; i < 3; ++i)
                out[i] += add[i];
        }
    }

    // ================================================================================================================
    // Object rules, the look from the environment, the shader's uniform block (task lt2-L1)
    // ================================================================================================================
    const ObjectRule &ruleFor(const PrimitiveKind &kind)
    {
        static const ObjectRule effect = {"effect", true, Facing::PositionOnly};
        static const ObjectRule shadow = {"shadow", false, Facing::PositionOnly};
        static const ObjectRule card = {"card", true, Facing::PositionOnly};
        static const ObjectRule rope = {"rope", true, Facing::PositionOnly};
        static const ObjectRule surface = {"surface", true, Facing::Lambert};
        if (kind.depthWriteOff)
            return effect;
        if (kind.strip)
            return kind.flatBlack ? shadow : card;
        if (kind.textured && kind.texW == 32u && kind.texH == 32u && kind.texPsm == 0u)
            return rope;
        return surface;
    }

    namespace
    {
        // Up to `count` numbers separated by commas; returns how many were read.
        int parseNumbers(const char *text, float *out, int count)
        {
            int n = 0;
            while (text && *text && n < count)
            {
                char *end = nullptr;
                const float v = std::strtof(text, &end);
                if (end == text)
                    break;
                out[n++] = v;
                text = end;
                while (*text == ',' || *text == ' ')
                    ++text;
            }
            return n;
        }
    }

    bool lookFromEnvironment(Look &look)
    {
        look = Look{};
        float ambient[3] = {0.6f, 0.6f, 0.6f};
        if (const char *text = std::getenv("PS2X_LIGHT_AMBIENT"))
        {
            float v[3];
            const int n = parseNumbers(text, v, 3);
            if (n == 1)
                ambient[0] = ambient[1] = ambient[2] = v[0];
            else if (n == 3)
                std::memcpy(ambient, v, sizeof(v));
        }
        for (int i = 0; i < 3; ++i)
            look.ambient[i] = std::clamp(ambient[i], 0.0f, 8.0f);

        Light spot;
        spot.type = LightType::Spot;
        float height = 900.0f, cone[2] = {22.0f, 34.0f}, colour[3] = {1.0f, 0.96f, 0.88f}, one = 0.0f;
        spot.strength = 1.5f;
        spot.radius = 2000.0f;
        if (const char *text = std::getenv("PS2X_LIGHT_SPOT_INTENSITY"))
            if (parseNumbers(text, &one, 1) == 1)
                spot.strength = std::clamp(one, 0.0f, 64.0f);
        if (const char *text = std::getenv("PS2X_LIGHT_SPOT_HEIGHT"))
            if (parseNumbers(text, &one, 1) == 1)
                height = std::clamp(one, 110.0f, 20000.0f);
        if (const char *text = std::getenv("PS2X_LIGHT_SPOT_RADIUS"))
            if (parseNumbers(text, &one, 1) == 1)
                spot.radius = std::clamp(one, 1.0f, 100000.0f);
        if (const char *text = std::getenv("PS2X_LIGHT_SPOT_COLOUR"))
        {
            float v[3];
            if (parseNumbers(text, v, 3) == 3)
                std::memcpy(colour, v, sizeof(v));
        }
        if (const char *text = std::getenv("PS2X_LIGHT_SPOT_CONE"))
        {
            float v[2];
            if (parseNumbers(text, v, 2) == 2)
                std::memcpy(cone, v, sizeof(v));
        }
        spot.position[0] = 0.0f;
        spot.position[1] = height;
        spot.position[2] = 0.0f;
        spot.target[0] = 0.0f;
        spot.target[1] = 100.0f; // the mat
        spot.target[2] = 0.0f;
        for (int i = 0; i < 3; ++i)
            spot.colour[i] = std::clamp(colour[i], 0.0f, 8.0f);
        spot.innerDegrees = std::clamp(cone[0], 0.0f, 89.0f);
        spot.outerDegrees = std::clamp(std::max(cone[1], spot.innerDegrees), 0.0f, 89.5f);
        look.lights.push_back(spot);

        const char *on = std::getenv("PS2X_LIGHT");
        return on && std::strcmp(on, "1") == 0;
    }

    // ================================================================================================================
    // Materials by texture key (task lt2-L3)
    // ================================================================================================================
    const char *materialKindName(MaterialKind kind)
    {
        switch (kind)
        {
        case MaterialKind::Keep: return "keep";
        case MaterialKind::Unlit: return "unlit";
        case MaterialKind::PositionOnly: return "position";
        case MaterialKind::FaceNormal: return "normal";
        case MaterialKind::Soft: return "soft";
        default: return "glow";
        }
    }

    const std::vector<MaterialEntry> &builtInMaterials()
    {
        // Chosen from evidence (docs/hd-lighting-tier2.md, "Built so far: L3"): textures that look wrong under the object rules alone.
        static const std::vector<MaterialEntry> table = {
            // Thin wire and netting are triangle lists on upright planes, so their face normal is sideways and a light from above gave
            // them nothing but the ambient level: a cage that stays dark in the middle of the spotlight. Wire is round and catches
            // light from any side, so these are lit by position, like the ropes.
            {0xa97f2e48c5a4cfd1ull, {MaterialKind::PositionOnly, 1.0f, 0.0f}, "cage netting (Cage Deathmatch, arena 8)"},
            {0xfa394369e7adee3eull, {MaterialKind::PositionOnly, 1.0f, 0.0f}, "cage frame (Cage Deathmatch, arena 8)"},
            {0x1af33ecad7e13ab0ull, {MaterialKind::PositionOnly, 1.0f, 0.0f}, "barbed wire (Barbed Wire Deathmatch, arena 9)"},
        };
        return table;
    }

    const Material *materialFor(const Look &look, uint64_t key)
    {
        if (key == 0u)
            return nullptr;
        for (const MaterialEntry &entry : look.materials)
            if (entry.key == key)
                return &entry.material;
        for (const MaterialEntry &entry : builtInMaterials())
            if (entry.key == key)
                return &entry.material;
        return nullptr;
    }

    LightChoice chooseLight(const PrimitiveKind &kind, const Material *material)
    {
        LightChoice choice;
        if (material)
        {
            choice.name = materialKindName(material->kind);
            choice.lit = material->kind != MaterialKind::Keep;
            choice.strength = std::clamp(material->strength, 0.0f, 4.0f);
            switch (material->kind)
            {
            case MaterialKind::Unlit:
                choice.mode = 3;
                break;
            case MaterialKind::Glow:
                choice.mode = 3;
                choice.glow = std::clamp(material->glow, 0.0f, 16.0f);
                break;
            case MaterialKind::PositionOnly:
                choice.mode = 2;
                break;
            case MaterialKind::Soft:
                choice.mode = 1;
                break;
            default:
                choice.mode = 0;
                break;
            }
            return choice;
        }
        const ObjectRule &rule = ruleFor(kind);
        choice.name = rule.name;
        choice.lit = rule.lit;
        if (std::strcmp(rule.name, "effect") == 0)
            choice.mode = 3; // shown as the game drew it
        else
            choice.mode = rule.facing == Facing::Lambert ? 0 : (rule.facing == Facing::Half ? 1 : 2);
        return choice;
    }

    void wrestlerShapeAt(const Look &look, const float position[3], const double eye[3], float outX, float outY, float lean, float rightSign, float out[3],
                         const float *tileLean)
    {
        out[0] = out[1] = out[2] = 0.0f;
        const auto dot = [](const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        const auto normalise = [&](float v[3]) {
            const float length = std::sqrt(dot(v, v));
            if (length > 0.0f)
                for (int i = 0; i < 3; ++i)
                    v[i] /= length;
            return length;
        };
        // The camera's axes at the pixel: V away from the eye, R to the right of the screen, U up the screen.
        float V[3] = {position[0] - static_cast<float>(eye[0]), position[1] - static_cast<float>(eye[1]), position[2] - static_cast<float>(eye[2])};
        normalise(V);
        float R[3] = {V[2], 0.0f, -V[0]}; // cross((0, 1, 0), V)
        if (!(normalise(R) > 1.0e-5f))
        {
            R[0] = 1.0f;
            R[1] = R[2] = 0.0f;
        }
        const float U[3] = {V[1] * R[2] - V[2] * R[1], V[2] * R[0] - V[0] * R[2], V[0] * R[1] - V[1] * R[0]}; // cross(V, R)
        for (int i = 0; i < 3; ++i)
            R[i] *= rightSign;
        const float sx = outX * lean, sy = outY * lean;
        // The surface's tip: the outline's alone (T1), or with what the tile's generated map adds (T3), held to kWrestlerLeanMost.
        float tx = sx, ty = sy, depth = std::sqrt(std::max(1.0f - lean * lean, 0.02f));
        if (tileLean && (tileLean[0] != 0.0f || tileLean[1] != 0.0f))
        {
            tx += tileLean[0];
            ty += tileLean[1];
            const float length = std::sqrt(tx * tx + ty * ty);
            if (length > kWrestlerLeanMost)
            {
                tx *= kWrestlerLeanMost / length;
                ty *= kWrestlerLeanMost / length;
            }
            depth = std::sqrt(std::max(1.0f - (tx * tx + ty * ty), 0.02f));
        }
        float N[3], N2[3], flat[3];
        for (int i = 0; i < 3; ++i)
        {
            N[i] = tx * R[i] - ty * U[i] - depth * V[i];
            N2[i] = sx * R[i] - sy * U[i] - 0.001f * V[i];
            flat[i] = -V[i];
        }
        normalise(N);
        normalise(N2);
        const auto wrap = [](float nl) { return std::clamp((nl + kWrestlerWrap) / (1.0f + kWrestlerWrap), 0.0f, 1.0f); };
        float today[3] = {}, shaped[3] = {}, rim[3] = {};
        float gain = 1.0f;
        bool first = true;
        for (const Light &light : look.lights)
        {
            // (the lights the shader's block holds: lightUniforms leaves the same ones out)
            if (!light.on || !(light.strength > 0.0f) || !(light.radius > 0.0f))
                continue;
            if (light.type == LightType::Spot && light.target[0] == light.position[0] && light.target[1] == light.position[1] && light.target[2] == light.position[2])
                continue;
            float l[3] = {light.position[0] - position[0], light.position[1] - position[1], light.position[2] - position[2]};
            if (!(normalise(l) > 0.0f))
            {
                l[0] = l[2] = 0.0f;
                l[1] = 1.0f;
            }
            if (first)
                gain = 1.0f / std::clamp(wrap(dot(flat, l)), 0.3f, 1.0f);
            first = false;
            float c[3];
            lightAt(light, position, flat, Facing::PositionOnly, c);
            const float face = wrap(dot(N, l)), edge = std::max(dot(N2, l), 0.0f);
            for (int i = 0; i < 3; ++i)
            {
                today[i] += c[i];
                shaped[i] += c[i] * face;
                rim[i] += c[i] * edge;
            }
        }
        for (int i = 0; i < 3; ++i)
            out[i] = look.wrestlerShape * (shaped[i] * gain - today[i]) + look.wrestlerRim * lean * lean * rim[i];
    }

    // The inverse of the normal distribution for the upper half: how many sigmas into a blurred edge the value b (1/2 on the edge, 1
    // far inside) lies. Through Winitzki's approximation of the inverse error function; held to about 3.9 sigmas.
    static float sigmasInside(float b)
    {
        const float x = std::clamp(b * 2.0f - 1.0f, 0.0f, 0.9999f);
        const float a = 0.147f;
        const float ln = std::log(1.0f - x * x);
        const float t = 2.0f / (3.14159265f * a) + ln * 0.5f;
        return 1.41421356f * std::sqrt(std::max(std::sqrt(std::max(t * t - ln / a, 0.0f)) - t, 0.0f));
    }

    void wrestlerTileLean(const Look &look, const float bulge[2], const float paint[2], float blurredMask, float out[2], float edgeKept)
    {
        const float length = std::min(std::sqrt(bulge[0] * bulge[0] + bulge[1] * bulge[1]), 1.0f);
        const float dTile = kTileBulgeRadius * (1.0f - length);
        const float dOut = look.wrestlerOutline * sigmasInside(blurredMask);
        const float x = std::clamp((dOut - dTile - kCreaseStart) / (kCreaseEnd - kCreaseStart), 0.0f, 1.0f);
        const float fade = std::max(1.0f - look.wrestlerCrease * (x * x * (3.0f - 2.0f * x)), edgeKept);
        const float toSlope = fade / std::sqrt(std::max(1.0f - length * length, 0.05f));
        const float sx = bulge[0] * toSlope + look.wrestlerPaint * paint[0];
        const float sy = bulge[1] * toSlope + look.wrestlerPaint * paint[1];
        const float k = look.wrestlerNormals / std::sqrt(1.0f + sx * sx + sy * sy);
        out[0] = sx * k;
        out[1] = sy * k;
    }

    // ---- the tag on wrestler part quads (task lt3-T2)
    namespace
    {
        // How far a fog byte is from another, round the 256 (0 to 128).
        inline uint32_t byteDistance(uint32_t a, uint32_t b)
        {
            const uint32_t d = (a - b) & 0xFFu;
            return d > 128u ? 256u - d : d;
        }
        // The fog byte the game writes for a corner that has no tag: the low byte of the whole part of w + 1 (the part emitter adds
        // (0, 0, bias, 1) to the projected corner before it turns it into the packet's numbers). The renderer has Q = 1 / w.
        // False when Q gives no w.
        inline bool untaggedByte(float q, uint32_t &byte)
        {
            if (!(q > 1.0e-12f) || !(q < 1.0e12f))
                return false;
            const double w = 1.0 / static_cast<double>(q) + 1.0;
            byte = static_cast<uint32_t>(static_cast<int64_t>(std::floor(w))) & 0xFFu;
            return true;
        }
        constexpr uint32_t kUntaggedReach = 2; // the renderer: a byte this close to the worked-out one is the game's own
        constexpr uint32_t kTaggedClear = 4;   // the game: a tag byte must be at least this far from the byte it replaces to count as clear of it

        inline PartTag tagFromData(uint8_t data1, uint8_t data2)
        {
            PartTag tag;
            tag.wrestler = data1 & 0x0Fu;
            tag.partClass = data1 >> 4;
            tag.order = data2 & 0x1Fu;
            tag.referee = (data2 & 0x20u) != 0u;
            return tag;
        }
    }

    uint8_t partTagCheckFirst(uint8_t data1, uint8_t data2)
    {
        uint32_t x = static_cast<uint32_t>(data1) * 167u + static_cast<uint32_t>(data2) * 89u + 0x5Bu;
        x ^= x >> 5;
        x = x * 0x2Du + (static_cast<uint32_t>(data1 ^ data2) >> 3);
        return static_cast<uint8_t>(x ^ (x >> 8));
    }

    uint8_t partTagCheckLast(uint8_t data1, uint8_t data2)
    {
        uint32_t x = static_cast<uint32_t>(data1) * 59u + static_cast<uint32_t>(data2) * 211u + 0xC6u;
        x ^= x >> 4;
        x = x * 0x35u + (static_cast<uint32_t>(data1) >> 2) + static_cast<uint32_t>(data2);
        return static_cast<uint8_t>(x ^ (x >> 8));
    }

    bool encodePartTag(const PartTag &tag, const uint8_t original[4], uint8_t out[4])
    {
        const uint8_t data1 = static_cast<uint8_t>((tag.wrestler & 0x0Fu) | ((tag.partClass & 0x0Fu) << 4));
        const uint8_t plain2 = static_cast<uint8_t>((tag.order > 31u ? 31u : tag.order) | (tag.referee ? 0x20u : 0u));
        for (uint32_t salt = 0; salt < 4u; ++salt)
        {
            const uint8_t data2 = static_cast<uint8_t>(plain2 | (salt << 6));
            const uint8_t bytes[4] = {partTagCheckFirst(data1, data2), data1, data2, partTagCheckLast(data1, data2)};
            // The second triangle (data, data, check) must not read as a first one (check, data, data).
            if (bytes[1] == partTagCheckFirst(bytes[2], bytes[3]))
                continue;
            // Neither triangle may have all three of its bytes near the bytes they replace.
            const auto near = [&](int i) { return byteDistance(bytes[i], original[i]) < kTaggedClear; };
            if ((near(0) && near(1) && near(2)) || (near(1) && near(2) && near(3)))
                continue;
            std::memcpy(out, bytes, 4);
            return true;
        }
        std::memcpy(out, original, 4);
        return false;
    }

    bool decodePartTag(const uint8_t fog[3], const float q[3], PartTag &tag)
    {
        // The game's own bytes?
        bool own = true;
        for (int i = 0; i < 3 && own; ++i)
        {
            uint32_t byte = 0;
            own = untaggedByte(q[i], byte) && byteDistance(fog[i], byte) <= kUntaggedReach;
        }
        if (own)
            return false;
        if (fog[0] == partTagCheckFirst(fog[1], fog[2]))
        {
            tag = tagFromData(fog[1], fog[2]);
            return true;
        }
        if (fog[2] == partTagCheckLast(fog[0], fog[1]))
        {
            tag = tagFromData(fog[0], fog[1]);
            return true;
        }
        return false;
    }

    // The game's nine classes of body tiles, as seen in the tag view (docs/hd-lighting-tier3.md, "Built so far: T2"):
    //   0 head, 1 chest, 2 waist, 3 upper arm, 4 forearm, 5 shin, 6 thigh, 7 hand, 8 foot.
    // PartsJoined in shaders/postfx_rim.hlsli must stay the same.
    bool partsJoined(uint32_t classA, uint32_t classB)
    {
        if (classA >= kPartClassCount || classB >= kPartClassCount)
            return true; // an object, or a class the table does not know: as before the tag (the bulge fades)
        if (classA == classB)
            return true;
        static const bool table[9][9] = {
            //  head   chest  waist  u.arm  f.arm  shin   thigh  hand   foot
            {true,  true,  false, false, false, false, false, false, false}, // head
            {true,  true,  true,  true,  false, false, false, false, false}, // chest
            {false, true,  true,  false, false, false, true,  false, false}, // waist
            {false, true,  false, true,  true,  false, false, false, false}, // upper arm
            {false, false, false, true,  true,  false, false, true,  false}, // forearm
            {false, false, false, false, false, true,  true,  false, true},  // shin
            {false, false, true,  false, false, true,  true,  false, false}, // thigh
            {false, false, false, false, true,  false, false, true,  false}, // hand
            {false, false, false, false, false, true,  false, false, true},  // foot
        };
        return table[classA][classB];
    }

    void generateTileNormals(const uint32_t *rgba, uint32_t w, uint32_t h, uint32_t alphaRef, uint8_t *out)
    {
        if (w == 0u || h == 0u)
            return;
        // Every array here has one more texel all round than the tile (index (y + 1) * pw + x + 1), so the loops need no edge cases:
        // the extra ring is empty for `inside` and the paint, and repeats the border texel for the bulge's height.
        const int W = static_cast<int>(w), H = static_cast<int>(h), pw = W + 2, ph = H + 2;
        const size_t count = static_cast<size_t>(pw) * static_cast<size_t>(ph);
        const float R = kTileBulgeRadius;
        const int reach = static_cast<int>(std::ceil(R + 0.5f)) + 1;
        // (kept between calls: a match makes thousands of maps, on the thread that hands the game's primitives to the renderer)
        static thread_local std::vector<uint8_t> insideStore;
        static thread_local std::vector<float> store;
        insideStore.assign(count, 0u);
        store.assign(count * 5u, 0.0f);
        uint8_t *const inside = insideStore.data();
        float *const column = store.data(), *const height = column + count, *const smooth = height + count, *const paintA = smooth + count, *const paintB = paintA + count;
        const uint32_t ref = std::max<uint32_t>(alphaRef, 1u);
        // The display encoding of a channel to linear light (the palette's colours are sRGB), by a table.
        static const std::array<float, 256> toLinear = [] {
            std::array<float, 256> table{};
            for (int i = 0; i < 256; ++i)
            {
                const float c = static_cast<float>(i) / 255.0f;
                table[static_cast<size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            }
            return table;
        }();
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const uint32_t c = rgba[static_cast<size_t>(y) * w + static_cast<size_t>(x)];
                if ((c >> 24) < ref)
                    continue;
                const size_t i = static_cast<size_t>(y + 1) * pw + static_cast<size_t>(x + 1);
                inside[i] = 1u;
                paintA[i] = std::sqrt(0.2126f * toLinear[c & 0xFFu] + 0.7152f * toLinear[(c >> 8) & 0xFFu] + 0.0722f * toLinear[(c >> 16) & 0xFFu]) * R;
            }
        // The bulge's height: from the distance to the nearest empty texel (anything outside the tile is empty). First the distance
        // up or down each column, then the nearest over the columns within reach: exact, and about 15 steps a texel.
        for (int x = 1; x <= W; ++x)
        {
            float run = 0.0f;
            for (int y = 1; y <= H; ++y)
            {
                run = inside[y * pw + x] ? run + 1.0f : 0.0f;
                column[y * pw + x] = run;
            }
            run = 0.0f;
            for (int y = H; y >= 1; --y)
            {
                run = inside[y * pw + x] ? run + 1.0f : 0.0f;
                column[y * pw + x] = std::min(column[y * pw + x], run);
            }
        }
        for (int y = 1; y <= H; ++y)
            for (int x = 1; x <= W; ++x)
            {
                if (!inside[y * pw + x])
                    continue;
                float best = static_cast<float>(reach * reach);
                const int lo = std::max(x - reach, 0), hi = std::min(x + reach, W + 1); // (columns 0 and W + 1 are empty: distance 0)
                const float *row = column + y * pw;
                for (int cx = lo; cx <= hi; ++cx)
                {
                    const float dx = static_cast<float>(cx - x), g = row[cx];
                    best = std::min(best, dx * dx + g * g);
                }
                const float d = std::min(std::max(std::sqrt(best) - 0.5f, 0.0f), R);
                height[y * pw + x] = std::sqrt(std::max(R * R - (R - d) * (R - d), 0.0f));
            }
        // Smoothed over 3 x 3 texels (the tile's border texels repeat outward, as the offline mock-up had it).
        const auto repeatBorder = [&](float *a) {
            for (int x = 1; x <= W; ++x)
            {
                a[x] = a[pw + x];
                a[(H + 1) * pw + x] = a[H * pw + x];
            }
            for (int y = 0; y < ph; ++y)
            {
                a[y * pw] = a[y * pw + 1];
                a[y * pw + W + 1] = a[y * pw + W];
            }
        };
        repeatBorder(height);
        for (int y = 1; y <= H; ++y)
            for (int x = 1; x <= W; ++x)
            {
                const float *c = height + y * pw + x;
                smooth[y * pw + x] = (c[-pw - 1] + c[-pw] + c[-pw + 1] + c[-1] + c[0] + c[1] + c[pw - 1] + c[pw] + c[pw + 1]) / 9.0f;
            }
        // The paint's height, smoothed twice over the texels of the figure only (empty texels hold 0 and are not counted).
        const auto smoothInside = [&](const float *from, float *to) {
            for (int y = 1; y <= H; ++y)
                for (int x = 1; x <= W; ++x)
                {
                    const size_t i = static_cast<size_t>(y * pw + x);
                    if (!inside[i])
                    {
                        to[i] = 0.0f;
                        continue;
                    }
                    const float *c = from + i;
                    const uint8_t *n = inside + i;
                    const int taps = n[-pw - 1] + n[-pw] + n[-pw + 1] + n[-1] + n[0] + n[1] + n[pw - 1] + n[pw] + n[pw + 1];
                    to[i] = (c[-pw - 1] + c[-pw] + c[-pw + 1] + c[-1] + c[0] + c[1] + c[pw - 1] + c[pw] + c[pw + 1]) / static_cast<float>(taps);
                }
        };
        smoothInside(paintA, paintB);
        smoothInside(paintB, paintA);
        const auto toByte = [](float v) { return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v * 127.0f)) + 128, 1, 255)); };
        for (int y = 1; y <= H; ++y)
            for (int x = 1; x <= W; ++x)
            {
                const size_t i = static_cast<size_t>(y * pw + x);
                // The bulge: central differences of the smoothed height (one-sided at the tile's border).
                const int xl = std::max(x - 1, 1), xr = std::min(x + 1, W), yu = std::max(y - 1, 1), yd = std::min(y + 1, H);
                const float gx = (smooth[y * pw + xr] - smooth[y * pw + xl]) / static_cast<float>(std::max(xr - xl, 1));
                const float gy = (smooth[yd * pw + x] - smooth[yu * pw + x]) / static_cast<float>(std::max(yd - yu, 1));
                const float inv = 1.0f / std::sqrt(gx * gx + gy * gy + 1.0f);
                // The paint: differences between texels of the figure only.
                float px = 0.0f, py = 0.0f;
                if (inside[i])
                {
                    const bool l = inside[i - 1] != 0u, r = inside[i + 1] != 0u, u = inside[i - pw] != 0u, d = inside[i + pw] != 0u;
                    const float here = paintA[i];
                    if (l || r)
                        px = ((r ? paintA[i + 1] : here) - (l ? paintA[i - 1] : here)) / ((l && r) ? 2.0f : 1.0f);
                    if (u || d)
                        py = ((d ? paintA[i + pw] : here) - (u ? paintA[i - pw] : here)) / ((u && d) ? 2.0f : 1.0f);
                }
                uint8_t *o = out + (static_cast<size_t>(y - 1) * w + static_cast<size_t>(x - 1)) * 4u;
                o[0] = toByte(-gx * inv);
                o[1] = toByte(-gy * inv);
                // (the paint is a height, so the surface leans away from where it rises: the stored value is minus the slope)
                o[2] = toByte(std::clamp(-px, -kTilePaintRange, kTilePaintRange) / kTilePaintRange);
                o[3] = toByte(std::clamp(-py, -kTilePaintRange, kTilePaintRange) / kTilePaintRange);
            }
    }

    void handNormalTexel(const uint8_t rgb[3], uint8_t out[4])
    {
        // Only x and y are read: z is whatever makes the normal unit length, so a map saved with two channels, or with a blue channel
        // that rounding spoilt, gives the same lean. A texel whose x and y are longer than 1 (an author's tool that did not normalize) is
        // shortened to length 1.
        float x = static_cast<float>(rgb[0]) / 127.5f - 1.0f, y = static_cast<float>(rgb[1]) / 127.5f - 1.0f;
        const float len = std::sqrt(x * x + y * y);
        if (len > 1.0f)
        {
            x /= len;
            y /= len;
        }
        const auto toByte = [](float v) { return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v * 127.0f)) + 128, 1, 255)); };
        out[0] = toByte(x);
        out[1] = toByte(-y); // the picture's +y is up, the generated maps' (and the card shader's) is down
        out[2] = 0u;
        out[3] = 0u;
    }

    void wrestlerHandLean(const Look &look, const float hand[2], float out[2])
    {
        out[0] = look.wrestlerNormals * hand[0];
        out[1] = look.wrestlerNormals * hand[1];
    }

    void generatedMapToPicture(const uint8_t *generated, uint32_t w, uint32_t h, float paintWeight, uint8_t *rgb)
    {
        const size_t count = static_cast<size_t>(w) * h;
        for (size_t i = 0; i < count; ++i)
        {
            const uint8_t *g = generated + i * 4u;
            const float bx = (static_cast<float>(g[0]) - 128.0f) / 127.0f, by = (static_cast<float>(g[1]) - 128.0f) / 127.0f;
            const float px = (static_cast<float>(g[2]) - 128.0f) / 127.0f * kTilePaintRange, py = (static_cast<float>(g[3]) - 128.0f) / 127.0f * kTilePaintRange;
            const float l = std::min(std::sqrt(bx * bx + by * by), 1.0f);
            const float k = 1.0f / std::sqrt(std::max(1.0f - l * l, 0.05f)); // (as the shape pass: the bulge as a slope)
            const float sx = bx * k + paintWeight * px, sy = by * k + paintWeight * py;
            const float inv = 1.0f / std::sqrt(1.0f + sx * sx + sy * sy);
            const float nx = sx * inv, ny = sy * inv, nz = inv; // y down, the generated maps' axes
            const auto toChannel = [](float v) { return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround((v * 0.5f + 0.5f) * 255.0f)), 0, 255)); };
            rgb[i * 3u + 0] = toChannel(nx);
            rgb[i * 3u + 1] = toChannel(-ny); // the picture's +y is up
            rgb[i * 3u + 2] = toChannel(nz);
        }
    }

    bool wrestlerNormalsFromEnvironment(Look &look)
    {
        const char *text = std::getenv("PS2X_LIGHT_NORMALS");
        if (!text || !*text)
            return false;
        float v[4] = {look.wrestlerNormals, look.wrestlerPaint, look.wrestlerCrease, look.wrestlerSmooth ? 1.0f : 0.0f};
        const int got = std::sscanf(text, "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
        if (got < 1)
            return false;
        look.wrestlerNormals = std::clamp(v[0], 0.0f, 1.5f);
        look.wrestlerPaint = std::clamp(v[1], 0.0f, 2.0f);
        look.wrestlerCrease = std::clamp(v[2], 0.0f, 1.0f);
        look.wrestlerSmooth = v[3] > 0.5f;
        return true;
    }

    bool wrestlerShapeFromEnvironment(Look &look)
    {
        const char *text = std::getenv("PS2X_LIGHT_SHAPE");
        if (!text || !*text)
            return false;
        float v[3] = {look.wrestlerShape, look.wrestlerRim, look.wrestlerOutline};
        const int got = std::sscanf(text, "%f,%f,%f", &v[0], &v[1], &v[2]);
        if (got < 1)
            return false;
        look.wrestlerShape = std::clamp(v[0], 0.0f, 1.0f);
        look.wrestlerRim = std::clamp(v[1], 0.0f, 4.0f);
        look.wrestlerOutline = std::clamp(v[2], 0.5f, 8.0f);
        return true;
    }

    // ================================================================================================================
    // The look as text, the built-in looks, the current look (task lt2-L2)
    // ================================================================================================================
    namespace
    {
        float clampNumber(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

        void clampLight(Light &l)
        {
            for (int i = 0; i < 3; ++i)
            {
                l.position[i] = clampNumber(l.position[i], -20000.0f, 20000.0f);
                l.target[i] = clampNumber(l.target[i], -20000.0f, 20000.0f);
                l.colour[i] = clampNumber(l.colour[i], 0.0f, 8.0f);
            }
            l.strength = clampNumber(l.strength, 0.0f, 64.0f);
            l.radius = clampNumber(l.radius, 1.0f, 100000.0f);
            l.innerDegrees = clampNumber(l.innerDegrees, 0.0f, 89.0f);
            l.outerDegrees = clampNumber(std::max(l.outerDegrees, l.innerDegrees), 0.0f, 89.5f);
        }

        Light spotLight(float x, float y, float z, float aimX, float aimY, float aimZ, float r, float g, float b, float strength, float inner, float outer,
                        float range)
        {
            Light l;
            l.type = LightType::Spot;
            l.position[0] = x; l.position[1] = y; l.position[2] = z;
            l.target[0] = aimX; l.target[1] = aimY; l.target[2] = aimZ;
            l.colour[0] = r; l.colour[1] = g; l.colour[2] = b;
            l.strength = strength;
            l.innerDegrees = inner;
            l.outerDegrees = outer;
            l.radius = range;
            return l;
        }
    }

    std::string formatLook(const Look &look)
    {
        char line[256];
        std::string out = "# Arena look: the lights of one arena (docs/hd-lighting-tier2.md). Units: the ring is 800 x 800 around (0, 0),\n"
                          "# the mat is at height 100, the post tops at 255. Edit it in the debug panel (F1, Lights) or by hand.\n"
                          "look 1\n";
        std::snprintf(line, sizeof(line), "ambient %.9g %.9g %.9g\n", look.ambient[0], look.ambient[1], look.ambient[2]);
        out += line;
        std::snprintf(line, sizeof(line), "wrestler_shape %.9g\nwrestler_rim %.9g\nwrestler_outline %.9g\n", look.wrestlerShape, look.wrestlerRim, look.wrestlerOutline);
        out += line;
        std::snprintf(line, sizeof(line), "wrestler_normals %.9g\nwrestler_paint %.9g\nwrestler_crease %.9g\nwrestler_smooth %d\n", look.wrestlerNormals,
                      look.wrestlerPaint, look.wrestlerCrease, look.wrestlerSmooth ? 1 : 0);
        out += line;
        for (const Light &l : look.lights)
        {
            std::snprintf(line, sizeof(line), "light %s %s\n", l.type == LightType::Spot ? "spot" : "point", l.on ? "on" : "off");
            out += line;
            std::snprintf(line, sizeof(line), "  position %.9g %.9g %.9g\n", l.position[0], l.position[1], l.position[2]);
            out += line;
            std::snprintf(line, sizeof(line), "  aim %.9g %.9g %.9g\n", l.target[0], l.target[1], l.target[2]);
            out += line;
            std::snprintf(line, sizeof(line), "  colour %.9g %.9g %.9g\n", l.colour[0], l.colour[1], l.colour[2]);
            out += line;
            std::snprintf(line, sizeof(line), "  intensity %.9g\n  cone %.9g %.9g\n  range %.9g\n", l.strength, l.innerDegrees, l.outerDegrees, l.radius);
            out += line;
        }
        for (const MaterialEntry &entry : look.materials)
        {
            std::snprintf(line, sizeof(line), "material %016llx %s", static_cast<unsigned long long>(entry.key), materialKindName(entry.material.kind));
            out += line;
            if (entry.material.kind == MaterialKind::Glow)
            {
                std::snprintf(line, sizeof(line), " %.9g", entry.material.glow);
                out += line;
            }
            else if (entry.material.strength != 1.0f && entry.material.kind != MaterialKind::Keep && entry.material.kind != MaterialKind::Unlit)
            {
                std::snprintf(line, sizeof(line), " strength %.9g", entry.material.strength);
                out += line;
            }
            if (!entry.note.empty())
                out += "   # " + entry.note;
            out += "\n";
        }
        return out;
    }

    bool parseLook(const std::string &text, Look &look, std::string *error)
    {
        Look parsed;
        parsed.ambient[0] = parsed.ambient[1] = parsed.ambient[2] = 1.0f;
        bool header = false;
        Light *light = nullptr;
        int number = 0;
        const auto fail = [&](const std::string &why) {
            if (error)
                *error = "line " + std::to_string(number) + ": " + why;
            return false;
        };
        size_t at = 0;
        while (at < text.size())
        {
            size_t end = text.find('\n', at);
            if (end == std::string::npos)
                end = text.size();
            std::string raw = text.substr(at, end - at);
            at = end + 1;
            ++number;
            const size_t hash = raw.find('#');
            std::string comment;
            if (hash != std::string::npos)
            {
                comment = raw.substr(hash + 1u);
                while (!comment.empty() && (comment.front() == ' ' || comment.front() == '\t'))
                    comment.erase(comment.begin());
                while (!comment.empty() && (comment.back() == '\r' || comment.back() == ' '))
                    comment.pop_back();
                raw.resize(hash);
            }
            // words
            std::vector<std::string> words;
            size_t i = 0;
            while (i < raw.size())
            {
                while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\r' || raw[i] == ','))
                    ++i;
                size_t j = i;
                while (j < raw.size() && raw[j] != ' ' && raw[j] != '\t' && raw[j] != '\r' && raw[j] != ',')
                    ++j;
                if (j > i)
                    words.push_back(raw.substr(i, j - i));
                i = j;
            }
            if (words.empty())
                continue;
            const std::string &word = words[0];
            float v[3] = {};
            const auto numbers = [&](size_t count) {
                if (words.size() != count + 1u)
                    return false;
                for (size_t k = 0; k < count; ++k)
                {
                    char *stop = nullptr;
                    v[k] = std::strtof(words[k + 1u].c_str(), &stop);
                    if (stop == words[k + 1u].c_str() || *stop != '\0' || !std::isfinite(v[k]))
                        return false;
                }
                return true;
            };
            if (!header)
            {
                if (word != "look" || words.size() != 2u || words[1] != "1")
                    return fail("the first line must be \"look 1\"");
                header = true;
                continue;
            }
            if (word == "ambient")
            {
                if (numbers(1))
                    v[1] = v[2] = v[0];
                else if (!numbers(3))
                    return fail("ambient takes one number or three");
                for (int k = 0; k < 3; ++k)
                    parsed.ambient[k] = clampNumber(v[k], 0.0f, 8.0f);
                light = nullptr;
            }
            else if (word == "wrestler_shape" || word == "wrestler_rim" || word == "wrestler_outline")
            {
                // The shape of the wrestlers (task lt3-T1). A file without these lines keeps the built-in values.
                if (!numbers(1))
                    return fail(word + " takes one number");
                if (word == "wrestler_shape")
                    parsed.wrestlerShape = clampNumber(v[0], 0.0f, 1.0f);
                else if (word == "wrestler_rim")
                    parsed.wrestlerRim = clampNumber(v[0], 0.0f, 4.0f);
                else
                    parsed.wrestlerOutline = clampNumber(v[0], 0.5f, 8.0f);
                light = nullptr;
            }
            else if (word == "wrestler_normals" || word == "wrestler_paint" || word == "wrestler_crease" || word == "wrestler_smooth")
            {
                // The generated shape of the wrestlers (task lt3-T3). A file without these lines keeps the built-in values.
                if (!numbers(1))
                    return fail(word + " takes one number");
                if (word == "wrestler_normals")
                    parsed.wrestlerNormals = clampNumber(v[0], 0.0f, 1.5f);
                else if (word == "wrestler_paint")
                    parsed.wrestlerPaint = clampNumber(v[0], 0.0f, 2.0f);
                else if (word == "wrestler_crease")
                    parsed.wrestlerCrease = clampNumber(v[0], 0.0f, 1.0f);
                else
                    parsed.wrestlerSmooth = v[0] > 0.5f;
                light = nullptr;
            }
            else if (word == "material")
            {
                // material <16 hex digits> keep|unlit|position|normal|soft [strength <n>]   or   material <key> glow <n>
                if (words.size() < 3u || words[1].size() != 16u)
                    return fail("a material is \"material <16 hex digits> keep|unlit|position|normal|soft|glow ...\"");
                char *stop = nullptr;
                MaterialEntry entry;
                entry.key = std::strtoull(words[1].c_str(), &stop, 16);
                if (*stop != '\0' || entry.key == 0u)
                    return fail("a material's key is 16 hex digits");
                const std::string &kind = words[2];
                size_t next = 3;
                const auto number = [&](float &to) {
                    if (next >= words.size())
                        return false;
                    char *end = nullptr;
                    to = std::strtof(words[next].c_str(), &end);
                    const bool good = end != words[next].c_str() && *end == '\0' && std::isfinite(to);
                    ++next;
                    return good;
                };
                if (kind == "keep")
                    entry.material.kind = MaterialKind::Keep;
                else if (kind == "unlit")
                    entry.material.kind = MaterialKind::Unlit;
                else if (kind == "position")
                    entry.material.kind = MaterialKind::PositionOnly;
                else if (kind == "normal")
                    entry.material.kind = MaterialKind::FaceNormal;
                else if (kind == "soft")
                    entry.material.kind = MaterialKind::Soft;
                else if (kind == "glow")
                {
                    entry.material.kind = MaterialKind::Glow;
                    if (!number(entry.material.glow))
                        return fail("glow takes a number");
                    entry.material.glow = clampNumber(entry.material.glow, 0.0f, 16.0f);
                }
                else
                    return fail("\"" + kind + "\" is not a kind of material");
                if (next < words.size())
                {
                    if (words[next] != "strength" || entry.material.kind == MaterialKind::Glow || entry.material.kind == MaterialKind::Keep ||
                        entry.material.kind == MaterialKind::Unlit)
                        return fail("only \"strength <n>\" may follow position, normal or soft");
                    ++next;
                    if (!number(entry.material.strength) || next != words.size())
                        return fail("strength takes one number");
                    entry.material.strength = clampNumber(entry.material.strength, 0.0f, 4.0f);
                }
                if (parsed.materials.size() >= kMaxMaterials)
                    return fail("more than 256 materials");
                entry.note = comment;
                bool replaced = false;
                for (MaterialEntry &have : parsed.materials)
                    if (have.key == entry.key)
                    {
                        have = entry; // a later line for the same texture wins
                        replaced = true;
                    }
                if (!replaced)
                    parsed.materials.push_back(entry);
                light = nullptr;
            }
            else if (word == "light")
            {
                if (words.size() < 2u || words.size() > 3u || (words[1] != "spot" && words[1] != "point") ||
                    (words.size() == 3u && words[2] != "on" && words[2] != "off"))
                    return fail("a light is \"light spot|point [on|off]\"");
                if (parsed.lights.size() >= static_cast<size_t>(kMaxLights))
                    return fail("more than 8 lights");
                Light l;
                l.type = words[1] == "spot" ? LightType::Spot : LightType::Point;
                l.on = words.size() < 3u || words[2] == "on";
                l.position[1] = 900.0f;
                l.target[1] = 100.0f;
                l.radius = 2000.0f;
                parsed.lights.push_back(l);
                light = &parsed.lights.back();
            }
            else if (word == "position" || word == "aim" || word == "colour" || word == "color" || word == "intensity" || word == "cone" || word == "range")
            {
                if (!light)
                    return fail("\"" + word + "\" before any light");
                if (word == "position" || word == "aim" || word == "colour" || word == "color")
                {
                    if (!numbers(3))
                        return fail(word + " takes three numbers");
                    float *to = word == "position" ? light->position : (word == "aim" ? light->target : light->colour);
                    std::memcpy(to, v, sizeof(v));
                }
                else if (word == "cone")
                {
                    if (!numbers(2))
                        return fail("cone takes two numbers (inner and outer half angle, degrees)");
                    light->innerDegrees = v[0];
                    light->outerDegrees = v[1];
                }
                else
                {
                    if (!numbers(1))
                        return fail(word + " takes one number");
                    (word == "intensity" ? light->strength : light->radius) = v[0];
                }
            }
            else
                return fail("\"" + word + "\" is not a word of a look file");
        }
        if (!header)
        {
            number = 1;
            return fail("the first line must be \"look 1\"");
        }
        for (Light &l : parsed.lights)
            clampLight(l);
        look = parsed;
        return true;
    }

    bool loadLookFile(const std::string &path, Look &look, std::string *error)
    {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
        {
            if (error)
                *error = "cannot open " + path;
            return false;
        }
        std::string text;
        char buffer[4096];
        size_t got;
        while ((got = std::fread(buffer, 1, sizeof(buffer), f)) != 0u && text.size() < (1u << 20))
            text.append(buffer, got);
        std::fclose(f);
        std::string why;
        if (!parseLook(text, look, &why))
        {
            if (error)
                *error = path + ": " + why;
            return false;
        }
        return true;
    }

    bool saveLookFile(const std::string &path, const Look &look, std::string *error)
    {
        std::error_code ec;
        const std::filesystem::path target(path);
        if (target.has_parent_path())
            std::filesystem::create_directories(target.parent_path(), ec);
        const std::string temporary = path + ".tmp";
        std::FILE *f = std::fopen(temporary.c_str(), "wb");
        const std::string text = formatLook(look);
        bool ok = f != nullptr && std::fwrite(text.data(), 1, text.size(), f) == text.size();
        if (f)
            ok = std::fclose(f) == 0 && ok;
        if (ok)
        {
            std::filesystem::rename(temporary, target, ec);
            ok = !ec;
        }
        if (!ok && error)
            *error = "cannot write " + path;
        return ok;
    }

    Look defaultLook()
    {
        Look look;
        look.ambient[0] = look.ambient[1] = look.ambient[2] = 0.62f;
        look.lights.push_back(spotLight(0.0f, 900.0f, 0.0f, 0.0f, 100.0f, 0.0f, 1.0f, 0.97f, 0.9f, 0.9f, 24.0f, 38.0f, 2000.0f));
        return look;
    }

    const std::vector<std::string> &presetNames()
    {
        static const std::vector<std::string> names = {"centre spot", "four corner spots", "two colours", "house lights"};
        return names;
    }

    Look presetLook(const std::string &name)
    {
        Look look = defaultLook();
        if (name == "four corner spots")
        {
            // A spotlight above each corner post, aimed at the middle of the mat: the cones overlap in the centre.
            look.ambient[0] = look.ambient[1] = look.ambient[2] = 0.55f;
            look.lights.clear();
            for (int corner = 0; corner < 4; ++corner)
            {
                const float x = (corner & 1) ? 450.0f : -450.0f, z = (corner & 2) ? 450.0f : -450.0f;
                look.lights.push_back(spotLight(x, 800.0f, z, 0.0f, 100.0f, 0.0f, 1.0f, 0.97f, 0.9f, 0.45f, 20.0f, 32.0f, 2200.0f));
            }
        }
        else if (name == "two colours")
        {
            // The hall dark, a white spot on the ring and a blue and a magenta light from two sides, as for an entrance.
            look.ambient[0] = 0.4f; look.ambient[1] = 0.4f; look.ambient[2] = 0.46f;
            look.lights.clear();
            look.lights.push_back(spotLight(0.0f, 900.0f, 0.0f, 0.0f, 100.0f, 0.0f, 1.0f, 0.97f, 0.9f, 0.8f, 22.0f, 34.0f, 2000.0f));
            look.lights.push_back(spotLight(-1100.0f, 700.0f, 0.0f, 0.0f, 100.0f, 0.0f, 0.25f, 0.45f, 1.0f, 1.2f, 20.0f, 36.0f, 3000.0f));
            look.lights.push_back(spotLight(1100.0f, 700.0f, 0.0f, 0.0f, 100.0f, 0.0f, 1.0f, 0.25f, 0.8f, 1.2f, 20.0f, 36.0f, 3000.0f));
        }
        else if (name == "house lights")
        {
            look.ambient[0] = look.ambient[1] = look.ambient[2] = 1.0f;
            look.lights.clear();
        }
        return look;
    }

    namespace
    {
        struct LookStore
        {
            std::mutex mutex;
            bool started = false;
            Look look;
            std::atomic<uint64_t> generation{1};
            int arena = -1;
            bool fixedByEnvironment = false; // PS2X_LIGHT_LOOK or the PS2X_LIGHT_AMBIENT / PS2X_LIGHT_SPOT_* values: an arena change keeps it
            bool show = false;
            std::string folder;        // of the user looks; empty: beside the settings file
            bool folderSet = false;
            std::string source = "built-in";
            // lightsWanted()
            uint64_t settingsGeneration = ~0ull;
            bool wanted = false;
        };

        LookStore &lookStore()
        {
            static LookStore store;
            return store;
        }

        std::string folderLocked(LookStore &s)
        {
            if (s.folderSet)
                return s.folder;
            const std::string settings = ps2x_settings::initialized() ? ps2x_settings::path() : std::string();
            if (settings.empty())
                return std::string();
            return (std::filesystem::path(settings).parent_path() / "lights").string();
        }

        std::string pathLocked(LookStore &s, int arena)
        {
            const std::string folder = folderLocked(s);
            if (folder.empty())
                return std::string();
            char name[32];
            std::snprintf(name, sizeof(name), "arena%02d.look", std::clamp(arena, 0, 99));
            return (std::filesystem::path(folder) / name).string();
        }

        // The look for an arena: the user's file if there is a good one, else the built-in. Says what it did on stderr.
        void loadForArenaLocked(LookStore &s)
        {
            const std::string path = pathLocked(s, std::max(s.arena, 0));
            std::error_code ec;
            if (!path.empty() && std::filesystem::exists(path, ec))
            {
                Look loaded;
                std::string why;
                if (loadLookFile(path, loaded, &why))
                {
                    s.look = loaded;
                    s.source = path;
                    std::fprintf(stderr, "[light] look for arena %d from %s (%zu lights)\n", s.arena, path.c_str(), loaded.lights.size());
                }
                else
                {
                    s.look = defaultLook();
                    s.source = "built-in (the file was bad)";
                    std::fprintf(stderr, "[light] %s; using the built-in look\n", why.c_str());
                }
            }
            else
            {
                s.look = defaultLook();
                s.source = "built-in";
            }
            s.generation.fetch_add(1, std::memory_order_relaxed);
        }

        void startLocked(LookStore &s)
        {
            if (s.started)
                return;
            s.started = true;
            s.look = defaultLook();
            if (const char *show = std::getenv("PS2X_LIGHT_SHOW")) // a test aid: "show lights" on from the start, without the panel
                s.show = std::strcmp(show, "1") == 0;
            if (const char *file = std::getenv("PS2X_LIGHT_LOOK"))
            {
                if (*file)
                {
                    std::string why;
                    Look loaded;
                    if (loadLookFile(file, loaded, &why))
                    {
                        s.look = loaded;
                        s.source = file;
                        std::fprintf(stderr, "[light] look from PS2X_LIGHT_LOOK=%s (%zu lights)\n", file, loaded.lights.size());
                    }
                    else
                        std::fprintf(stderr, "[light] PS2X_LIGHT_LOOK: %s; using the built-in look\n", why.c_str());
                    s.fixedByEnvironment = true;
                }
            }
            else
            {
                for (const char *name : {"PS2X_LIGHT_AMBIENT", "PS2X_LIGHT_SPOT_INTENSITY", "PS2X_LIGHT_SPOT_COLOUR", "PS2X_LIGHT_SPOT_CONE", "PS2X_LIGHT_SPOT_HEIGHT",
                                         "PS2X_LIGHT_SPOT_RADIUS"})
                    if (std::getenv(name))
                        s.fixedByEnvironment = true;
                if (s.fixedByEnvironment)
                {
                    lookFromEnvironment(s.look);
                    s.source = "the environment";
                }
            }
            s.generation.fetch_add(1, std::memory_order_relaxed);
        }
    }

    bool lightsWanted()
    {
        LookStore &s = lookStore();
        if (!ps2x_settings::initialized())
        {
            // No settings store (the replayer, the tests): the environment, read each time.
            const char *env = std::getenv("PS2X_LIGHT");
            return env && (std::strcmp(env, "1") == 0 || std::strcmp(env, "on") == 0);
        }
        const uint64_t generation = ps2x_settings::generation();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (generation != s.settingsGeneration)
        {
            s.settingsGeneration = generation;
            s.wanted = ps2x_settings::get("light") == "on";
        }
        return s.wanted;
    }

    uint64_t lookGeneration() { return lookStore().generation.load(std::memory_order_relaxed); }

    Look currentLook()
    {
        LookStore &s = lookStore();
        std::lock_guard<std::mutex> lock(s.mutex);
        startLocked(s);
        return s.look;
    }

    void setCurrentLook(const Look &look)
    {
        LookStore &s = lookStore();
        std::lock_guard<std::mutex> lock(s.mutex);
        startLocked(s);
        s.look = look;
        if (s.look.lights.size() > static_cast<size_t>(kMaxLights))
            s.look.lights.resize(static_cast<size_t>(kMaxLights));
        for (Light &l : s.look.lights)
            clampLight(l);
        s.source = "edited";
        s.generation.fetch_add(1, std::memory_order_relaxed);
    }

    bool showLights()
    {
        LookStore &s = lookStore();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.show;
    }

    void setShowLights(bool show)
    {
        LookStore &s = lookStore();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.show != show)
        {
            s.show = show;
            s.generation.fetch_add(1, std::memory_order_relaxed);
        }
    }

    int currentArena()
    {
        LookStore &s = lookStore();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.arena;
    }

    void setArena(int arena)
    {
        LookStore &s = lookStore();
        std::lock_guard<std::mutex> lock(s.mutex);
        startLocked(s);
        if (arena == s.arena)
            return;
        s.arena = arena;
        std::fprintf(stderr, "[light] arena %d\n", arena);
        if (!s.fixedByEnvironment)
            loadForArenaLocked(s);
        // A test aid (no panel click needed): PS2X_LIGHT_SAVE_PRESET=<a preset's name> makes that preset the look of the arena and
        // saves it as the user's file, as the panel's preset button and Save would.
        if (const char *preset = std::getenv("PS2X_LIGHT_SAVE_PRESET"))
        {
            if (*preset)
            {
                s.look = presetLook(preset);
                s.generation.fetch_add(1, std::memory_order_relaxed);
                const std::string path = pathLocked(s, std::max(s.arena, 0));
                std::string why;
                if (!path.empty() && saveLookFile(path, s.look, &why))
                {
                    s.source = path;
                    std::fprintf(stderr, "[light] PS2X_LIGHT_SAVE_PRESET: \"%s\" saved to %s\n", preset, path.c_str());
                }
                else
                    std::fprintf(stderr, "[light] PS2X_LIGHT_SAVE_PRESET: not saved (%s)\n", path.empty() ? "no settings folder" : why.c_str());
            }
        }
    }

    std::string userLookPath(int arena)
    {
        LookStore &s = lookStore();
        std::lock_guard<std::mutex> lock(s.mutex);
        return pathLocked(s, arena);
    }

    void setLookFolder(const std::string &folder)
    {
        LookStore &s = lookStore();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.folder = folder;
        s.folderSet = true;
    }

    bool saveUserLook(std::string *message)
    {
        LookStore &s = lookStore();
        std::lock_guard<std::mutex> lock(s.mutex);
        startLocked(s);
        const std::string path = pathLocked(s, std::max(s.arena, 0));
        if (path.empty())
        {
            if (message)
                *message = "there is no settings folder to save the look in";
            return false;
        }
        std::string why;
        if (!saveLookFile(path, s.look, &why))
        {
            if (message)
                *message = why;
            return false;
        }
        s.source = path;
        if (message)
            *message = "saved " + path;
        std::fprintf(stderr, "[light] look for arena %d saved to %s\n", std::max(s.arena, 0), path.c_str());
        return true;
    }

    void resetLook()
    {
        LookStore &s = lookStore();
        std::lock_guard<std::mutex> lock(s.mutex);
        startLocked(s);
        s.look = defaultLook();
        s.source = "built-in";
        s.generation.fetch_add(1, std::memory_order_relaxed);
    }

    bool reloadLook(std::string *message)
    {
        LookStore &s = lookStore();
        std::lock_guard<std::mutex> lock(s.mutex);
        startLocked(s);
        loadForArenaLocked(s);
        if (message)
            *message = "the look is now: " + s.source;
        return true;
    }

    std::string lookSource()
    {
        LookStore &s = lookStore();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.source;
    }

    void markerColour(int index, float rgb[3])
    {
        static const float palette[8][3] = {{1.0f, 0.25f, 0.2f}, {0.2f, 0.9f, 0.3f}, {0.3f, 0.5f, 1.0f}, {1.0f, 0.85f, 0.2f},
                                            {1.0f, 0.3f, 0.9f}, {0.2f, 0.9f, 0.9f}, {1.0f, 0.55f, 0.15f}, {0.75f, 0.75f, 0.75f}};
        std::memcpy(rgb, palette[std::clamp(index, 0, 7)], sizeof(palette[0]));
    }

    LightStatus &lightStatus()
    {
        static LightStatus status;
        return status;
    }

    LightUniforms lightUniforms(const Look &look, Facing facing, bool showMarkers)
    {
        LightUniforms u = {};
        for (int i = 0; i < 3; ++i)
            u.ambient[i] = look.ambient[i];
        u.rule[0] = facing == Facing::Lambert ? 0.0f : (facing == Facing::Half ? 1.0f : 2.0f);
        u.rule[1] = showMarkers ? 1.0f : 0.0f;
        u.rule[2] = 1.0f;
        const float degrees = 3.14159265358979f / 180.0f;
        int count = 0, index = -1;
        for (const Light &light : look.lights)
        {
            ++index;
            if (count >= kMaxLights)
                break;
            if (!light.on || !(light.strength > 0.0f) || !(light.radius > 0.0f))
                continue;
            float axis[3] = {0.0f, -1.0f, 0.0f};
            if (light.type == LightType::Spot)
            {
                const float d[3] = {light.target[0] - light.position[0], light.target[1] - light.position[1], light.target[2] - light.position[2]};
                const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (!(length > 0.0f))
                    continue; // (lightAt gives nothing for such a spot either)
                for (int i = 0; i < 3; ++i)
                    axis[i] = d[i] / length;
            }
            for (int i = 0; i < 3; ++i)
            {
                u.position[count][i] = light.position[i];
                u.axis[count][i] = axis[i];
                u.colour[count][i] = light.colour[i];
            }
            u.position[count][3] = light.radius;
            u.axis[count][3] = light.strength;
            u.colour[count][3] = light.type == LightType::Spot ? 1.0f : 0.0f;
            u.cone[count][0] = std::cos(light.outerDegrees * degrees);
            u.cone[count][1] = std::cos(light.innerDegrees * degrees);
            u.cone[count][3] = static_cast<float>(index);
            ++count;
        }
        u.ambient[3] = static_cast<float>(count);
        return u;
    }
}

namespace ps2x_gs
{
    void sceneCamera(const float *matrix16, uint32_t gameFrame, uint32_t buffer)
    {
        if (matrix16)
            ps2x_lighting::channel().post(matrix16, gameFrame, buffer);
    }

    bool partTag(uint32_t wrestler, uint32_t partClass, uint32_t order, uint32_t flags, const uint8_t original[4], uint8_t out[4])
    {
        ps2x_lighting::PartTag tag;
        tag.wrestler = static_cast<uint8_t>(wrestler & 0x0Fu);
        tag.partClass = static_cast<uint8_t>(partClass & 0x0Fu);
        tag.order = static_cast<uint8_t>(order > 31u ? 31u : order);
        tag.referee = (flags & 1u) != 0u;
        return ps2x_lighting::encodePartTag(tag, original, out);
    }

    bool partTagRead(const uint8_t fog[3], const float q[3], uint32_t &wrestler, uint32_t &partClass, uint32_t &order, uint32_t &flags)
    {
        ps2x_lighting::PartTag tag;
        if (!ps2x_lighting::decodePartTag(fog, q, tag))
            return false;
        wrestler = tag.wrestler;
        partClass = tag.partClass;
        order = tag.order;
        flags = tag.referee ? 1u : 0u;
        return true;
    }

    void sceneKickBegin(uint32_t gameFrame, uint32_t buffer)
    {
        ps2x_lighting::channel().kickBegin(gameFrame, buffer);
    }

    void sceneArena(int arena)
    {
        ps2x_lighting::setArena(arena);
    }

    void sceneKickEnd(uint64_t packets)
    {
        ps2x_lighting::CameraChannel &c = ps2x_lighting::channel();
        c.kickEnd(packets);
        if (ps2x_lighting::g_statsOn)
        {
            const ps2x_lighting::ChannelStats s = c.stats();
            if (s.kicks % 600u == 0u)
                std::fprintf(stderr, "[light] kicks %llu (with packets %llu), with a camera %llu; cameras posted %llu, replaced %llu, stale %llu\n",
                             static_cast<unsigned long long>(s.kicks), static_cast<unsigned long long>(s.kicksWithPackets),
                             static_cast<unsigned long long>(s.kicksWithCamera), static_cast<unsigned long long>(s.posted),
                             static_cast<unsigned long long>(s.replaced), static_cast<unsigned long long>(s.stale));
        }
    }
}
