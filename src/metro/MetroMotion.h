#pragma once
#include "MetroTypes.h"

struct AttributeCurve {
    struct AttribPoint {
        float   time;
        vec4    value;
    };

    MyArray<AttribPoint> points;
};

class MetroMotion {
public:
    MetroMotion(const CharString& name = "");
    ~MetroMotion();

    bool                    LoadFromData(MemStream& stream);
    void                    SetPath(const CharString& path);
    const CharString&       GetName() const;
    const CharString&       GetPath() const;
    

    size_t                  GetBonesCRC() const;
    size_t                  GetNumBones() const;
    size_t                  GetNumLocators() const;
    size_t                  GetNumKeys() const;
    float                   GetMotionTimeInSeconds() const;

    bool                    IsBoneAnimated(const size_t boneIdx) const;
    bool                    HasBoneRotation(const size_t boneIdx) const;
    bool                    HasBonePosition(const size_t boneIdx) const;
    quat                    GetBoneRotation(const size_t boneIdx, const size_t key) const;
    vec3                    GetBonePosition(const size_t boneIdx, const size_t key) const;

    // Slides a bone's whole position curve so its first key sits at the given bind position,
    // leaving it alone when the first key is already within onlyIfFurtherThan of it. Returns
    // how far the curve moved, which is zero when it was left alone.
    float                   AnchorBonePosition(const size_t boneIdx, const vec3& bindPosition,
                                               const float onlyIfFurtherThan = 0.0f);

//private:
    bool                    LoadInternal();
    void                    ReadAttributeCurve(const uint8_t* curveData, AttributeCurve& curve, const size_t attribSize, const bool bigEndian);

//private:
    CharString              mName;
    CharString              mPath;

    // header
    size_t                  mVersion;
    size_t                  mBonesCRC;
    size_t                  mNumBones;
    size_t                  mNumLocators;
    // info
    size_t                  mFlags;
    float                   mSpeed;
    float                   mAccrue;
    float                   mFalloff;
    size_t                  mNumKeys;
    size_t                  mJumpFrame;
    size_t                  mLandFrame;
    Bitset256               mAffectedBones;
    size_t                  mMotionsDataSize;
    size_t                  mMotionsOffsetsSize;
    Bitset256               mHighQualityBones;

    // Redux-era motions use 128 bit bone masks and store their data block big endian
    bool                    mReduxLayout;
    size_t                  mMaskBytes;
    // bones this motion actually animates, taken from the data block header
    Bitset256               mMotionBonesMask;
    // curves whose offset resolved to nothing decodable - those bones hold their bind pose
    size_t                  mRefusedCurves;
    // data
    BytesArray              mMotionsData;
    // curves
    MyArray<AttributeCurve> mBonesRotations;
    MyArray<AttributeCurve> mBonesPositions;
};
