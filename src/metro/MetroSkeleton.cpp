#include "MetroSkeleton.h"
#include "MetroBinArchive.h"
#include "MetroReflection.h"

enum SkeletonChunks : size_t {
    SC_SelfData             = 0x00000001,
    SC_StringsDictionary    = 0x00000002,
};


void ParentMapped::Serialize(MetroReflectionReader& reader) {
    METRO_READ_MEMBER(reader, parent_bone);
    METRO_READ_MEMBER(reader, self_bone);
    METRO_READ_MEMBER(reader, q);
    METRO_READ_MEMBER(reader, t);
    METRO_READ_MEMBER(reader, s);
}

//#NOTE_SK: skeleton versions differ a lot between the games - Metro 2033 / Last Light Redux
//          write version 7, Exodus writes 21+. Bone records grew a field along the way, and
//          MetroBone::Serialize has no other way to know which layout it is looking at.
static uint32_t gCurrentSkeletonVersion = 0;

void MetroBone::Serialize(MetroReflectionReader& reader) {
    METRO_READ_MEMBER(reader, name);
    METRO_READ_MEMBER(reader, parent);
    METRO_READ_MEMBER(reader, q);
    METRO_READ_MEMBER(reader, t);

    //#NOTE_SK: the same two bytes are described differently by different games. Exodus writes
    //          them as bp(u8) followed by bpf(u8); the Redux games write a single bp(u16).
    //          Byte-wise it is identical, but these archives are self-describing, so reading
    //          an inline Redux skeleton with the Exodus field list made VerifyTypeInfo consume
    //          the following data as if it were a property name - and from there the whole
    //          bone array was garbage: every bone after the first came out nameless and
    //          parentless, which is to say with no hierarchy for the animation to drive.
    if (gCurrentSkeletonVersion > 18) {
        uint8_t bpByte = 0;
        reader.VerifyTypeInfo("bp", MetroTypeGetAlias<uint8_t>());
        reader >> bpByte;
        bp = bpByte;

        METRO_READ_MEMBER(reader, bpf);
    } else {
        METRO_READ_MEMBER(reader, bp);
        bpf = 0;
    }
}


MetroSkeleton::MetroSkeleton() {

}
MetroSkeleton::~MetroSkeleton() {

}

bool MetroSkeleton::LoadFromData(MemStream& stream) {
    bool result = false;

    MetroBinArchive bin(kEmptyString, stream, MetroBinArchive::kHeaderDoAutoSearch);
    MetroReflectionReader reader = bin.ReflectionReader();
    if (reader.Good()) {
        this->DeserializeSelf(reader);
        result = !this->bones.empty();
    }

    return result;
}

size_t MetroSkeleton::GetNumBones() const {
    return this->bones.size();
}

// The motions folders are shared, so a folder can hold motions built for a different
// skeleton that happens to have the same number of bones. Only the CRC tells them apart.
uint32_t MetroSkeleton::GetBonesCRC() const {
    return this->crc;
}

const quat& MetroSkeleton::GetBoneRotation(const size_t idx) const {
    return this->bones[idx].q;
}

const vec3& MetroSkeleton::GetBonePosition(const size_t idx) const {
    return this->bones[idx].t;
}

mat4 MetroSkeleton::GetBoneTransform(const size_t idx) const {
    mat4 result = MatFromQuat(this->bones[idx].q);
    result[3] = vec4(this->bones[idx].t, 1.0f);

    return result;
}

mat4 MetroSkeleton::GetBoneFullTransform(const size_t idx) const {
    //#NOTE_SK: this used to recurse on the parent with no cycle detection whatsoever, so a
    //          skeleton whose parent links loop back on themselves blew the stack and took the
    //          process with it - which is exactly what a Metro 2033 Redux skeleton did. Walking
    //          up iteratively with a hard bound on the chain length cannot do that: no chain
    //          can be longer than the number of bones.
    if (idx >= this->bones.size()) {
        return MatIdentity;
    }

    mat4 result = this->GetBoneTransform(idx);

    size_t parentIdx = this->GetBoneParentIdx(idx);
    for (size_t guard = 0; parentIdx != MetroBone::InvalidIdx && guard < this->bones.size(); ++guard) {
        result = this->GetBoneTransform(parentIdx) * result;
        parentIdx = this->GetBoneParentIdx(parentIdx);
    }

    return result;
}

const size_t MetroSkeleton::GetBoneParentIdx(const size_t idx) const {
    size_t result = MetroBone::InvalidIdx;

    if (idx >= this->bones.size()) {
        return result;
    }

    const CharString& parentName = this->bones[idx].parent;

    //#NOTE_SK: an empty parent name means "this is a root". Searching for it would happily
    //          pair every unnamed bone with the first unnamed bone - including with itself -
    //          and that is a cycle in the hierarchy.
    if (parentName.empty()) {
        return result;
    }

    for (size_t i = 0; i < this->bones.size(); ++i) {
        if (i != idx && this->bones[i].name == parentName) {
            result = i;
            break;
        }
    }

    return result;
}

const CharString& MetroSkeleton::GetBoneName(const size_t idx) const {
    return this->bones[idx].name;
}

const CharString& MetroSkeleton::GetMotionsStr() const {
    return this->motions;
}


//#NOTE_SK: skeleton versions differ a lot between the games - Metro 2033 / Last Light Redux
//          write version 7, Exodus writes 21+. The field list grew over time, and reading a
//          version 7 skeleton with the Exodus layout used to run the reflection cursor off
//          the end and segfault. The gates below follow the version notes that were already
//          in this function as comments.

void MetroSkeleton::DeserializeSelf(MetroReflectionReader& reader) {
    MetroReflectionReader skeletonReader = reader.OpenSection("skeleton");
    if (skeletonReader.Good()) {
        METRO_READ_MEMBER(skeletonReader, ver);
        METRO_READ_MEMBER(skeletonReader, crc);

        gCurrentSkeletonVersion = this->ver;

        if (this->ver < 15) {
            METRO_READ_MEMBER(skeletonReader, facefx);
        }
        if (this->ver > 16) {
            METRO_READ_MEMBER(skeletonReader, pfnn);
        }
        if (this->ver > 20) {
            METRO_READ_MEMBER(skeletonReader, has_as);
        }

        METRO_READ_MEMBER(skeletonReader, motions);

        if (this->ver > 12) {
            METRO_READ_MEMBER(skeletonReader, source_info);
        }
        if (this->ver > 13) {
            METRO_READ_MEMBER(skeletonReader, parent_skeleton);
            METRO_READ_STRUCT_ARRAY_MEMBER(skeletonReader, parent_bone_maps);
        }

        METRO_READ_STRUCT_ARRAY_MEMBER(skeletonReader, bones);
    }
    reader.CloseSection(skeletonReader);

    //#NOTE_SK: fix-up bones transforms by swizzling them back
    for (auto& b : bones) {
        b.q = MetroSwizzle(b.q);
        b.t = MetroSwizzle(b.t);
    }

    //#TODO_SK:
    // locators
    // aux_bones
    // procedural
}
