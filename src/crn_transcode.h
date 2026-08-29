#pragma once
#include <cstddef>
#include <cstdint>

// Transcodes a Crunch (.crn) stream into raw DXTn blocks.
//
// The Metro 2033 / Last Light Redux archives store the bulk of their textures as Crunch
// streams under the .512c / .1024c / .2048c extensions - roughly 6600 of the 7600 textures
// in Metro 2033 Redux. Crunch is not a container around DXT, it is its own entropy coded
// format, so the only way to get pixels out is to transcode it back to DXTn first.
//
//#NOTE_SK: deliberately a plain C-style interface built out of PODs. crn_transcode.cpp is
//          compiled as native code inside an otherwise /clr assembly, and handing a
//          std::vector across that boundary corrupts the heap - the two sides do not agree
//          on the container's layout. Raw pointers and integers cross safely; the caller
//          owns the buffer.

enum CRNPixelFormat {
    kCRNFormatUnsupported = 0,
    kCRNFormatBC1,      // DXT1
    kCRNFormatBC2,      // DXT3
    kCRNFormatBC3       // DXT5
};

struct CRNImageInfo {
    uint32_t    width;
    uint32_t    height;
    uint32_t    numMips;
    uint32_t    format;         // CRNPixelFormat
    uint32_t    totalBytes;     // size of the whole mip chain, tightly packed
};

// True when the blob opens with the Crunch signature.
bool CRN_IsCrunchStream(const void* data, const size_t length);

// Reads the header only. False when the stream is not something we can transcode.
bool CRN_GetInfo(const void* data, const size_t length, CRNImageInfo* outInfo);

// Transcodes the whole mip chain into dst, which must be at least totalBytes big.
bool CRN_Decode(const void* data, const size_t length, void* dst, const size_t dstSize);
