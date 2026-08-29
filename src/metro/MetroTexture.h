#pragma once
#include "mycommon.h"

class MetroTexture {
public:
    enum class TextureFormat : size_t {
        BC1,
        BC2,
        BC3,
        BC7,
        BC6H,
        RGBA
    };

public:
    MetroTexture();
    ~MetroTexture();

    bool            LoadFromData(MemStream& stream, const CharString& name);
    bool            LoadFromFile(const fs::path& fileName);

    bool            SaveAsDDS(const fs::path& filePath);
    bool            SaveAsLegacyDDS(const fs::path& filePath);
    bool            SaveAsTGA(const fs::path& filePath);
    // Alpha handling for PNG export. Auto keeps the channel only when it looks like a cutout
    // mask rather than a gloss/spec mask - see the note in the implementation.
    enum class PNGAlpha {
        Auto,
        Keep,
        Drop
    };
    bool            SaveAsPNG(const fs::path& filePath, const PNGAlpha alphaMode = PNGAlpha::Keep);
    bool            SaveAsMetroTexture(const fs::path& filePath);

    bool            IsCubemap() const;
    size_t          GetWidth() const;
    size_t          GetHeight() const;
    size_t          GetDepth() const;
    size_t          GetNumMips() const;
    TextureFormat   GetFormat() const;
    size_t          GetBytesPerBlock() const;

    bool            GetRGBA(BytesArray& imagePixels) const;
    bool            GetBGRA(BytesArray& imagePixels) const;
    const uint8_t*  GetRawData() const;
    size_t          GetDataSize() const;

private:
    BytesArray      mData;
    bool            mIsCubemap;
    size_t          mWidth;
    size_t          mHeight;
    size_t          mDepth;
    size_t          mNumMips;
    TextureFormat   mFormat;
};
