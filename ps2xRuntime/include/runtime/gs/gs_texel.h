#pragma once

// FPWR: texel and CLUT helpers shared by the two GS renderers (GSCpuBackend and GSGpuBackend), so
// there is one copy of the rules for what a texture read returns:
//   - TEXA alpha rules for 24- and 16-bit textures and CLUTs,
//   - which CLUT entry an index selects (CSM1/CSM2, CSA, 4- and 8-bit indices),
//   - loading the CLUT buffer from GS memory (LoadClut, TEX0.CLD).
// Moved unchanged out of gs_cpu_backend.cpp. Nothing here includes SDL, and no header that generated
// game code includes refers to this one.

#include "runtime/gs/gs_types.h"

#include <array>
#include <cstdint>

namespace GSTexel
{
    inline uint32_t Rgba5551ToRgba8888(uint16_t c)
    {
        const uint32_t r = ((c >> 0) & 0x1F) << 3;
        const uint32_t g = ((c >> 5) & 0x1F) << 3;
        const uint32_t b = ((c >> 10) & 0x1F) << 3;
        const uint32_t a = ((c >> 15) & 0x01) << 7;
        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    inline uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    inline uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        // CSM1 swaps address bits 3 and 4. Preserve the remaining bits:
        // 16-bit CLUTs expose a ninth address bit through CSA[4].
        return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    inline bool isFourBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    inline bool isEightBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H;
    }

    // The texel (RGBA8888, alpha as the GS sees it) that `index` selects from the 512-entry CLUT
    // buffer for a texture of format `sourcePsm` and the TEX0 CLUT fields cpsm, csm, csa.
    inline uint32_t LookupClutEntry(const uint16_t *clut, const GSTexaReg &texa, uint8_t index,
                                    uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm)
    {
        const bool sixteenBit = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
        const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
        const uint32_t clutBase = (static_cast<uint32_t>(csa) & csaMask) << 4u;
        const uint32_t sourceIndex = isFourBitIndexedPsm(sourcePsm)
                                         ? (static_cast<uint32_t>(index) & 0x0Fu)
                                         : static_cast<uint32_t>(index);

        uint32_t clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (!sixteenBit && csm == 0u && isEightBitIndexedPsm(sourcePsm))
        {
            const uint32_t block = ((sourceIndex & 0xF0u) + clutBase) < 240u ? ((sourceIndex & 0xF0u) + clutBase) : 240u;
            clutIndex = block + (sourceIndex & 0x0Fu);
        }

        switch (cpsm)
        {
        case GS_PSM_CT32:
        {
            const uint32_t raw = static_cast<uint32_t>(clut[clutIndex]) | (static_cast<uint32_t>(clut[clutIndex + 256u]) << 16u);
            return applyTexa(texa, cpsm, raw);
        }
        case GS_PSM_CT24:
        {
            const uint32_t raw = static_cast<uint32_t>(clut[clutIndex]) | (static_cast<uint32_t>(clut[clutIndex + 256u]) << 16u);
            return applyTexa(texa, cpsm, raw & 0x00FFFFFFu);
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return applyTexa(texa, cpsm, Rgba5551ToRgba8888(clut[clutIndex]));
        default:
            break;
        }

        return 0xFFFF00FFu;
    }

    // LoadClut with TEX0.CLD already decided to load: copies the CLUT described by tex0/texclut from GS
    // memory into the 512-entry buffer. `readTexture(psm, base, bw, x, y)` reads one pixel of GS memory
    // the way a texture read does.
    template <typename ReadTexture>
    inline void LoadClutFromMemory(std::array<uint16_t, 512> &clut, const GSTex0Reg &tex0,
                                   const GSTexClutReg &texclut, ReadTexture &&readTexture)
    {
        const bool fourBit = isFourBitIndexedPsm(tex0.psm);
        const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
        const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
        if (!sixteenBit && !thirtyTwoBit)
            return;

        const uint32_t entryCount = fourBit ? 16u : 256u;
        const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
        const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;

        const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
        const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;

        for (uint32_t entry = firstEntry; entry < entryCount; ++entry)
        {
            uint32_t sourceX = 0u;
            uint32_t sourceY = 0u;
            uint32_t sourceWidth = 1u;

            if (tex0.csm == 0u)
            {
                const uint32_t sourceIndex = swizzleClutIndexCSM1(entry);
                sourceX = sourceIndex & 0x0Fu;
                sourceY = sourceIndex >> 4u;
            }
            else
            {
                sourceWidth = texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
                sourceX = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
                sourceY = static_cast<uint32_t>(texclut.cov);
            }

            const uint32_t raw = readTexture(tex0.cpsm, tex0.cbp, sourceWidth, sourceX, sourceY);
            const uint32_t destination = (loadCsm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
            if (sixteenBit)
            {
                clut[destination] = static_cast<uint16_t>(raw);
            }
            else
            {
                clut[destination] = static_cast<uint16_t>(raw & 0xFFFFu);
                clut[destination + 256u] = static_cast<uint16_t>(raw >> 16u);
            }
        }
    }

    // What TEX0.CLD says about a LoadClut call. Returns false when the call loads nothing; otherwise
    // `clutCbp` (the two CLUT base pointers the backend remembers) is updated as the register says.
    inline bool ClutLoadWanted(const GSTex0Reg &tex0, std::array<uint32_t, 2> &clutCbp)
    {
        switch (tex0.cld)
        {
        case 0u:
        case 6u:
        case 7u:
            return false;
        case 1u:
            return true;
        case 2u:
            clutCbp[0] = tex0.cbp;
            return true;
        case 3u:
            clutCbp[1] = tex0.cbp;
            return true;
        case 4u:
            if (clutCbp[0] == tex0.cbp)
                return false;
            clutCbp[0] = tex0.cbp;
            return true;
        case 5u:
            if (clutCbp[1] == tex0.cbp)
                return false;
            clutCbp[1] = tex0.cbp;
            return true;
        default:
            return false;
        }
    }
}
