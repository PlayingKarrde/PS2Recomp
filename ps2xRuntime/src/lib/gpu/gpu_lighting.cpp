// Lights for the match scene, part one: the camera channel, undoing the projection, the light types. See gpu_lighting.h.
#include "gpu/gpu_lighting.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
}

namespace ps2x_gs
{
    void sceneCamera(const float *matrix16, uint32_t gameFrame, uint32_t buffer)
    {
        if (matrix16)
            ps2x_lighting::channel().post(matrix16, gameFrame, buffer);
    }

    void sceneKickBegin(uint32_t gameFrame, uint32_t buffer)
    {
        ps2x_lighting::channel().kickBegin(gameFrame, buffer);
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
