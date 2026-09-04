#include "MetroLevel.h"
#include "VFXReader.h"
#include "MetroModel.h"
#include "hashing.h"

#include <algorithm>
#include <cmath>

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
    , mNumPlacedObjects(0)
    , mNumUnplacedRefs(0)
{
    mBBox.Reset();
}

MetroLevel::~MetroLevel() {
    std::for_each(mMeshes.begin(), mMeshes.end(), [](MetroMesh* mesh) { delete mesh; });
}

bool MetroLevel::LoadFromData(VFXReader* vfxReader, const size_t descriptionFileIdx, const bool withPlacedObjects) {
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

    if (withPlacedObjects) {
        this->ReadPlacedObjects(vfxReader, folder);
    }

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

size_t MetroLevel::GetNumPlacedObjects() const {
    return mNumPlacedObjects;
}

size_t MetroLevel::GetNumUnplacedRefs() const {
    return mNumUnplacedRefs;
}


//=============================================================================================
// Objects placed around the level
//
//#NOTE_SK: the walls, floors and tunnels are baked into the level's own geometry, but the
//          things standing on them - doors, crates, lamps, the metro carriages - are entities
//          listed in level.bin, each naming a model and where to put it.
//
//          level.bin carries no debug info and, unusually for these archives, not a single
//          string: an entity names its model by the CRC32 of the model's path relative to
//          content\meshes and without the extension. Hashing every model in the archive and
//          looking those values up turns the numbers back into names - 210 distinct models
//          across 547 references in l02_exhibition.
//
//          The transform is harder, because the records around it are variable length and
//          there are no field names to walk. It does not need to be parsed, though: a
//          rotation matrix is an extremely improbable thing to find in arbitrary bytes - three
//          rows of equal length, each perpendicular to the others - so scanning forward from
//          the reference for the first run of twelve floats that satisfies that finds it.
//          Bounding the scan at the next reference keeps one entity from claiming the next
//          one's transform. That places 544 of 547 references.
//=============================================================================================

namespace {

    // one entity: which model, and where it goes
    struct PlacedObject {
        size_t  modelFileIdx;
        vec3    axisX, axisY, axisZ;    // already swizzled into export space
        vec3    position;
    };

    void CollectModelHashes(VFXReader* vfx, const MetroFile& dir, const CharString& prefix,
                            MyDict<uint32_t, size_t>& out) {
        for (const size_t ci : dir) {
            const MetroFile& f = vfx->GetFile(ci);
            const CharString full = prefix.empty() ? f.name : (prefix + scast<char>(92) + f.name);
            if (f.IsFile()) {
                if (full.size() > 6 && full.compare(full.size() - 6, 6, ".model") == 0) {
                    //#NOTE_SK: the hash is over the path as the engine names it - relative to
                    //          content\meshes and without the extension. Hashing the whole
                    //          archive path instead matches nothing at all.
                    CharString rel = full.substr(0, full.size() - 6);

                    CharString prefixToStrip("content");
                    prefixToStrip += scast<char>(92);
                    prefixToStrip += "meshes";
                    prefixToStrip += scast<char>(92);

                    if (rel.size() > prefixToStrip.size() &&
                        rel.compare(0, prefixToStrip.size(), prefixToStrip) == 0) {
                        rel = rel.substr(prefixToStrip.size());
                    }

                    out[Hash_CalculateCRC32(rel)] = ci;
                }
            } else {
                CollectModelHashes(vfx, f, full, out);
            }
        }
    }

    inline float ReadFloat(const uint8_t* p) {
        float f;
        memcpy(&f, p, sizeof(f));
        return f;
    }

    // Twelve floats: a rotation, possibly uniformly scaled, then a position.
    bool ReadTransform(const uint8_t* p, vec3& a0, vec3& a1, vec3& a2, vec3& t) {
        float m[9];
        for (int i = 0; i < 9; ++i) {
            m[i] = ReadFloat(p + (i * 4));
            if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1000.0f) {
                return false;
            }
        }

        float len[3];
        for (int r = 0; r < 3; ++r) {
            len[r] = std::sqrt((m[r*3] * m[r*3]) + (m[r*3+1] * m[r*3+1]) + (m[r*3+2] * m[r*3+2]));
            if (len[r] < 1e-3f || len[r] > 1000.0f) {
                return false;
            }
        }

        // the three rows have to be the same length - a uniform scale, or none at all
        if (std::fabs(len[0] - len[1]) > (1e-3f * len[0]) || std::fabs(len[0] - len[2]) > (1e-3f * len[0])) {
            return false;
        }

        // ...and mutually perpendicular, which is what makes this test worth anything
        for (int a = 0; a < 3; ++a) {
            for (int b = a + 1; b < 3; ++b) {
                const float d = ((m[a*3] * m[b*3]) + (m[a*3+1] * m[b*3+1]) + (m[a*3+2] * m[b*3+2])) / (len[a] * len[b]);
                if (std::fabs(d) > 0.01f) {
                    return false;
                }
            }
        }

        float pos[3];
        for (int i = 0; i < 3; ++i) {
            pos[i] = ReadFloat(p + ((9 + i) * 4));
            if (!std::isfinite(pos[i]) || std::fabs(pos[i]) > 5000.0f) {
                return false;
            }
        }

        a0 = vec3(m[0], m[1], m[2]);
        a1 = vec3(m[3], m[4], m[5]);
        a2 = vec3(m[6], m[7], m[8]);
        t  = vec3(pos[0], pos[1], pos[2]);
        return true;
    }

} // anonymous namespace

void MetroLevel::ReadPlacedObjects(VFXReader* vfxReader, const MetroFile* folder) {
    size_t binIdx = MetroFile::InvalidFileIdx;
    for (const size_t ci : *folder) {
        const MetroFile& f = vfxReader->GetFile(ci);
        if (f.IsFile() && f.name == "level.bin") {
            binIdx = ci;
            break;
        }
    }

    if (MetroFile::InvalidFileIdx == binIdx) {
        LogPrintF(LogLevel::Warning, "level: no level.bin, nothing to place on the map");
        return;
    }

    MemStream binStream = vfxReader->ExtractFile(binIdx);
    if (!binStream || binStream.Length() < 64) {
        return;
    }

    BytesArray bin(binStream.Length());
    binStream.ReadToBuffer(bin.data(), bin.size());

    MyDict<uint32_t, size_t> modelsByHash;
    CollectModelHashes(vfxReader, vfxReader->GetRootFolder(), kEmptyString, modelsByHash);

    // every position at which the file names a model we have
    MyArray<size_t> refs;
    for (size_t off = 0; (off + sizeof(uint32_t)) <= bin.size(); ++off) {
        const uint32_t v = *rcast<const uint32_t*>(bin.data() + off);
        if (v != 0) {
            auto it = modelsByHash.find(v);
            if (it != modelsByHash.end()) {
                refs.push_back(off);
            }
        }
    }

    //#NOTE_SK: orthonormality alone is a strong filter but not a sufficient one - on eight of
    //          the game's maps it occasionally locked onto a matrix belonging to some other
    //          record, and the map came out with props flung kilometres away: l03_chase grew
    //          twenty-three fold. Where the object has to end up is known, though, because the
    //          level's own geometry has already been read, so requiring the position to land
    //          on the map is part of finding the transform rather than a check afterwards.
    //          It also drops the handful of entities the designers parked off the map.
    const vec3 levelSize = mBBox.maximum - mBBox.minimum;
    const vec3 margin = vec3(std::max(levelSize.x * 0.2f, 10.0f),
                             std::max(levelSize.y * 0.2f, 10.0f),
                             std::max(levelSize.z * 0.2f, 10.0f));
    const vec3 allowedMin = mBBox.minimum - margin;
    const vec3 allowedMax = mBBox.maximum + margin;

    MyArray<PlacedObject> placements;
    placements.reserve(refs.size());
    size_t offMapRejects = 0;

    //#NOTE_SK: the transform sits BEFORE the model reference, at a fixed distance, and getting
    //          this backwards is what put every object on the map in the wrong place.
    //
    //          Searching forwards - which is what this did at first - walks straight into the
    //          NEXT entity's matrix. There is always one close by to grab: l02_exhibition holds
    //          3913 transforms against 547 model references, the rest belonging to lights,
    //          triggers and spawn points. So every object wore some later entity's position and
    //          the map came out scattered, some pieces a hundred metres from where they belong.
    //
    //          Measured over all 28 maps, backwards: 22395 of 31913 references have their
    //          matrix at exactly -184 bytes and another 4873 at -188 - 85% at two fixed offsets.
    //          Forwards the same references produce 69 different offsets covering two thirds.
    //          Taking the fixed offsets first and only then looking nearby keeps a record from
    //          ever reaching into its neighbour.
    //
    //          The mistake survived my checks because none of them actually tested position:
    //          an upright barrel stays upright whichever upright matrix you hand it, and every
    //          wrong matrix still belonged to something standing somewhere on the same map.
    //          What does test it is that lamps hang and people stand - read this way the
    //          characters on l02_exhibition come out at -0.01 m, exactly on the floor, and the
    //          lamps at 2.05 m; read forwards the characters floated 0.89 m off the ground.
    static const int kFixedOffsets[] = { -184, -188 };
    static const int kNearestWindowFrom = -160;
    static const int kNearestWindowTo   = -320;

    for (size_t i = 0; i < refs.size(); ++i) {
        const size_t at = refs[i];

        // never reach back past the previous reference - that matrix belongs to that record
        const long long floorPos = (i > 0) ? scast<long long>(refs[i - 1] + sizeof(uint32_t)) : 0;

        vec3 a0, a1, a2, t;
        bool found = false;

        auto tryAt = [&](const int delta) -> bool {
            const long long pos = scast<long long>(at) + delta;
            if (pos < floorPos || (pos + 48) > scast<long long>(bin.size())) {
                return false;
            }
            if (!ReadTransform(bin.data() + pos, a0, a1, a2, t)) {
                return false;
            }
            const vec3 where = MetroSwizzle(t);
            if (where.x < allowedMin.x || where.x > allowedMax.x ||
                where.y < allowedMin.y || where.y > allowedMax.y ||
                where.z < allowedMin.z || where.z > allowedMax.z) {
                ++offMapRejects;
                return false;
            }
            return true;
        };

        for (const int delta : kFixedOffsets) {
            if (tryAt(delta)) { found = true; break; }
        }

        // a small minority of records carry a field or two more; look close by, never far
        for (int delta = kNearestWindowFrom; !found && delta >= kNearestWindowTo; --delta) {
            if (tryAt(delta)) { found = true; }
        }

        if (!found) {
            ++mNumUnplacedRefs;
            continue;
        }

        PlacedObject po;
        po.modelFileIdx = modelsByHash[*rcast<const uint32_t*>(bin.data() + at)];

        //#NOTE_SK: model vertices come out of ConvertVertex already swizzled, and the transform
        //          is in the game's own axis order. Swizzling is a swap of x and z, so the axes
        //          have to be swizzled themselves and the first and last exchanged to match.
        po.axisX = MetroSwizzle(a2);
        po.axisY = MetroSwizzle(a1);
        po.axisZ = MetroSwizzle(a0);
        po.position = MetroSwizzle(t);

        placements.push_back(po);
    }

    LogPrintF(LogLevel::Info, "level: %zu model references in level.bin, %zu of them placed",
              refs.size(), placements.size());
    if (offMapRejects) {
        LogPrintF(LogLevel::Info, "level: %zu candidate transforms pointed off the map and were passed over", offMapRejects);
    }

    // load each distinct model once, then stamp copies of it around the level
    MyDict<size_t, MetroModel*> loaded;
    size_t addedVertices = 0, addedTriangles = 0, instance = 0;

    for (const PlacedObject& po : placements) {
        MetroModel* model = nullptr;

        auto it = loaded.find(po.modelFileIdx);
        if (it != loaded.end()) {
            model = it->second;
        } else {
            MemStream ms = vfxReader->ExtractFile(po.modelFileIdx);
            if (ms) {
                model = new MetroModel();
                //#NOTE_SK: geometry only - a prop's animation library is of no interest here
                //          and there are hundreds of props
                if (!model->LoadFromData(ms, vfxReader, po.modelFileIdx, false)) {
                    MySafeDelete(model);
                }
            }
            loaded[po.modelFileIdx] = model;
        }

        if (!model) {
            continue;
        }

        const CharString baseName = fs::path(vfxReader->GetFile(po.modelFileIdx).name).stem().u8string();

        for (size_t mi = 0; mi < model->GetNumMeshes(); ++mi) {
            const MetroMesh* src = model->GetMesh(mi);
            if (src->vertices.empty() || src->faces.empty()) {
                continue;
            }

            MetroMesh* dst = new MetroMesh(*src);
            // instance number and mesh number both, so a model made of several pieces does
            // not hand Blender a pile of same-named objects to disambiguate for us
            dst->name = baseName + "_" + std::to_string(instance) + "_" + std::to_string(mi);
            dst->bbox.Reset();

            for (MetroVertex& v : dst->vertices) {
                const vec3 p = v.pos;
                v.pos = (po.axisX * p.x) + (po.axisY * p.y) + (po.axisZ * p.z) + po.position;

                const vec3 n(v.normal.x, v.normal.y, v.normal.z);
                const vec3 rotated = (po.axisX * n.x) + (po.axisY * n.y) + (po.axisZ * n.z);
                const float len = Length(rotated);
                if (len > MM_Epsilon) {
                    v.normal = vec4(rotated / len, v.normal.w);
                }

                dst->bbox.Absorb(v.pos);
                mBBox.Absorb(v.pos);
            }

            addedVertices += dst->vertices.size();
            addedTriangles += dst->faces.size();
            mMeshes.push_back(dst);
        }

        ++mNumPlacedObjects;
        ++instance;
    }

    for (auto& kv : loaded) {
        MySafeDelete(kv.second);
    }

    mNumTriangles += addedTriangles;

    LogPrintF(LogLevel::Info,
              "level: placed %zu objects from %zu distinct models, adding %zu vertices and %zu triangles (%zu references had no transform)",
              mNumPlacedObjects, loaded.size(), addedVertices, addedTriangles, mNumUnplacedRefs);
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
            // name it after the material so the map arrives as something readable rather
            // than as four hundred numbered lumps
            mesh->name = "level_" + fs::path(mesh->materials.front()).filename().u8string()
                       + "_" + std::to_string(mMeshes.size());
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
