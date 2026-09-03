#pragma once
#include "MetroTypes.h"

class VFXReader;
class MetroTexturesDatabase;
class MetroSkeleton;
class MetroMotion;

class MetroModel {
public:
    MetroModel();
    ~MetroModel();

    bool                    LoadFromData(MemStream& stream, VFXReader* vfxReader, const size_t fileIdx);

    // Builds a model out of a whole level's geometry, so that everything the exporter already
    // does for a .model - materials, textures, units, embedding - applies to a map unchanged.
    bool                    LoadFromLevel(VFXReader* vfxReader, const size_t descriptionFileIdx);
    bool                    SaveAsOBJ(const fs::path& filePath, VFXReader* vfxReader, MetroTexturesDatabase* database);
    bool                    SaveAsFBX(const fs::path& filePath, VFXReader* vfxReader, MetroTexturesDatabase* database,
                                      const bool withAnims, const bool embedTextures = true);

    bool                    IsAnimated() const;
    const AABBox&           GetBBox() const;
    const vec4&             GetBSphere() const;
    size_t                  GetNumMeshes() const;
    const MetroMesh*        GetMesh(const size_t idx) const;

    const CharString&       GetSkeletonPath() const;
    const MetroSkeleton*    GetSkeleton() const;
    size_t                  GetNumMotions() const;
    const MetroMotion*      GetMotion(const size_t idx) const;

private:
    void                    ReadSubChunks(MemStream& stream);
    void                    LoadLinkedMeshes(const StringArray& links);
    void                    LoadMotions();

private:
    AABBox                  mBBox;
    vec4                    mBSphere;
    MyArray<MetroMesh*>     mMeshes;
    CharString              mSkeletonPath;
    MetroSkeleton*          mSkeleton;
    MyArray<MetroMotion*>   mMotions;

    // these are temp pointers, invalid after loading
    MetroMesh*              mCurrentMesh;
    VFXReader*              mVFXReader;
    size_t                  mThisFileIdx;
    // raised when a chunk asks for more data than the stream can possibly hold,
    // which is what a model from a game this parser does not understand looks like
    bool                    mParseError;
    // Redux-era assets need a few layout tweaks compared to the Exodus ones
    bool                    mReduxAssets;
};
