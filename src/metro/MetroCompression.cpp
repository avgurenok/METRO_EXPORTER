#include "MetroCompression.h"

#define LZ4_DISABLE_DEPRECATE_WARNINGS
#include "lz4.h"
#include "lz4hc.h"


size_t MetroCompression::DecompressStream(const void* compressedData, const size_t compressedSize, void* uncompressedData, const size_t uncompressedSize) {
    const uint8_t* src = rcast<const uint8_t*>(compressedData);
    char* dst = rcast<char*>(uncompressedData);

    // The stream is a chain of blocks, each one prefixed by
    //   uint32 blockSize            - size of the whole block, this 8 byte header included
    //   uint32 blockUncompressedSize
    // Blocks are chained, so every one of them may reference up to 64k of the previously
    // decompressed output as its dictionary.
    //
    //#NOTE_SK: we deliberately use the *safe* lz4 entry point here and bounds-check every
    //          field first. The archive is not to be trusted - a truncated or partially
    //          copied .vfs pak used to send the unchecked decompressor past the end of both
    //          buffers, which corrupted the heap and hung the whole application instead of
    //          simply reporting a bad file.
    const size_t kMaxDictSize = 64 * 1024;

    size_t inCursor = 0, outCursor = 0;

    while (inCursor + 8 <= compressedSize) {
        uint32_t blockSize = 0, blockUncompressedSize = 0;
        memcpy(&blockSize, src + inCursor, sizeof(blockSize));
        memcpy(&blockUncompressedSize, src + inCursor + 4, sizeof(blockUncompressedSize));
        inCursor += 8;

        if (blockSize < 8) {
            return 0;
        }

        const size_t payloadSize = scast<size_t>(blockSize) - 8;
        if (payloadSize > (compressedSize - inCursor) || blockUncompressedSize > (uncompressedSize - outCursor)) {
            return 0;
        }

        const size_t dictSize = std::min<size_t>(outCursor, kMaxDictSize);

        const int nbWritten = LZ4_decompress_safe_usingDict(rcast<const char*>(src + inCursor),
                                                            dst + outCursor,
                                                            scast<int>(payloadSize),
                                                            scast<int>(blockUncompressedSize),
                                                            dst + outCursor - dictSize,
                                                            scast<int>(dictSize));
        if (nbWritten != scast<int>(blockUncompressedSize)) {
            return 0;
        }

        outCursor += blockUncompressedSize;
        inCursor += payloadSize;
    }

    return outCursor;
}

size_t MetroCompression::DecompressBlob(const void* compressedData, const size_t compressedSize, void* uncompressedData, const size_t uncompressedSize) {
    const int result = LZ4_decompress_safe(rcast<const char*>(compressedData), rcast<char*>(uncompressedData), scast<int>(compressedSize), scast<int>(uncompressedSize));
    return (result > 0 ? scast<size_t>(result) : 0);
}

size_t MetroCompression::CompressStream(const void* data, const size_t dataLength, BytesArray& compressed) {
    const size_t kMaxBlockSize = 0x30000;

    const size_t maxCompressedBlock = scast<size_t>(LZ4_compressBound(scast<int>(kMaxBlockSize)));
    BytesArray temp(maxCompressedBlock);
    char* dst = rcast<char*>(temp.data());

    size_t result = 0, bytesLeft = dataLength;

    MemStream inStream(data, dataLength);
    MemWriteStream outStream;

    while (bytesLeft) {
        const size_t blockSize = std::min<size_t>(kMaxBlockSize, bytesLeft);
        const char* src = rcast<const char*>(inStream.GetDataAtCursor());

        size_t packedSize = 0;

        const int lz4Result = LZ4_compress_HC(src, dst, scast<int>(blockSize), scast<int>(maxCompressedBlock), LZ4HC_CLEVEL_MAX);
        if (lz4Result <= 0) {
            // ooops, error :(
            return 0;
        } else {
            packedSize = scast<size_t>(lz4Result);
        }

        outStream.Write(scast<uint32_t>(packedSize + 8));
        outStream.Write(scast<uint32_t>(blockSize));
        outStream.Write(dst, packedSize);

        bytesLeft -= blockSize;
        result += blockSize;
    }

    outStream.buffer.swap(compressed);

    return result;
}

size_t MetroCompression::CompressBlob(const void* data, const size_t dataLength, BytesArray& compressed) {
    size_t result = 0;

    const size_t maxCompressedBlock = scast<size_t>(LZ4_compressBound(scast<int>(dataLength)));
    compressed.resize(maxCompressedBlock);

    const char* src = rcast<const char*>(data);
    char* dst = rcast<char*>(compressed.data());

    const int lz4Result = LZ4_compress_HC(src, dst, scast<int>(dataLength), scast<int>(maxCompressedBlock), LZ4HC_CLEVEL_MAX);
    if (lz4Result <= 0) {
        compressed.resize(0);
    } else {
        result = scast<size_t>(lz4Result);
        compressed.resize(result);
    }

    return result;
}
