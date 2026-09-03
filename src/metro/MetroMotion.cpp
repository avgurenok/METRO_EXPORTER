#include "MetroMotion.h"

PACKED_STRUCT_BEGIN
struct MotionDataHeader {   // size = 48 bytes
    Bitset256   bonesMask;
    uint16_t    numLocators;
    uint16_t    numXforms;
    uint32_t    totalSize;
    uint64_t    unknown_0;
} PACKED_STRUCT_END;

enum MotionChunks {
    MC_HeaderChunk  = 0x00000000,
    MC_InfoChunk    = 0x00000001,
    MC_DataChunk    = 0x00000009,
};

MetroMotion::MetroMotion(const CharString& name)
    : mName(name)
    // header
    , mVersion(0)
    , mBonesCRC(0)
    , mNumBones(0)
    // info
    , mFlags(0)
    , mSpeed(0.0f)
    , mAccrue(0.0f)
    , mFalloff(0.0f)
    , mNumKeys(0)
    , mJumpFrame(0)
    , mLandFrame(0)
    , mAffectedBones()
    , mMotionsDataSize(0)
    , mMotionsOffsetsSize(0)
    , mHighQualityBones()
    , mReduxLayout(false)
    , mMaskBytes(sizeof(Bitset256))
    , mMotionBonesMask()
    , mRefusedCurves(0)
{

}
MetroMotion::~MetroMotion() {

}

bool MetroMotion::LoadFromData(MemStream& stream) {
    bool result = false;

    while (!stream.Ended()) {
        const size_t chunkId = stream.ReadTyped<uint32_t>();
        const size_t chunkSize = stream.ReadTyped<uint32_t>();
        const size_t chunkEnd = stream.GetCursor() + chunkSize;

        switch (chunkId) {
            case MC_HeaderChunk: {
                mVersion = stream.ReadTyped<uint32_t>();

                //#TODO_SK: check bones info against skeleton !
                mBonesCRC = stream.ReadTyped<uint32_t>();
                mNumBones = stream.ReadTyped<uint16_t>();
                mNumLocators = stream.ReadTyped<uint16_t>();
            } break;

            case MC_InfoChunk: {
                mFlags = stream.ReadTyped<uint16_t>();

                mSpeed = stream.ReadTyped<float>();
                mAccrue = stream.ReadTyped<float>();
                mFalloff = stream.ReadTyped<float>();

                mNumKeys = stream.ReadTyped<uint32_t>();
                mJumpFrame = stream.ReadTyped<uint16_t>();
                mLandFrame = stream.ReadTyped<uint16_t>();

                //#NOTE_SK: the bone masks are not always 256 bits wide. Everything in this
                //          chunk apart from the two masks is a fixed 30 bytes - flags(2),
                //          three floats(12), numKeys(4), jump/land(4), and the two sizes(8) -
                //          so whatever is left over is the two masks, and half of that is one
                //          mask. Metro 2033 Redux writes 16 byte (128 bone) masks and a 62
                //          byte chunk, Exodus writes 32 byte masks and a 94 byte one.
                const size_t kFixedPart = 30;
                mMaskBytes = sizeof(Bitset256);
                if (chunkSize > kFixedPart) {
                    const size_t masksTotal = chunkSize - kFixedPart;
                    const size_t candidate = masksTotal / 2;
                    if ((masksTotal % 2) == 0 && (candidate == 16 || candidate == 32)) {
                        mMaskBytes = candidate;
                    }
                }
                mReduxLayout = (mMaskBytes < sizeof(Bitset256));

                memset(&mAffectedBones, 0, sizeof(mAffectedBones));
                stream.ReadToBuffer(&mAffectedBones, mMaskBytes);

                mMotionsDataSize = stream.ReadTyped<uint32_t>();
                mMotionsOffsetsSize = stream.ReadTyped<uint32_t>();

                memset(&mHighQualityBones, 0, sizeof(mHighQualityBones));
                if (chunkEnd > stream.GetCursor() && (chunkEnd - stream.GetCursor()) >= mMaskBytes) {
                    stream.ReadToBuffer(&mHighQualityBones, mMaskBytes);
                }
            } break;

            case MC_DataChunk: {
                //#NOTE_SK: used to assert on a mismatch, which killed the app outright on any
                //          motion we misread. Just refuse the motion instead.
                if (chunkSize != mMotionsDataSize) {
                    LogPrintF(LogLevel::Warning, "motion %s: data chunk is %zu bytes, info said %zu - skipping",
                              mName.c_str(), chunkSize, mMotionsDataSize);
                    return false;
                }

                mMotionsData.resize(mMotionsDataSize);
                stream.ReadToBuffer(mMotionsData.data(), mMotionsData.size());
            } break;
        }

        stream.SetCursor(chunkEnd);
    }

    result = this->LoadInternal();

    mMotionsData.resize(0);

    return result;
}

void MetroMotion::SetPath(const CharString& path) {
    mPath = path;
}

const CharString& MetroMotion::GetName() const {
    return mName;
}

const CharString& MetroMotion::GetPath() const {
    return mPath;
}

size_t MetroMotion::GetBonesCRC() const {
    return mBonesCRC;
}

size_t MetroMotion::GetNumBones() const {
    return mNumBones;
}

size_t MetroMotion::GetNumLocators() const {
    return mNumLocators;
}

size_t MetroMotion::GetNumKeys() const {
    return mNumKeys;
}

float MetroMotion::GetMotionTimeInSeconds() const {
    return scast<float>(mNumKeys) / 30.0f;
}

//#NOTE_SK: there are two bone masks and they do not mean the same thing. The one in the data
//          block says which bones that block carries curves for - the offset table holds
//          exactly one triple per bit set in it. The one in the info chunk is something
//          broader; it is all ones in half the motions in Metro 2033 Redux, and where it is
//          not, it is usually a superset.
//
//          This used to answer with the intersection of the two, and the same intersection
//          drove the walk over the offset table. In 778 of the game's 10693 motions the info
//          mask is missing bones the block mask has - and there the walk skipped a triple,
//          so every bone after it was handed the previous bone's rotation. That is what threw
//          creatures' limbs inside out on their attack animations, and it is why the ones that
//          animate few bones looked worst. Only the block mask can index the table.
bool MetroMotion::IsBoneAnimated(const size_t boneIdx) const {
    return mMotionBonesMask.IsPresent(boneIdx);
}

//#NOTE_SK: a bone can be listed as animated and still carry an empty curve for one of its
//          attributes - most bones never translate, so their position curve is empty, and a
//          bone the animator left alone has an empty rotation curve too. "Empty" means "leave
//          this attribute at the bind pose", but GetBoneRotation/GetBonePosition have no way
//          to know the bind pose and hand back an identity rotation and a zero offset instead.
//          Callers have to ask first and substitute the bind pose themselves, otherwise every
//          such bone snaps to its parent's origin.
bool MetroMotion::HasBoneRotation(const size_t boneIdx) const {
    return (boneIdx < mBonesRotations.size()) && !mBonesRotations[boneIdx].points.empty();
}

bool MetroMotion::HasBonePosition(const size_t boneIdx) const {
    return (boneIdx < mBonesPositions.size()) && !mBonesPositions[boneIdx].points.empty();
}

quat MetroMotion::GetBoneRotation(const size_t boneIdx, const size_t key) const {
    quat result(1.0f, 0.0f, 0.0f, 0.0f);

    const AttributeCurve& curve = mBonesRotations[boneIdx];
    if (!curve.points.empty()) {
        if (curve.points.size() == 1) { // constant value
            result = *rcast<const quat*>(&curve.points.front().value);
        } else {
            const float timing = scast<float>(key) / 30.0f;

            const size_t numPoints = curve.points.size();
            size_t pointA = numPoints, pointB = numPoints;
            for (size_t i = 0; i < numPoints; ++i) {
                const auto& p = curve.points[i];
                if (p.time >= timing) {
                    pointB = i;
                    break;
                }
            }

            if (pointB == numPoints) {
                pointB--;
                pointA = pointB;
            } else if (pointB == 0) {
                pointA = 0;
            } else {
                pointA = pointB - 1;
            }

            const auto& pA = curve.points[pointA];

            if (pointA == pointB) {
                result = *rcast<const quat*>(&pA.value);
            } else {
                const auto& pB = curve.points[pointB];
                const float t = (timing - pA.time) / (pB.time - pA.time);

                result = QuatSlerp(*rcast<const quat*>(&pA.value), *rcast<const quat*>(&pB.value), t);
            }
        }
    }

    return result;
}

vec3 MetroMotion::GetBonePosition(const size_t boneIdx, const size_t key) const {
    vec3 result(0.0f);

    const AttributeCurve& curve = mBonesPositions[boneIdx];
    if (!curve.points.empty()) {
        if (curve.points.size() == 1) { // constant value
            result = *rcast<const vec3*>(&curve.points.front().value);
        } else {
            const float timing = scast<float>(key) / 30.0f;

            const size_t numPoints = curve.points.size();
            size_t pointA = numPoints, pointB = numPoints;
            for (size_t i = 0; i < numPoints; ++i) {
                const auto& p = curve.points[i];
                if (p.time >= timing) {
                    pointB = i;
                    break;
                }
            }

            if (pointB == numPoints) {
                pointB--;
                pointA = pointB;
            } else if (pointB == 0) {
                pointA = 0;
            } else {
                pointA = pointB - 1;
            }

            const auto& pA = curve.points[pointA];

            if (pointA == pointB) {
                result = pA.value;
            } else {
                const auto& pB = curve.points[pointB];
                const float t = (timing - pA.time) / (pB.time - pA.time);

                result = Lerp(*rcast<const vec3*>(&pA.value), *rcast<const vec3*>(&pB.value), t);
            }
        }
    }

    return result;
}



//#NOTE_SK: animations that belong to a level are authored where the object actually stands in
//          that level, so their root bone's position curve carries world coordinates - in
//          Metro 2033 Redux that reaches 1492 metres. Played against a model sitting at the
//          origin, the model leaves the viewport the instant you press play and performs its
//          animation somewhere off in the dark. Sliding the curve so its first key lands on
//          the bind position keeps every bit of the movement and drops only the placement,
//          which is the one part of it that means nothing outside its level.
//
//          An animation that was already anchored has its first key at the bind position
//          already, so this shifts it by zero and changes nothing.
float MetroMotion::AnchorBonePosition(const size_t boneIdx, const vec3& bindPosition, const float onlyIfFurtherThan) {
    if (boneIdx >= mBonesPositions.size()) {
        return 0.0f;
    }

    AttributeCurve& curve = mBonesPositions[boneIdx];
    if (curve.points.empty()) {
        return 0.0f;
    }

    const vec3 firstKey(curve.points.front().value);
    const vec3 delta = firstKey - bindPosition;

    const float distance = Length(delta);
    if (distance <= 0.0f || distance <= onlyIfFurtherThan) {
        return 0.0f;
    }

    for (auto& p : curve.points) {
        p.value.x -= delta.x;
        p.value.y -= delta.y;
        p.value.z -= delta.z;
    }

    return distance;
}


enum class AttribCurveType : uint8_t {
    Invalid         = 0,
    Uncompressed    = 1,    // raw float values
    OneValue        = 2,    // constant value, no curve
    Unknown_3       = 3,
    CompressedPos   = 4,    // quantized position, scale + offset + u16 values
    CompressedQuat  = 5,    // quantized quaternion (xyz, we restore w), s16_snorm values
    Unknown_6       = 6,
    Empty           = 7     // no curve, why not just filter it out with mask ???
};


//#NOTE_SK: the motion data block stores its sizes and its offset table big endian, unlike
//          everything around it. Verified against Metro 2033 Redux: read little endian the
//          totals come out as nonsense like 0x80080000, read big endian they land exactly on
//          the data chunk size, and the offsets fall inside the buffer.
static uint32_t ReadBE32(const uint8_t* p) {
    return (scast<uint32_t>(p[0]) << 24) |
           (scast<uint32_t>(p[1]) << 16) |
           (scast<uint32_t>(p[2]) <<  8) |
           (scast<uint32_t>(p[3]));
}

static uint32_t ReadLE32(const uint8_t* p) {
    return (scast<uint32_t>(p[3]) << 24) |
           (scast<uint32_t>(p[2]) << 16) |
           (scast<uint32_t>(p[1]) <<  8) |
           (scast<uint32_t>(p[0]));
}

//#NOTE_SK: ...except the block is not big endian all the way through. Partway along, both
//          the offset table and the curves it points at switch to little endian, and nothing
//          in the header says where. Reading the whole block one way left the tail of the
//          bone list - which is where the facial bones live - with no curves at all, so every
//          face bone fell back to a zero translation and the head tore itself apart.
//
//          Nothing here guesses: an offset has to land inside the block, and a curve header
//          has to carry a known type, and for both of those only one of the two readings ever
//          qualifies. Checked over all 3568 Metro 2033 Redux motions - every offset resolves,
//          every curve header decodes, and every rotation comes out a unit quaternion.

// How many bytes a curve occupies, so one that would run off the end can be refused
// before it is decoded rather than after.
static size_t CurveByteSize(const uint32_t header, const size_t attribSize) {
    const size_t numPoints = scast<size_t>(header & 0xFFFF);

    switch ((header >> 16) & 0xF) {
        case 1:  return 4 + (numPoints * 4) + (numPoints * attribSize * 4);   // Uncompressed
        case 2:  return 4 + (attribSize * 4);                                 // OneValue
        case 4:  return 4 + 4 + 24 + (numPoints * 2) + (numPoints * 6);       // CompressedPos
        case 5:  return 4 + 4 + (numPoints * 2) + (numPoints * 6);            // CompressedQuat
        case 3:
        case 6:
        case 7:  return 4;                                                    // no payload we read
        default: return 0;
    }
}

// A curve header is four bytes: an attribute marker, a type nibble, and a point count.
//
//#NOTE_SK: the marker is what tells the two byte orders apart, and it says more than the
//          old code assumed. Its low nibble is the component count - 4 for a rotation, 3 for
//          the vector attributes - and its high nibble says whether the curve carries data,
//          so a populated rotation reads 0x14 but an empty one reads 0x04. Matching the whole
//          byte against 0x14 therefore failed on every empty rotation curve, and the guess
//          that followed decoded that bone's neighbours as garbage. Matching the low nibble
//          instead, and insisting the type nibble names a real type, leaves exactly one
//          reading standing.
static bool DecodeCurveHeader(const uint8_t* p, const size_t attribSize, const size_t avail,
                              uint32_t& outHeader, size_t& outSize, bool& outBigEndian) {
    for (int pass = 0; pass < 2; ++pass) {
        const bool be = (pass == 0);

        const uint8_t marker = be ? p[0] : p[3];
        if ((marker & 0x0F) != attribSize) {
            continue;
        }

        const uint32_t header = be ? ReadBE32(p) : ReadLE32(p);

        const uint32_t type = (header >> 16) & 0xF;
        if (type < 1 || type > 7) {
            continue;
        }

        const size_t size = CurveByteSize(header, attribSize);
        if (size == 0 || size > avail) {
            continue;
        }

        outHeader = header;
        outSize = size;
        outBigEndian = be;
        return true;
    }

    return false;
}

// Turns one offset-table slot into a curve.
//
//#NOTE_SK: an offset used to be accepted on the strength of landing somewhere inside the
//          block, which is a very low bar - a wrong reading lands inside the block most of
//          the time, and then a bone was handed whatever bytes happened to be there. That is
//          what threw creatures' bones inside out and flung animated props across the level:
//          nothing was detected as wrong, it just decoded rubbish. An offset now has to point
//          at the curve region and land on a header that actually decodes, and a slot that
//          fails both readings is left empty, which the caller reads as "hold the bind pose".
static bool ResolveCurve(const uint8_t* entry, const uint8_t* block, const size_t dataSize,
                         const size_t curvesBegin, const size_t attribSize,
                         size_t& outOffset, size_t& outSize, bool& outBigEndian) {
    const uint32_t candidates[2] = { ReadBE32(entry), ReadLE32(entry) };

    for (const uint32_t off : candidates) {
        if (off < curvesBegin || (scast<size_t>(off) + 4) > dataSize) {
            continue;
        }

        uint32_t header = 0;
        if (DecodeCurveHeader(block + off, attribSize, dataSize - off, header, outSize, outBigEndian)) {
            outOffset = off;
            return true;
        }
    }

    return false;
}

bool MetroMotion::LoadInternal() {
    bool result = false;

    if (mMotionsData.empty() || mMotionsData.size() <= mMotionsOffsetsSize) {
        return false;
    }

    const uint8_t* ptr = mMotionsData.data();
    const size_t dataSize = mMotionsData.size();

    // bonesMask(mMaskBytes) + numLocators(2) + numXforms(2) + totalSize(4) + unknown(8)
    const size_t headerSize = mMaskBytes + 2 + 2 + 4 + 8;
    if (dataSize < headerSize) {
        return false;
    }

    Bitset256& bonesMask = mMotionBonesMask;
    memset(&bonesMask, 0, sizeof(bonesMask));

    //#NOTE_SK: the mask inside the data block follows that block's big endian convention,
    //          unlike mAffectedBones which comes from the little endian info chunk. Read the
    //          wrong way round it points at bones past the end of the skeleton and nothing
    //          ever intersects, which is why every animation came out empty.
    if (mReduxLayout) {
        for (size_t i = 0; (i + 4) <= mMaskBytes && (i / 4) < 8; i += 4) {
            bonesMask.dwords[i / 4] = ReadBE32(ptr + i);
        }
    } else {
        memcpy(&bonesMask, ptr, mMaskBytes);
    }

    const uint8_t* offsetsTable = ptr + headerSize;

    //#NOTE_SK: the table lives between the block header and the first curve, and the size the
    //          info chunk gave us is exactly where the curves begin. Bounding by that instead
    //          of by the whole block keeps a misread motion from reading curve payloads as if
    //          they were offsets.
    const size_t offsetsEnd = (mMotionsOffsetsSize > headerSize && mMotionsOffsetsSize <= dataSize)
                                ? mMotionsOffsetsSize : dataSize;
    const size_t maxOffsetEntries = (offsetsEnd - headerSize) / sizeof(uint32_t);

    mBonesRotations.resize(mNumBones);
    mBonesPositions.resize(mNumBones);

    // one bone occupies three consecutive table slots: rotation, position, and a scale
    // curve that these games always leave empty
    const size_t kSlotsPerBone = 3;

    for (size_t boneIdx = 0, flatIdx = 0; boneIdx < mNumBones; ++boneIdx) {
        //#NOTE_SK: the block mask alone - see IsBoneAnimated. Bringing the info chunk's mask
        //          into this is what misaligned the table.
        if (bonesMask.IsPresent(boneIdx)) {
            //#NOTE_SK: every offset here comes straight off the wire - bound both the table
            //          lookup and the curve pointers, otherwise a motion we misread sends
            //          ReadAttributeCurve wandering outside the buffer
            if (((flatIdx * kSlotsPerBone) + kSlotsPerBone) > maxOffsetEntries) {
                break;
            }

            for (size_t slot = 0; slot < 2; ++slot) {   // 0 = rotation, 1 = position
                const size_t attribSize = (slot == 0) ? 4 : 3;

                size_t curveOffset = 0, curveSize = 0;
                bool be = true;
                if (!ResolveCurve(offsetsTable + (((flatIdx * kSlotsPerBone) + slot) * 4),
                                  ptr, dataSize, offsetsEnd, attribSize,
                                  curveOffset, curveSize, be)) {
                    ++mRefusedCurves;
                    continue;
                }

                this->ReadAttributeCurve(ptr + curveOffset,
                                         (slot == 0) ? mBonesRotations[boneIdx] : mBonesPositions[boneIdx],
                                         attribSize, be);
            }

            ++flatIdx;
        }
    }

    result = true;

    return result;
}

//#NOTE_SK: the Redux motion data block is big endian throughout - not just its offset table
//          but the curve payloads too. These little helpers keep the two cases side by side
//          instead of duplicating the whole decoder.
namespace {

    inline uint16_t ReadU16(const uint8_t* p, const bool be) {
        return be ? scast<uint16_t>((scast<uint16_t>(p[0]) << 8) | p[1])
                  : scast<uint16_t>((scast<uint16_t>(p[1]) << 8) | p[0]);
    }

    inline int16_t ReadS16(const uint8_t* p, const bool be) {
        return scast<int16_t>(ReadU16(p, be));
    }

    inline uint32_t ReadU32(const uint8_t* p, const bool be) {
        return be ? ((scast<uint32_t>(p[0]) << 24) | (scast<uint32_t>(p[1]) << 16) | (scast<uint32_t>(p[2]) << 8) | p[3])
                  : ((scast<uint32_t>(p[3]) << 24) | (scast<uint32_t>(p[2]) << 16) | (scast<uint32_t>(p[1]) << 8) | p[0]);
    }

    inline float ReadF32(const uint8_t* p, const bool be) {
        const uint32_t bits = ReadU32(p, be);
        float f;
        memcpy(&f, &bits, sizeof(f));
        return f;
    }

} // anonymous namespace

void MetroMotion::ReadAttributeCurve(const uint8_t* curveData, AttributeCurve& curve, const size_t attribSize, const bool bigEndian) {
    const bool be = bigEndian;

    const uint32_t curveHeader = ReadU32(curveData, be);

    const size_t numPoints = scast<size_t>(curveHeader & 0xFFFF);
    const AttribCurveType ctype = scast<AttribCurveType>((curveHeader >> 16) & 0xF);

    assert(attribSize <= 4);

    curveData += 4;

    if (ctype == AttribCurveType::Empty) {
        curve.points.clear();
    } else if (ctype == AttribCurveType::OneValue) {
        curve.points.resize(1);
        auto& p = curve.points.back();
        p.time = 0.0f;
        for (size_t i = 0; i < attribSize; ++i) {
            (&p.value.x)[i] = ReadF32(curveData + (i * 4), be);
        }

        p.value = MetroSwizzle(p.value);
    } else if (ctype == AttribCurveType::Unknown_3 || ctype == AttribCurveType::Unknown_6) {
        curve.points.clear();
    } else {
        curve.points.resize(numPoints);

        switch (ctype) {
            case AttribCurveType::Uncompressed: {
                const uint8_t* timingsPtr = curveData;
                const uint8_t* valuesPtr = curveData + (numPoints * sizeof(float));

                for (auto& p : curve.points) {
                    p.time = ReadF32(timingsPtr, be);
                    for (size_t i = 0; i < attribSize; ++i) {
                        (&p.value.x)[i] = ReadF32(valuesPtr + (i * 4), be);
                    }

                    p.value = MetroSwizzle(p.value);

                    timingsPtr += sizeof(float);
                    valuesPtr += attribSize * sizeof(float);
                }
            } break;

            case AttribCurveType::CompressedPos: {
                const float timingScale = 1.0f / ReadF32(curveData, be);
                curveData += 4;

                const vec3 scale(ReadF32(curveData + 0, be), ReadF32(curveData + 4, be), ReadF32(curveData + 8, be));
                const vec3 offset(ReadF32(curveData + 12, be), ReadF32(curveData + 16, be), ReadF32(curveData + 20, be));
                curveData += sizeof(vec3[2]);

                const uint8_t* timingsPtr = curveData;
                const uint8_t* valuesPtr = curveData + (numPoints * sizeof(uint16_t));

                for (auto& p : curve.points) {
                    p.time = scast<float>(ReadU16(timingsPtr, be)) * timingScale;

                    p.value.x = scast<float>(ReadU16(valuesPtr + 0, be)) * scale.x + offset.x;
                    p.value.y = scast<float>(ReadU16(valuesPtr + 2, be)) * scale.y + offset.y;
                    p.value.z = scast<float>(ReadU16(valuesPtr + 4, be)) * scale.z + offset.z;

                    p.value = MetroSwizzle(p.value);

                    timingsPtr += sizeof(uint16_t);
                    valuesPtr += 3 * sizeof(uint16_t);
                }
            } break;

            case AttribCurveType::CompressedQuat: {
                const float normFactor = 0.0000215805f;

                const float timingScale = 1.0f / ReadF32(curveData, be);
                curveData += 4;

                const uint8_t* timingsPtr = curveData;
                const uint8_t* valuesPtr = curveData + (numPoints * sizeof(uint16_t));

                for (auto& p : curve.points) {
                    p.time = scast<float>(ReadU16(timingsPtr, be)) * timingScale;

                    const int16_t v0 = ReadS16(valuesPtr + 0, be);
                    const int16_t v1 = ReadS16(valuesPtr + 2, be);
                    const int16_t v2 = ReadS16(valuesPtr + 4, be);

                    const int permutation = (v1 & 1) | (2 * (v0 & 1));
                    const int wsign = (v2 & 1);

                    const float qx = scast<float>(v0) * normFactor;
                    const float qy = scast<float>(v1) * normFactor;
                    const float qz = scast<float>(v2) * normFactor;
                    const float t = 1.0f - (qx * qx) - (qy * qy) - (qz * qz);
                    const float qw = (t < 0.0f) ? 0.0f : (wsign ? -std::sqrtf(t) : std::sqrtf(t));

                    switch (permutation) {
                        case 0: p.value = vec4(qw, qx, qy, qz); break;
                        case 1: p.value = vec4(qx, qw, qy, qz); break;
                        case 2: p.value = vec4(qx, qy, qw, qz); break;
                        case 3: p.value = vec4(qx, qy, qz, qw); break;
                    }

                    p.value = MetroSwizzle(p.value);

                    *rcast<quat*>(&p.value) = Normalize(*rcast<quat*>(&p.value));

                    timingsPtr += sizeof(uint16_t);
                    valuesPtr += 3 * sizeof(uint16_t);
                }
            } break;
        }
    }
}
