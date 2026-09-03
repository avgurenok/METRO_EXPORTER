#pragma once
#include "MetroTypes.h"

class VFXReader;

//#NOTE_SK: a level's geometry lives in two files side by side. The one named "level" describes
//          it - a material table and a list of sections, each naming a slice of the buffers -
//          and "level.geom_pc" holds one shared vertex buffer and one shared index buffer for
//          the whole map. Metro 2033 Redux writes a 20 byte section record; Exodus writes 28,
//          the extra two fields being its shadow geometry.
PACKED_STRUCT_BEGIN
struct LevelSectionInfo {   // 20 bytes
    uint32_t    vertexType;
    uint32_t    vbOffset;
    uint32_t    numVertices;
    uint32_t    ibOffset;
    uint32_t    numIndices;
} PACKED_STRUCT_END;

class MetroLevel {
public:
    MetroLevel();
    ~MetroLevel();

    // descriptionFileIdx is the level's "level" file; the geometry is found next to it
    bool                LoadFromData(VFXReader* vfxReader, const size_t descriptionFileIdx,
                                     const bool withPlacedObjects = true);

    size_t              GetNumMeshes() const;
    const MetroMesh*    GetMesh(const size_t idx) const;
    // hands the meshes over to the caller and forgets them, so a MetroModel can adopt them
    // and reuse everything the model exporter already knows how to do
    MyArray<MetroMesh*> DetachMeshes();

    const AABBox&       GetBBox() const;
    size_t              GetNumSections() const;
    size_t              GetNumTriangles() const;
    size_t              GetNumPlacedObjects() const;
    size_t              GetNumUnplacedRefs() const;

private:
    bool                ReadDescription(MemStream& stream);
    bool                ReadGeometry(MemStream& stream);
    void                BuildMeshes();
    void                ReadPlacedObjects(VFXReader* vfxReader, const MetroFile* folder);

private:
    StringArray               mMaterials;       // texture name per material
    MyArray<LevelSectionInfo> mSections;
    MyArray<uint16_t>         mSectionMaterial;
    BytesArray                mVertexBuffer;
    BytesArray                mIndexBuffer;
    MyArray<MetroMesh*>       mMeshes;
    AABBox                    mBBox;
    size_t                    mNumTriangles;
    size_t                    mNumPlacedObjects;
    size_t                    mNumUnplacedRefs;
};
