#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace GSMem
{
    class TexturePageCache
    {
    public:
        static constexpr uint32_t kPageSize = 8192u;

        void Invalidate() noexcept
        {
            m_pageBase = UINT32_MAX;
        }

        // FPWR: the cache holds one copy of one page and is not refreshed when VRAM changes (only
        // Invalidate() and a miss refresh it), so what a read returns can depend on its history. A GS
        // recording (gs_recording.h) therefore saves and restores it with the rest of the backend state.
        uint32_t CachedPageBase() const noexcept { return m_pageBase; } // UINT32_MAX when empty
        const uint8_t *CachedPageBytes() const noexcept { return m_bytes.data(); }
        void RestoreCachedPage(uint32_t pageBase, const uint8_t *bytes) noexcept
        {
            m_source = nullptr;
            m_sourceContext = nullptr;
            if (pageBase == UINT32_MAX || !bytes)
            {
                m_pageBase = UINT32_MAX;
                return;
            }
            std::memcpy(m_bytes.data(), bytes, kPageSize);
            m_pageBase = pageBase;
        }

        // FPWR: an optional source that can supply an older copy of a page (nullptr = use VRAM).
        using PageSource = const uint8_t *(*)(const void *context, uint32_t pageBase);
        void SetSource(PageSource source, const void *context) noexcept
        {
            m_source = source;
            m_sourceContext = context;
            m_pageBase = UINT32_MAX;
        }

        // byteAddress is the wrapped, swizzled VRAM address. The returned
        // pointer is valid only until the next miss or invalidation.
        const uint8_t* Resolve(const uint8_t* vram, uint32_t byteAddress) noexcept
        {
            const uint32_t pageBase = byteAddress & ~(kPageSize - 1u);
            if (m_pageBase != pageBase)
            {
                const uint8_t *page = m_source ? m_source(m_sourceContext, pageBase) : nullptr;
                std::memcpy(m_bytes.data(), page ? page : vram + pageBase, kPageSize);
                m_pageBase = pageBase;
            }
            return m_bytes.data() + (byteAddress & (kPageSize - 1u));
        }

    private:
        alignas(64) std::array<uint8_t, kPageSize> m_bytes{};
        uint32_t m_pageBase = UINT32_MAX;
        PageSource m_source = nullptr;
        const void *m_sourceContext = nullptr;
    };
}
