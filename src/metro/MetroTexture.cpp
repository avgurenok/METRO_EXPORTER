#include "MetroTexture.h"
#include "MetroCompression.h"
#include "crn_transcode.h"
#include "dds_utils.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma warning (disable : 4793) // `anonymous namespace'::stbiw__outfile': function compiled as native
#include "stb_image_write.h"
#pragma warning (default : 4793)

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_JPEG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize.h"

#include <fstream>
#include <intrin.h>



static size_t NumMipsFromResolution(const size_t resolution) {
    size_t result = 0;

    unsigned long index;
    if (_BitScanReverse64(&index, resolution)) {
        result = static_cast<size_t>(index) + 1;
    }

    return result;
}


MetroTexture::MetroTexture()
    : mIsCubemap(false)
    , mWidth(0)
    , mHeight(0)
    , mDepth(0)
    , mNumMips(0)
    , mFormat(TextureFormat::BC7)
{
}
MetroTexture::~MetroTexture() {
}


bool MetroTexture::LoadFromData(MemStream& stream, const CharString& name) {
    bool result = false;

    const uint8_t* data = stream.GetDataAtCursor();
    const size_t length = stream.Remains();

    if (length < sizeof(uint32_t)) {
        return false;
    }

    uint32_t magic = 0;
    memcpy(&magic, data, sizeof(magic));

    //#NOTE_SK: the Redux games keep almost all of their textures as Crunch streams under the
    //          .512c / .1024c / .2048c extensions - about 6600 of the 7600 textures in Metro
    //          2033 Redux. Crunch is its own entropy coded format rather than a DXT container,
    //          so it has to be transcoded back to DXTn before anything else can touch it.
    if (CRN_IsCrunchStream(data, length)) {
        CRNImageInfo info = {};
        if (!CRN_GetInfo(data, length, &info) || !info.totalBytes) {
            return false;
        }

        switch (info.format) {
            case kCRNFormatBC1: mFormat = TextureFormat::BC1; break;
            case kCRNFormatBC2: mFormat = TextureFormat::BC2; break;
            case kCRNFormatBC3: mFormat = TextureFormat::BC3; break;
            default: return false;
        }

        mData.resize(info.totalBytes);
        if (!CRN_Decode(data, length, mData.data(), mData.size())) {
            mData.clear();
            return false;
        }

        mWidth = info.width;
        mHeight = info.height;
        mDepth = 1;
        mNumMips = info.numMips;

        return true;
    }


    if (magic == cDDSFileSignature) {
        // this is a plain DDS file
        stream.SkipBytes(4); // skip DDS magic

        DDSURFACEDESC2 ddsHdr;
        DDS_HEADER_DXT10 dx10Hdr;

        stream.ReadStruct(ddsHdr);

        const uint32_t fourCC = ddsHdr.ddpfPixelFormat.dwFourCC;
        if (fourCC == PIXEL_FMT_FOURCC('D', 'X', '1', '0')) {
            stream.ReadStruct(dx10Hdr);

            switch (dx10Hdr.dxgiFormat) {
                case DXGI_FORMAT_BC7_TYPELESS:
                case DXGI_FORMAT_BC7_UNORM:
                case DXGI_FORMAT_BC7_UNORM_SRGB: {
                    mFormat = TextureFormat::BC7;
                } break;

                case DXGI_FORMAT_BC6H_TYPELESS:
                case DXGI_FORMAT_BC6H_SF16:
                case DXGI_FORMAT_BC6H_UF16: {
                    mFormat = TextureFormat::BC6H;
                    mIsCubemap = true;
                } break;

                default:
                    return false;
            }
        } else {
            //#NOTE_SK: Metro 2033 / Last Light Redux predate the DX10 DDS extension and
            //          ship good old DX9-style DXTn files instead
            switch (fourCC) {
                case PIXEL_FMT_DXT1: {
                    mFormat = TextureFormat::BC1;
                } break;

                case PIXEL_FMT_DXT2:
                case PIXEL_FMT_DXT3: {
                    mFormat = TextureFormat::BC2;
                } break;

                case PIXEL_FMT_DXT4:
                case PIXEL_FMT_DXT5: {
                    mFormat = TextureFormat::BC3;
                } break;

                default:
                    return false;
            }
        }

        mWidth = ddsHdr.dwWidth;
        mHeight = ddsHdr.dwHeight;
        mDepth = 1;
        mNumMips = (ddsHdr.dwMipMapCount > 1) ? ddsHdr.dwMipMapCount : 1;

        mData.resize(stream.Remains());
        stream.ReadToBuffer(mData.data(), mData.size());

        result = true;
    } else {
        // headerless texture, the resolution and the mip count live in the extension
        CharString extension = fs::path(name).extension().string();
        size_t dimension = 0, numMips = 0;
        if (extension == ".512") {
            dimension = 512;
            numMips = 10;
        } else if (extension == ".1024") {
            dimension = 1024;
            numMips = 1;
        } else if (extension == ".2048") {
            dimension = 2048;
            numMips = 1;
        }

        if (dimension > 0) {
            //#NOTE_SK: two different games store these very differently.
            //          Redux keeps a plain, already unpacked BC1 or BC3 mip chain, so its
            //          length matches the chain size to the byte. Exodus keeps an extra
            //          LZ4 blob that unpacks into BC7. The exact size tells the two apart.
            const size_t bc1Size = DDS_GetCompressedSizeBCn(dimension, dimension, numMips, 8);
            const size_t bc3Size = DDS_GetCompressedSizeBCn(dimension, dimension, numMips, 16);

            if (length == bc1Size || length == bc3Size) {
                mFormat = (length == bc1Size) ? TextureFormat::BC1 : TextureFormat::BC3;
                mData.resize(length);
                memcpy(mData.data(), data, length);

                mWidth = dimension;
                mHeight = dimension;
                mDepth = 1;
                mNumMips = numMips;

                result = true;
            } else {
                const size_t bc7size = DDS_GetCompressedSizeBC7(dimension, dimension, numMips);
                mData.resize(bc7size);
                const size_t uresult = MetroCompression::DecompressBlob(data, length, mData.data(), bc7size);
                if (uresult != bc7size) {
                    mData.resize(0);
                } else {
                    mWidth = dimension;
                    mHeight = dimension;
                    mDepth = 1;
                    mNumMips = numMips;
                    mFormat = TextureFormat::BC7;

                    result = true;
                }
            }
        }
    }

    return result;
}

bool MetroTexture::LoadFromFile(const fs::path& fileName) {
    bool result = false;

    const std::wstring ext = fileName.extension().native();
    if (ext == L".tga" || ext == L".png") {
        std::ifstream file(fileName, std::ifstream::binary);
        if (file.good()) {
            BytesArray fileData;

            file.seekg(0, std::ios::end);
            fileData.resize(file.tellg());
            file.seekg(0, std::ios::beg);
            file.read(rcast<char*>(fileData.data()), fileData.size());
            file.close();

            int width, height, bpp;
            uint8_t* pixels = stbi_load_from_memory(fileData.data(), scast<int>(fileData.size()), &width, &height, &bpp, STBI_rgb_alpha);
            fileData.resize(0);
            if (pixels) {
                mData.resize(width * height * 4);
                memcpy(mData.data(), pixels, mData.size());

                stbi_image_free(pixels);

                mWidth = scast<size_t>(width);
                mHeight = scast<size_t>(height);
                mDepth = 1;
                mNumMips = 1;
                mFormat = TextureFormat::RGBA;

                result = true;
            }
        }
    }

    return result;
}

bool MetroTexture::SaveAsDDS(const fs::path& filePath) {
    bool result = false;

    std::ofstream file(filePath, std::ofstream::binary);
    if (!file.good()) {
        return false;
    }

    //#NOTE_SK: BC1/BC2/BC3 predate the DX10 header extension, and plenty of tools still
    //          choke on a DX10 header wrapped around them, so those go out as DX9 DDS
    uint32_t fourCC = 0;
    switch (mFormat) {
        case TextureFormat::BC1: fourCC = PIXEL_FMT_DXT1; break;
        case TextureFormat::BC2: fourCC = PIXEL_FMT_DXT3; break;
        case TextureFormat::BC3: fourCC = PIXEL_FMT_DXT5; break;
        default: break;
    }

    file.write(rcast<const char*>(&cDDSFileSignature), sizeof(cDDSFileSignature));

    if (fourCC) {
        DDSURFACEDESC2 ddsHdr;
        DDS_MakeDX9Header(ddsHdr, mWidth, mHeight, mNumMips, fourCC);
        file.write(rcast<const char*>(&ddsHdr), sizeof(ddsHdr));
    } else {
        DDSURFACEDESC2 ddsHdr;
        DDS_HEADER_DXT10 dx10Hdr;
        DDS_MakeDX10Headers(ddsHdr, dx10Hdr, mWidth, mHeight, mNumMips, mIsCubemap);
        file.write(rcast<const char*>(&ddsHdr), sizeof(ddsHdr));
        file.write(rcast<const char*>(&dx10Hdr), sizeof(dx10Hdr));
    }

    file.write(rcast<const char*>(mData.data()), mData.size());

    result = true;

    return result;
}

//#TODO: also mips ?
bool MetroTexture::SaveAsLegacyDDS(const fs::path& filePath) {
    bool result = false;

    //#TODO: add support!
    if (this->IsCubemap()) {
        return false;
    }

    std::ofstream file(filePath, std::ofstream::binary);
    if (file.good()) {
        DDSURFACEDESC2 ddsHdr;
        DDS_MakeDX9Header(ddsHdr, mWidth, mHeight, 1);

        BytesArray rgbaPixels;
        this->GetRGBA(rgbaPixels);

        BytesArray bc3Blocks(DDS_GetCompressedSizeBC7(mWidth, mHeight, 1));
        DDS_CompressBC3(rgbaPixels.data(), bc3Blocks.data(), mWidth, mHeight);

        file.write(rcast<const char*>(&cDDSFileSignature), sizeof(cDDSFileSignature));
        file.write(rcast<const char*>(&ddsHdr), sizeof(ddsHdr));
        file.write(rcast<const char*>(bc3Blocks.data()), bc3Blocks.size());

        result = true;
    }

    return result;
}

bool MetroTexture::SaveAsTGA(const fs::path& filePath) {
    bool result = false;

    std::ofstream file(filePath, std::ofstream::binary);
    if (file.good()) {
        BytesArray bgraPixels;
        if (this->GetBGRA(bgraPixels)) {
            uint16_t hdr[9] = { 0 };
            hdr[1] = 2;
            hdr[6] = scast<uint16_t>(mWidth);
            hdr[7] = scast<uint16_t>(mHeight);
            hdr[8] = 32;
            file.write(rcast<const char*>(hdr), sizeof(hdr));

            const size_t pitch = mWidth * 4;
            const char* pixels = rcast<const char*>(bgraPixels.data()) + (pitch * (mHeight - 1));
            for (size_t y = 0; y < mHeight; ++y) {
                file.write(pixels, pitch);
                pixels -= pitch;
            }

            result = true;
        }
    }

    return result;
}

bool MetroTexture::SaveAsPNG(const fs::path& filePath, const PNGAlpha alphaMode) {
    BytesArray rgbaPixels;
    if (!this->GetRGBA(rgbaPixels) || rgbaPixels.empty()) {
        return false;
    }

    const size_t numPixels = mWidth * mHeight;

    bool keepAlpha = (alphaMode == PNGAlpha::Keep);

    if (alphaMode == PNGAlpha::Auto) {
        //#NOTE_SK: Blender's FBX importer wires the diffuse straight into Alpha whenever the
        //          image has an alpha channel at all (import_fbx.py: `if image.depth == 32`).
        //          Metro mostly uses that channel for gloss and specular masks, so handing it
        //          a 32 bit diffuse turns solid objects semi-transparent. A real cutout mask
        //          is bimodal - lots of fully transparent texels and lots of fully opaque
        //          ones - while a gloss mask sits in the middle, so keep the channel only
        //          when it actually looks like cutout.
        size_t transparent = 0, opaque = 0;
        for (size_t i = 0; i < numPixels; ++i) {
            const uint8_t a = rgbaPixels[(i * 4) + 3];
            if (a < 16) {
                ++transparent;
            } else if (a > 240) {
                ++opaque;
            }
        }

        const double transparentFrac = scast<double>(transparent) / scast<double>(numPixels ? numPixels : 1);
        const double opaqueFrac = scast<double>(opaque) / scast<double>(numPixels ? numPixels : 1);

        keepAlpha = (transparentFrac > 0.01) && (opaqueFrac > 0.5);
    }

    std::ofstream file(filePath, std::ofstream::binary);
    if (!file.good()) {
        return false;
    }

    auto writeFunc = [](void* ptr, void* data, int size) {
        std::ofstream* filePtr = rcast<std::ofstream*>(ptr);
        filePtr->write(rcast<const char*>(data), size);
    };

    int success = 0;
    if (keepAlpha) {
        success = stbi_write_png_to_func(writeFunc, &file, scast<int>(mWidth), scast<int>(mHeight), 4, rgbaPixels.data(), 0);
    } else {
        BytesArray rgbPixels(numPixels * 3);
        for (size_t i = 0; i < numPixels; ++i) {
            rgbPixels[(i * 3) + 0] = rgbaPixels[(i * 4) + 0];
            rgbPixels[(i * 3) + 1] = rgbaPixels[(i * 4) + 1];
            rgbPixels[(i * 3) + 2] = rgbaPixels[(i * 4) + 2];
        }
        success = stbi_write_png_to_func(writeFunc, &file, scast<int>(mWidth), scast<int>(mHeight), 3, rgbPixels.data(), 0);
    }

    return success > 0;
}

bool MetroTexture::SaveAsMetroTexture(const fs::path& filePath) {
    bool result = false;

    size_t resolution = scast<size_t>(mWidth);

    BytesArray mipBuffer = mData;
    BytesArray nextMipBuffer; nextMipBuffer.resize(mipBuffer.size() / 4);

    if (resolution == 2048) {
        std::ofstream file(filePath.native() + L".2048", std::ofstream::binary);
        if (file.good()) {
            const size_t bc7Size = DDS_GetCompressedSizeBC7(resolution, resolution, 1);
            BytesArray bc7Buffer;
            bc7Buffer.resize(bc7Size);

            DDS_CompressBC7(mipBuffer.data(), bc7Buffer.data(), resolution, resolution);

            BytesArray lz4Buffer;
            MetroCompression::CompressBlob(bc7Buffer.data(), bc7Size, lz4Buffer);

            file.write(rcast<const char*>(lz4Buffer.data()), lz4Buffer.size());
            file.flush();
            file.close();
        }

        stbir_resize_uint8(mipBuffer.data(), scast<int>(resolution), scast<int>(resolution), 0,
                           nextMipBuffer.data(), scast<int>(resolution / 2), scast<int>(resolution / 2), 0, 4);

        resolution /= 2;
        mipBuffer = nextMipBuffer;
    }

    if (resolution == 1024) {
        std::ofstream file(filePath.native() + L".1024", std::ofstream::binary);
        if (file.good()) {
            const size_t bc7Size = DDS_GetCompressedSizeBC7(resolution, resolution, 1);
            BytesArray bc7Buffer;
            bc7Buffer.resize(bc7Size);

            DDS_CompressBC7(mipBuffer.data(), bc7Buffer.data(), resolution, resolution);

            BytesArray lz4Buffer;
            MetroCompression::CompressBlob(bc7Buffer.data(), bc7Size, lz4Buffer);

            file.write(rcast<const char*>(lz4Buffer.data()), lz4Buffer.size());
            file.flush();
            file.close();
        }

        stbir_resize_uint8(mipBuffer.data(), scast<int>(resolution), scast<int>(resolution), 0,
                           nextMipBuffer.data(), scast<int>(resolution / 2), scast<int>(resolution / 2), 0, 4);

        resolution /= 2;
        mipBuffer = nextMipBuffer;
    }

    if (resolution == 512) {
        std::ofstream file(filePath.native() + L".512", std::ofstream::binary);
        if (file.good()) {
            const size_t bc7Size = DDS_GetCompressedSizeBC7(resolution, resolution, 10);
            BytesArray bc7Buffer;
            bc7Buffer.resize(bc7Size);

            uint8_t* bufferA = mipBuffer.data();
            uint8_t* bufferB = nextMipBuffer.data();

            uint8_t* bc7Blocks = bc7Buffer.data();
            size_t mipResolution = resolution;
            for (size_t i = 0; i < 10; ++i) {
                DDS_CompressBC7(bufferA, bc7Blocks, mipResolution, mipResolution);

                const size_t mipSize = DDS_GetCompressedSizeBC7(mipResolution, mipResolution, 1);
                bc7Blocks += mipSize;

                const size_t nextResolution = std::max<size_t>(4, mipResolution / 2);
                if (nextResolution != resolution) { //mip size changed - resample
                    stbir_resize_uint8(bufferA, scast<int>(mipResolution), scast<int>(mipResolution), 0,
                                       bufferB, scast<int>(nextResolution), scast<int>(nextResolution), 0, 4);

                    std::swap(bufferA, bufferB);
                }
                mipResolution = nextResolution;
            }

            BytesArray lz4Buffer;
            MetroCompression::CompressBlob(bc7Buffer.data(), bc7Size, lz4Buffer);

            file.write(rcast<const char*>(lz4Buffer.data()), lz4Buffer.size());
            file.flush();
            file.close();

            result = true;
        }
    }

    return false;
}

bool MetroTexture::IsCubemap() const {
    return mIsCubemap;
}

size_t MetroTexture::GetWidth() const {
    return mWidth;
}

size_t MetroTexture::GetHeight() const {
    return mHeight;
}

size_t MetroTexture::GetDepth() const {
    return mDepth;
}

size_t MetroTexture::GetNumMips() const {
    return mNumMips;
}

MetroTexture::TextureFormat MetroTexture::GetFormat() const {
    return mFormat;
}

size_t MetroTexture::GetBytesPerBlock() const {
    // BC1 packs a 4x4 block into 8 bytes, every other block format we handle uses 16
    return (mFormat == TextureFormat::BC1) ? 8 : 16;
}

bool MetroTexture::GetRGBA(BytesArray& imagePixels) const {
    bool result = false;

    if (!mData.empty() && mWidth && mHeight) {
        imagePixels.resize(mWidth * mHeight * 4);

        switch (mFormat) {
            case TextureFormat::BC1: {
                DDS_DecompressBC1(mData.data(), imagePixels.data(), mWidth, mHeight);
                result = true;
            } break;

            case TextureFormat::BC2: {
                DDS_DecompressBC2(mData.data(), imagePixels.data(), mWidth, mHeight);
                result = true;
            } break;

            case TextureFormat::BC3: {
                DDS_DecompressBC3(mData.data(), imagePixels.data(), mWidth, mHeight);
                result = true;
            } break;

            case TextureFormat::BC7: {
                DDS_DecompressBC7(mData.data(), imagePixels.data(), mWidth, mHeight);
                result = true;
            } break;

            case TextureFormat::RGBA: {
                memcpy(imagePixels.data(), mData.data(), std::min(imagePixels.size(), mData.size()));
                result = true;
            } break;

            default: {
                //#TODO_SK: BC6H (hdr cubemaps) still has no software decompressor here
                imagePixels.clear();
            } break;
        }
    }

    return result;
}

bool MetroTexture::GetBGRA(BytesArray& imagePixels) const {
    const bool result = this->GetRGBA(imagePixels);
    if (result) {
        uint8_t* rgba = imagePixels.data();
        for (size_t i = 0; i < mWidth * mHeight; ++i) {
            std::swap(rgba[0], rgba[2]);
            rgba += 4;
        }
    }
    return result;
}

const uint8_t* MetroTexture::GetRawData() const {
    return mData.data();
}

size_t MetroTexture::GetDataSize() const {
    return mData.size();
}
