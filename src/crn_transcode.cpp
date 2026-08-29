#include "crn_transcode.h"

//#NOTE_SK: crn_decomp.h picks its platform with `#ifdef WIN32`, and a 64 bit MSVC build only
//          defines _WIN32/_WIN64 - so without this it falls through to the glibc branch and
//          fails to compile on malloc_usable_size. Set before the include, library untouched.
#if defined(_WIN32) && !defined(WIN32)
#   define WIN32 1
#endif

//#NOTE_SK: crn_decomp.h carries its own implementation, so exactly one translation unit may
//          include it "hot" like this - everyone else would get duplicate symbols.
#include "crn_decomp.h"

#include <algorithm>

namespace {

    // Works out the format and the exact per-level layout, straight from the library.
    //
    //#NOTE_SK: the level dimensions come from crnd rather than from (w+3)/4 arithmetic -
    //          Crunch keeps its own minimum block footprint on the small mips, and guessing
    //          the pitch makes crnd_unpack_level write outside the buffer.
    bool GatherLayout(const void* data,
                      const crnd::uint32 dataSize,
                      crnd::crn_texture_info& texInfo,
                      uint32_t& outFormat,
                      size_t& outBytesPerBlock,
                      size_t* levelPitches,
                      size_t* levelSizes,
                      size_t& outTotal) {
        if (!crnd::crnd_get_texture_info(data, dataSize, &texInfo)) {
            return false;
        }

        // plain 2D textures only, and only the DXTn family
        if (texInfo.m_faces != 1 || !texInfo.m_width || !texInfo.m_height || !texInfo.m_levels) {
            return false;
        }

        switch (texInfo.m_format) {
            case cCRNFmtDXT1: outFormat = kCRNFormatBC1; outBytesPerBlock = 8;  break;
            case cCRNFmtDXT3: outFormat = kCRNFormatBC2; outBytesPerBlock = 16; break;
            case cCRNFmtDXT5: outFormat = kCRNFormatBC3; outBytesPerBlock = 16; break;
            default:
                // DXN and the swizzled DXT5 derivatives - nothing sane to hand a BCn view
                return false;
        }

        outTotal = 0;
        for (crnd::uint32 level = 0; level < texInfo.m_levels; ++level) {
            crnd::crn_level_info levelInfo;
            if (!crnd::crnd_get_level_info(data, dataSize, level, &levelInfo)) {
                return false;
            }

            levelPitches[level] = static_cast<size_t>(levelInfo.m_blocks_x) * outBytesPerBlock;
            levelSizes[level] = levelPitches[level] * levelInfo.m_blocks_y;
            outTotal += levelSizes[level];
        }

        return outTotal != 0;
    }

    const size_t kMaxLevels = 32;

} // anonymous namespace

bool CRN_IsCrunchStream(const void* data, const size_t length) {
    if (!data || length < 2) {
        return false;
    }

    // Crunch streams open with a big endian 0x4878 ("Hx")
    const uint8_t* p = static_cast<const uint8_t*>(data);
    return (p[0] == 0x48) && (p[1] == 0x78);
}

bool CRN_GetInfo(const void* data, const size_t length, CRNImageInfo* outInfo) {
    if (!outInfo || !CRN_IsCrunchStream(data, length) || length > 0xFFFFFFFFull) {
        return false;
    }

    const crnd::uint32 dataSize = static_cast<crnd::uint32>(length);

    crnd::crn_texture_info texInfo;
    uint32_t format = kCRNFormatUnsupported;
    size_t bytesPerBlock = 0, total = 0;
    size_t pitches[kMaxLevels] = {}, sizes[kMaxLevels] = {};

    if (!GatherLayout(data, dataSize, texInfo, format, bytesPerBlock, pitches, sizes, total)) {
        return false;
    }
    if (texInfo.m_levels > kMaxLevels) {
        return false;
    }

    outInfo->width = texInfo.m_width;
    outInfo->height = texInfo.m_height;
    outInfo->numMips = texInfo.m_levels;
    outInfo->format = format;
    outInfo->totalBytes = static_cast<uint32_t>(total);

    return true;
}

bool CRN_Decode(const void* data, const size_t length, void* dst, const size_t dstSize) {
    if (!dst || !CRN_IsCrunchStream(data, length) || length > 0xFFFFFFFFull) {
        return false;
    }

    const crnd::uint32 dataSize = static_cast<crnd::uint32>(length);

    crnd::crn_texture_info texInfo;
    uint32_t format = kCRNFormatUnsupported;
    size_t bytesPerBlock = 0, total = 0;
    size_t pitches[kMaxLevels] = {}, sizes[kMaxLevels] = {};

    if (!GatherLayout(data, dataSize, texInfo, format, bytesPerBlock, pitches, sizes, total)) {
        return false;
    }
    if (texInfo.m_levels > kMaxLevels || total > dstSize) {
        return false;
    }

    crnd::crnd_unpack_context ctx = crnd::crnd_unpack_begin(data, dataSize);
    if (!ctx) {
        return false;
    }

    bool ok = true;
    size_t offset = 0;
    for (crnd::uint32 level = 0; level < texInfo.m_levels; ++level) {
        void* levelDst = static_cast<uint8_t*>(dst) + offset;
        if (!crnd::crnd_unpack_level(ctx, &levelDst, static_cast<crnd::uint32>(sizes[level]),
                                     static_cast<crnd::uint32>(pitches[level]), level)) {
            ok = false;
            break;
        }
        offset += sizes[level];
    }

    crnd::crnd_unpack_end(ctx);

    return ok;
}
