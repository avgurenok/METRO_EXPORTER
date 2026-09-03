#include "MetroLevel.h"
#include "VFXReader.h"

#include <algorithm>

enum DescriptionChunk : size_t {
    DC_Header       = 0x00000001,
    DC_Materials    = 0x00000002,
    DC_Sections     = 0x00000003,
    DC_SectionInfo  = 0x00000015,
};

enum GeomChunk : size_t {
    GC_Vertices     = 0x00000009,
    GC_Indices      = 0x0000000A,
};

//#NOTE_SK: faces index into a 16 bit buffer, so no single exported mesh can hold more than
//          this many vertices. Sections that share a material are merged to keep the export
//          from turning into 650 separate objects, and the merge stops short of the ceiling.
static const size_t kMaxVerticesPerMesh = 65535;


MetroLevel::MetroLevel()
    : mBBox()
    , mNumTriangles(0)
{
    mBBox.Reset();
}

MetroLevel::~MetroLevel() {
    std::for_each(mMeshes.begin(), mMeshes.end(), [](MetroMesh* mesh) { delete mesh; });
}

bool MetroLevel::LoadFromData(VFXReader* vfxReader, const size_t descriptionFileIdx) {
    if (!vfxReader) {
        return false;
    }

    const MetroFile* folder = vfxReader->GetParentFolder(descriptionFileIdx);
    if (!folder) {
        return false;
    }

    const MetroFile& descFile = vfxReader->GetFile(descriptionFileIdx);
    const CharString geomName = descFile.name + ".geom_pc";

    size_t geometryFileIdx = MetroFile::InvalidFileIdx;
    for (const size_t ci : *folder) {
        const MetroFile& f = vfxReader->GetFile(ci);
        if (f.IsFile() && f.name == geomName) {
            geometryFileIdx = ci;
            break;
        }
    }

    if (MetroFile::InvalidFileIdx == geometryFileIdx) {
        LogPrintF(LogLevel::Error, "level: no %s next to %s", geomName.c_str(), descFile.name.c_str());
        return false;
    }

    MemStream descStream = vfxReader->ExtractFile(descriptionFileIdx);
    if (!descStream || !this->ReadDescription(descStream)) {
        return false;
    }

    MemStream geomStream = vfxReader->ExtractFile(geometryFileIdx);
    if (!geomStream || !this->ReadGeometry(geomStream)) {
        return false;
    }

    this->BuildMeshes();

    LogPrintF(LogLevel::Info, "level: %zu sections over %zu materials, %zu triangles -> %zu meshes",
              mSections.size(), mMaterials.size(), mNumTriangles, mMeshes.size());

    return !mMeshes.empty();
}

size_t MetroLevel::GetNumMeshes() const {
    return mMeshes.size();
}

const MetroMesh* MetroLevel::GetMesh(const size_t idx) const {
    return mMeshes[idx];
}

MyArray<MetroMesh*> MetroLevel::DetachMeshes() {
    MyArray<MetroMesh*> result;
    result.swap(mMeshes);
    return result;
}

const AABBox& MetroLevel::GetBBox() const {
    return mBBox;
}

size_t MetroLevel::GetNumSections() const {
    return mSections.size();
}

size_t MetroLevel::GetNumTriangles() const {
    return mNumTriangles;
}


bool MetroLevel::ReadDescription(MemStream& stream) {
    while (!stream.Ended()) {
        if ((stream.GetCursor() + 8) > stream.Length()) {
            break;
        }

        const size_t chunkId = stream.ReadTyped<uint32_t>();
        const size_t chunkSize = stream.ReadTyped<uint32_t>();
        const size_t chunkEnd = stream.GetCursor() + chunkSize;
        if (chunkEnd > stream.Length()) {
            break;
        }

        if (DC_Materials == chunkId) {
            //#NOTE_SK: a count, then four fields per material - the shader, the texture, the
            //          material itself, and a flags word. The exporter wants the texture.
            const size_t count = stream.ReadTyped<uint16_t>();
            mMaterials.reserve(count);
            for (size_t i = 0; i < count && stream.GetCursor() < chunkEnd; ++i) {
                stream.ReadStringZ();                           // shader
                const CharString texture = stream.ReadStringZ();
                stream.ReadStringZ();                           // material
                if ((stream.GetCursor() + 4) > chunkEnd) {
                    break;
                }
                stream.ReadTyped<uint32_t>();
                mMaterials.push_back(texture);
            }
        } else if (DC_Sections == chunkId) {
            //#NOTE_SK: sections are numbered from zero and in order, so the moment the index
            //          stops matching we have run off the end of the list rather than into
            //          the next one.
            size_t expected = 0;
            while ((stream.GetCursor() + 8) <= chunkEnd) {
                const size_t sectionIdx = stream.ReadTyped<uint32_t>();
                if (sectionIdx != expected) {
                    break;
                }

                const size_t sectionSize = stream.ReadTyped<uint32_t>();
                const size_t sectionEnd = stream.GetCursor() + sectionSize;
                if (sectionEnd > chunkEnd) {
                    break;
                }

                LevelSectionInfo info = {};
                uint16_t material = 0xFFFF;
                bool gotInfo = false;

                while ((stream.GetCursor() + 8) <= sectionEnd) {
                    const size_t subId = stream.ReadTyped<uint32_t>();
                    const size_t subSize = stream.ReadTyped<uint32_t>();
                    const size_t subEnd = stream.GetCursor() + subSize;
                    if (subEnd > sectionEnd) {
                        break;
                    }

                    if (DC_SectionInfo == subId && subSize == sizeof(LevelSectionInfo)) {
                        stream.ReadStruct(info);
                        gotInfo = true;
                    } else if (DC_Header == subId && subSize >= 4) {
                        //#NOTE_SK: this is the same 64 byte header a .model carries - version,
                        //          then type, and only then the material. Reading the front of
                        //          it as the material gave every section the same number, and
                        //          the whole map came out as six untextured lumps.
                        stream.ReadTyped<uint8_t>();    // version
                        stream.ReadTyped<uint8_t>();    // type
                        material = stream.ReadTyped<uint16_t>();
                    }

                    stream.SetCursor(subEnd);
                }

                if (gotInfo) {
                    mSections.push_back(info);
                    mSectionMaterial.push_back(material);
                }

                stream.SetCursor(sectionEnd);
                ++expected;
            }
        }

        stream.SetCursor(chunkEnd);
    }

    return !mSections.empty();
}

bool MetroLevel::ReadGeometry(MemStream& stream) {
    while (!stream.Ended()) {
        if ((stream.GetCursor() + 8) > stream.Length()) {
            break;
        }

        const size_t chunkId = stream.ReadTyped<uint32_t>();
        const size_t chunkSize = stream.ReadTyped<uint32_t>();
        const size_t chunkEnd = stream.GetCursor() + chunkSize;
        if (chunkEnd > stream.Length()) {
            break;
        }

        //#NOTE_SK: unlike the per-model geometry chunks, these carry no counts in front of
        //          them - the chunk is the buffer. Both sizes divide exactly, which is how
        //          we know: 10700960 bytes of vertices is 334405 whole VertexLevel and not
        //          334404 and a remainder.
        if (GC_Vertices == chunkId) {
            mVertexBuffer.resize(chunkSize);
            stream.ReadToBuffer(mVertexBuffer.data(), chunkSize);
        } else if (GC_Indices == chunkId) {
            mIndexBuffer.resize(chunkSize);
            stream.ReadToBuffer(mIndexBuffer.data(), chunkSize);
        }

        stream.SetCursor(chunkEnd);
    }

    if (mVertexBuffer.empty() || mIndexBuffer.empty()) {
        LogPrintF(LogLevel::Error, "level: geometry file has no vertex or index buffer");
        return false;
    }

    if ((mVertexBuffer.size() % sizeof(VertexLevel)) != 0 || (mIndexBuffer.size() % 2) != 0) {
        LogPrintF(LogLevel::Error, "level: geometry buffers are not a whole number of elements");
        return false;
    }

    return true;
}

void MetroLevel::BuildMeshes() {
    const size_t numVertices = mVertexBuffer.size() / sizeof(VertexLevel);
    const size_t numIndices = mIndexBuffer.size() / sizeof(uint16_t);

    const VertexLevel* srcVertices = rcast<const VertexLevel*>(mVertexBuffer.data());
    const uint16_t* srcIndices = rcast<const uint16_t*>(mIndexBuffer.data());

    //#NOTE_SK: sections are grouped by material so the map arrives as a few dozen objects
    //          instead of hundreds. Within a group the vertices are concatenated and the
    //          indices shifted along, which is the only reason the ceiling above matters.
    MyArray<size_t> order(mSections.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [this](const size_t a, const size_t b) {
        return mSectionMaterial[a] < mSectionMaterial[b];
    });

    MetroMesh* mesh = nullptr;
    uint16_t meshMaterial = 0xFFFF;

    for (const size_t si : order) {
        const LevelSectionInfo& info = mSections[si];
        const uint16_t material = mSectionMaterial[si];

        // a section that does not fit the buffers is not a section, it is a misread
        if ((scast<size_t>(info.vbOffset) + info.numVertices) > numVertices ||
            (scast<size_t>(info.ibOffset) + info.numIndices) > numIndices ||
            info.numVertices == 0 || info.numIndices < 3) {
            continue;
        }

        const bool needNew = (mesh == nullptr) ||
                             (material != meshMaterial) ||
                             ((mesh->vertices.size() + info.numVertices) > kMaxVerticesPerMesh);

        if (needNew) {
            mesh = new MetroMesh();
            mesh->skinned = false;
            mesh->vscale = 1.0f;
            mesh->type = 0;
            mesh->shaderId = 0;
            mesh->materials.push_back((material < mMaterials.size()) ? mMaterials[material] : kEmptyString);
            mesh->bbox.Reset();
            mMeshes.push_back(mesh);
            meshMaterial = material;
        }

        const size_t base = mesh->vertices.size();

        mesh->vertices.reserve(base + info.numVertices);
        for (size_t v = 0; v < info.numVertices; ++v) {
            const MetroVertex vertex = ConvertVertex(srcVertices[info.vbOffset + v]);
            mesh->vertices.push_back(vertex);
            mesh->bbox.Absorb(vertex.pos);
            mBBox.Absorb(vertex.pos);
        }

        const size_t numFaces = info.numIndices / 3;
        mesh->faces.reserve(mesh->faces.size() + numFaces);
        for (size_t f = 0; f < numFaces; ++f) {
            const uint16_t* tri = srcIndices + info.ibOffset + (f * 3);

            // indices are relative to the section's own slice of the vertex buffer
            if (tri[0] >= info.numVertices || tri[1] >= info.numVertices || tri[2] >= info.numVertices) {
                continue;
            }

            MetroFace face;
            face.a = scast<uint16_t>(base + tri[0]);
            face.b = scast<uint16_t>(base + tri[1]);
            face.c = scast<uint16_t>(base + tri[2]);
            mesh->faces.push_back(face);
        }

        mNumTriangles += numFaces;
    }
}
