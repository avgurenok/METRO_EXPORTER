#include "MetroModel.h"
#include "VFXReader.h"
#include "MetroTexturesDatabase.h"
#include "MetroTexture.h"
#include "MetroSkeleton.h"
#include "MetroMotion.h"
#include "MetroLevel.h"

#define FBXSDK_NEW_API
#define FBXSDK_SHARED
#include "fbxsdk.h"

#pragma comment (lib, "libfbxsdk.lib")

#include <fstream>
#include <sstream>

enum ModelChunks {
    MC_HeaderChunk          = 0x00000001,
    MC_MaterialsChunk       = 0x00000002,
    MC_VerticesChunk        = 0x00000003,
    MC_FacesChunk           = 0x00000004,
    MC_SkinnedVerticesChunk = 0x00000005,

    MC_SubMeshesChunk       = 0x00000009,

    MC_Lod_1_Chunk          = 0x0000000B,   // 11
    MC_Lod_2_Chunk          = 0x0000000C,   // 12

    MC_MeshesInline         = 0x0000000F,   // 15
    MC_MeshesLinks          = 0x00000010,   // 16

    MC_SkeletonLink         = 0x00000014,   // 20
    MC_SkeletonInline       = 0x00000018,   // 24
};

static const size_t kMetroModelMaxMaterials = 4;


PACKED_STRUCT_BEGIN
struct MdlHeader {          // size = 64
    enum : uint32_t {
        Flag_ModelIsDraft = 1
    };

    uint8_t     version;
    uint8_t     type;
    uint16_t    shaderId;
    AABBox      bbox;
    vec4        bsphere;
    uint32_t    checkSum;
    float       invLod;
    uint32_t    flags;
    float       vscale;
    float       texelDensity;
} PACKED_STRUCT_END;

PACKED_STRUCT_BEGIN
struct MetroOBB {           // size = 60
    mat3    matrix;
    vec3    offset;
    vec3    hsize;
} PACKED_STRUCT_END;



MetroModel::MetroModel()
    : mSkeleton(nullptr)
    , mCurrentMesh(nullptr)
    , mVFXReader(nullptr)
    , mThisFileIdx(MetroFile::InvalidFileIdx)
    , mParseError(false)
    , mReduxAssets(false)
{
}
MetroModel::~MetroModel() {
    std::for_each(mMeshes.begin(), mMeshes.end(), [](MetroMesh* mesh) { delete mesh; });
    std::for_each(mMotions.begin(), mMotions.end(), [](MetroMotion* motion) { delete motion; });
    MySafeDelete(mSkeleton);
}

bool MetroModel::LoadFromLevel(VFXReader* vfxReader, const size_t descriptionFileIdx) {
    MetroLevel level;
    if (!level.LoadFromData(vfxReader, descriptionFileIdx)) {
        return false;
    }

    mVFXReader = vfxReader;
    mThisFileIdx = descriptionFileIdx;

    std::for_each(mMeshes.begin(), mMeshes.end(), [](MetroMesh* m) { delete m; });
    mMeshes = level.DetachMeshes();

    mBBox = level.GetBBox();

    const vec3 centre = mBBox.Center();
    const vec3 extent = mBBox.maximum - centre;
    mBSphere = vec4(centre, Length(extent));

    return !mMeshes.empty();
}

bool MetroModel::LoadFromData(MemStream& stream, VFXReader* vfxReader, const size_t fileIdx) {
    bool result = false;

    mVFXReader = vfxReader;
    mThisFileIdx = fileIdx;
    mReduxAssets = (vfxReader != nullptr) && (vfxReader->GetVersion() < 3);

    this->ReadSubChunks(stream);

    this->LoadMotions();

    result = !mParseError && !mMeshes.empty();

    return result;
}

// Resolves the archive lookup names for a mesh material's diffuse and bump textures.
//
// The sidecar file written next to an exported model is always the leaf of these names plus
// ".tga", and both the exporter and MainForm::ExtractModel go through here - otherwise the
// two disagree and the model references a file nobody ever wrote.
//
//#NOTE_SK: the textures database only exists in Exodus archives - the Redux games ship no
//          textures_handles_storage.bin at all, so `database` is null there and calling into
//          it threw a NullReferenceException right out of the FBX exporter. Without the
//          database the material name doubles as the texture name and the normal map is the
//          same name with a "_bump" suffix, which is how the older games address them.
static void ResolveTextureNames(const MetroTexturesDatabase* database,
                                const CharString& materialName,
                                CharString& outDiffuse,
                                CharString& outBump) {
    outDiffuse = materialName;
    outBump.clear();

    if (database) {
        const CharString& src = database->GetSourceName(materialName);
        if (!src.empty()) {
            outDiffuse = src;
        }
        outBump = database->GetBumpName(materialName);
    } else if (!materialName.empty()) {
        outBump = materialName + "_bump";
    }
}

// The file name an exported model refers to for a given texture.
static CharString TextureSidecarName(const CharString& textureName) {
    return fs::path(textureName).filename().u8string() + ".png";
}

// Pulls a texture out of the archive and drops it next to the exported model.
//
//#NOTE_SK: this deliberately lives inside the exporter rather than in the UI. When the two
//          were separate they had to agree on the file name and on the order of operations,
//          and they did not - the model referenced files nobody had written, which is how you
//          end up with pink materials on import. Now whoever writes the .fbx also writes the
//          images it points at, and there is nothing left to keep in sync.
//
//          PNG rather than TGA: same pixels, but a 2048 normal map is a couple of megabytes
//          instead of sixteen, which matters a lot once the images are embedded as well.
static CharString WriteTextureSidecar(VFXReader* vfxReader, const CharString& textureName, const fs::path& folder) {
    if (!vfxReader || textureName.empty()) {
        return kEmptyString;
    }

    // highest resolution first; the "c" variants are Crunch, which MetroTexture transcodes
    static const char* kExts[] = { ".2048", ".2048c", ".1024", ".1024c", ".512", ".512c", ".dds" };

    CharString basePath("content");
    basePath += static_cast<char>(92);   // backslash, kept out of literals on purpose
    basePath += "textures";
    basePath += static_cast<char>(92);   // backslash, kept out of literals on purpose
    basePath += textureName;

    size_t textureIdx = MetroFile::InvalidFileIdx;
    for (const char* ext : kExts) {
        textureIdx = vfxReader->FindFile(basePath + ext);
        if (textureIdx != MetroFile::InvalidFileIdx) {
            break;
        }
    }

    if (textureIdx == MetroFile::InvalidFileIdx) {
        LogPrintF(LogLevel::Warning, "no texture file in the archive for '%s'", textureName.c_str());
        return kEmptyString;
    }

    const CharString outName = TextureSidecarName(textureName);
    const fs::path outPath = folder / outName;

    std::error_code ec;
    if (fs::exists(outPath, ec)) {
        // several materials commonly share one texture
        return outName;
    }

    MemStream stream = vfxReader->ExtractFile(textureIdx);
    if (!stream) {
        return kEmptyString;
    }

    const MetroFile& mf = vfxReader->GetFile(textureIdx);

    MetroTexture texture;
    if (!texture.LoadFromData(stream, mf.name)) {
        LogPrintF(LogLevel::Warning, "could not decode texture '%s'", mf.name.c_str());
        return kEmptyString;
    }

    //#NOTE_SK: normal maps never want an alpha channel wired anywhere, and for the diffuse
    //          we let the heuristic decide - see MetroTexture::PNGAlpha
    const bool isNormalMap = (textureName.size() > 5) && (textureName.compare(textureName.size() - 5, 5, "_bump") == 0);
    const MetroTexture::PNGAlpha alphaMode = isNormalMap ? MetroTexture::PNGAlpha::Drop : MetroTexture::PNGAlpha::Auto;

    if (!texture.SaveAsPNG(outPath, alphaMode)) {
        LogPrintF(LogLevel::Error, "could not write '%s'", outPath.u8string().c_str());
        return kEmptyString;
    }

    return outName;
}

bool MetroModel::SaveAsOBJ(const fs::path& filePath, VFXReader* vfxReader, MetroTexturesDatabase* database) {
    bool result = false;

    const bool exportWithMats = (vfxReader != nullptr);

    std::ofstream file(filePath, std::ofstream::binary);
    if (file.good()) {
        CharString matName = filePath.filename().string();
        matName[matName.size() - 3] = 'm';
        matName[matName.size() - 2] = 't';
        matName[matName.size() - 1] = 'l';

        std::ostringstream stringBuilder;
        stringBuilder << "# Generated from Metro Exodus model file" << std::endl;
        stringBuilder << "# using MetroEX tool made by iOrange, 2019" << std::endl << std::endl;

        if (exportWithMats) {
            stringBuilder << "mtllib " << matName << std::endl << std::endl;
        }

        size_t lastIdx = 0;
        for (size_t i = 0; i < mMeshes.size(); ++i) {
            const MetroMesh* mesh = mMeshes[i];
            if (!mesh->vertices.empty() && !mesh->faces.empty()) {
                for (const MetroVertex& v : mesh->vertices) {
                    stringBuilder << "v " << v.pos.x << ' ' << v.pos.y << ' ' << v.pos.z << std::endl;
                }
                stringBuilder << "# " << mesh->vertices.size() << " vertices" << std::endl << std::endl;

                for (const MetroVertex& v : mesh->vertices) {
                    stringBuilder << "vt " << v.uv0.x << ' ' << (1.0f - v.uv0.y) << std::endl;
                }
                stringBuilder << "# " << mesh->vertices.size() << " texcoords" << std::endl << std::endl;

                for (const MetroVertex& v : mesh->vertices) {
                    stringBuilder << "vn " << v.normal.x << ' ' << v.normal.y << ' ' << v.normal.z << std::endl;
                }
                stringBuilder << "# " << mesh->vertices.size() << " normals" << std::endl << std::endl;

                stringBuilder << "g Mesh_" << i << std::endl;
                if (exportWithMats) {
                    stringBuilder << "usemtl " << "Material_" << i << std::endl;
                }

                for (const MetroFace& f : mesh->faces) {
                    const size_t a = f.c + lastIdx + 1;
                    const size_t b = f.b + lastIdx + 1;
                    const size_t c = f.a + lastIdx + 1;

                    stringBuilder << "f " << a << '/' << a << '/' << a <<
                                      ' ' << b << '/' << b << '/' << b <<
                                      ' ' << c << '/' << c << '/' << c << std::endl;
                }
                stringBuilder << "# " << mesh->faces.size() << " faces" << std::endl << std::endl;

                lastIdx += mesh->vertices.size();
            }
        }

        const CharString& str = stringBuilder.str();
        file.write(str.c_str(), str.length());
        file.flush();

        if (exportWithMats) {
            fs::path modelFolder = filePath.parent_path();

            CharString matPath = filePath.string();
            matPath[matPath.size() - 3] = 'm';
            matPath[matPath.size() - 2] = 't';
            matPath[matPath.size() - 1] = 'l';

            std::ofstream mtlFile(matPath, std::ofstream::binary);
            if (mtlFile.good()) {
                std::ostringstream mtlBuilder;
                mtlBuilder << "# Generated from Metro Exodus model file" << std::endl;
                mtlBuilder << "# using MetroEX tool made by iOrange, 2019" << std::endl << std::endl;

                for (size_t i = 0; i < mMeshes.size(); ++i) {
                    const MetroMesh* mesh = mMeshes[i];
                    if (!mesh->vertices.empty() && !mesh->faces.empty()) {
                        mtlBuilder << "newmtl " << "Material_" << i << std::endl;

                        const CharString& textureName = mesh->materials.empty() ? kEmptyString : mesh->materials.front();

                        CharString sourceName, bumpName;
                        ResolveTextureNames(database, textureName, sourceName, bumpName);

                        const CharString textureTgaName = TextureSidecarName(sourceName);

                        mtlBuilder << "Kd 1 1 1" << std::endl;
                        mtlBuilder << "Ke 0 0 0" << std::endl;
                        mtlBuilder << "Ns 1000" << std::endl;
                        mtlBuilder << "illum 2" << std::endl;
                        mtlBuilder << "map_Ka " << textureTgaName << std::endl;
                        mtlBuilder << "map_Kd " << textureTgaName << std::endl;

                        if (!bumpName.empty()) {
                            const CharString bumpTgaName = TextureSidecarName(bumpName);
                            mtlBuilder << "bump " << bumpTgaName << std::endl;
                            mtlBuilder << "map_bump " << bumpTgaName << std::endl;
                        }

                        mtlBuilder << std::endl;
                    }
                }

                const CharString& mtlStr = mtlBuilder.str();
                mtlFile.write(mtlStr.c_str(), mtlStr.length());
                mtlFile.flush();
            }
        }

        result = true;
    }

    return result;
}

struct ClusterInfo {
    MyArray<int>      vertexIdxs;
    MyArray<float>    weigths;
};
void CollectClusters(const MetroMesh* mesh, const MetroSkeleton* skeleton, MyArray<ClusterInfo>& clusters) {
    const size_t numBones = skeleton->GetNumBones();
    clusters.resize(numBones);

    for (size_t i = 0; i < numBones; ++i) {
        ClusterInfo& cluster = clusters[i];

        for (size_t j = 0; j < mesh->vertices.size(); ++j) {
            const MetroVertex& v = mesh->vertices[j];

            for (size_t k = 0; k < 4; ++k) {
                const size_t boneIdx = v.bones[k];
                if (boneIdx == i && v.weights[k]) {
                    cluster.vertexIdxs.push_back(scast<int>(j));
                    cluster.weigths.push_back(scast<float>(v.weights[k]) * (1.0f / 255.0f));
                }
            }
        }
    }
}


static FbxVector4 MetroVecToFbxVec(const vec3& v) {
    return FbxVector4(v.x, v.y, v.z);
}

static FbxVector4 MetroRotToFbxRot(const quat& q) {
    vec3 euler = QuatToEuler(q);
    return FbxVector4(Rad2Deg(euler.x), Rad2Deg(euler.y), Rad2Deg(euler.z));
}


//#NOTE_SK: a skeleton can legitimately have more than one parentless bone - the Redux ones
//          routinely do. The old version kept only the last of them and silently dropped the
//          other subtrees outside the scene graph, which leaves their bones unparented in the
//          exported file; importers then have nothing sane to bind the skin to and the
//          animation appears to do nothing. Every root is handed back now.
static void CreateFBXSkeleton(FbxScene* scene, const MetroSkeleton* skeleton, MyArray<FbxNode*>& boneNodes, MyArray<FbxNode*>& rootNodes) {
    const size_t numBones = skeleton->GetNumBones();
    boneNodes.reserve(numBones);

    for (size_t i = 0; i < numBones; ++i) {
        const size_t parentIdx = skeleton->GetBoneParentIdx(i);
        const CharString& name = skeleton->GetBoneName(i);

        FbxSkeleton* attribute = FbxSkeleton::Create(scene, name.c_str());
        if (MetroBone::InvalidIdx == parentIdx) {
            attribute->SetSkeletonType(FbxSkeleton::eRoot);
        } else {
            attribute->SetSkeletonType(FbxSkeleton::eLimbNode);
        }

        FbxNode* node = FbxNode::Create(scene, name.c_str());
        node->SetNodeAttribute(attribute);

        boneNodes.push_back(node);
    }

    for (size_t i = 0; i < numBones; ++i) {
        FbxNode* node = boneNodes[i];
        const size_t parentIdx = skeleton->GetBoneParentIdx(i);

        const quat& bindQ = skeleton->GetBoneRotation(i);
        const vec3& bindT = skeleton->GetBonePosition(i);

        node->LclTranslation.Set(MetroVecToFbxVec(bindT));
        node->LclRotation.Set(MetroRotToFbxRot(bindQ));

        if (MetroBone::InvalidIdx != parentIdx && parentIdx < numBones) {
            boneNodes[parentIdx]->AddChild(node);
        } else {
            rootNodes.push_back(node);
        }
    }
}

// The curve code doesn't differentiate between angles and other data, so an interpolation from 179 to -179
// will cause the bone to rotate all the way around through 0 degrees.  So here we make a second pass over the
// rotation tracks to convert the angles into a more interpolation-friendly format.
static void CorrectAnimTrackInterpolation(MyArray<FbxNode*>& boneNodes, FbxAnimLayer* animLayer) {
    for (FbxNode* bone : boneNodes) {
        FbxAnimCurveNode* rotCurveNode = bone->LclRotation.GetCurveNode(animLayer);
        if (rotCurveNode) {
            //#NOTE_SK: just because fucking FBX doesn't allow us to use quaternions for rotations
            //          we'll get angles "clicking" issues
            //          this is the only way I found to fight this issue
            FbxAnimCurveFilterUnroll unrollFilter;
            unrollFilter.SetForceAutoTangents(true);
            unrollFilter.Apply(*rotCurveNode);
        }
    }
}

static void AddAnimTrackToScene(FbxScene* scene, const MetroMotion* motion, const CharString& animName, MyArray<FbxNode*>& skelNodes) {
    FbxAnimStack* animStack = FbxAnimStack::Create(scene, animName.c_str());
    FbxAnimLayer* animLayer = FbxAnimLayer::Create(scene->GetFbxManager(), "Base_Layer");
    animStack->AddMember(animLayer);

    const double animFPS = 30.0f;

    FbxTime startTime, stopTime;
    startTime.SetGlobalTimeMode(FbxTime::eFrames30);
    stopTime.SetGlobalTimeMode(FbxTime::eFrames30);

    startTime.SetSecondDouble(0.0);

    // kinda hack to get animation duration
    const double animDuration = scast<double>(motion->GetMotionTimeInSeconds());
    stopTime.SetSecondDouble(animDuration);

    FbxTimeSpan animTimeSpan;
    animTimeSpan.Set(startTime, stopTime);
    animStack->SetLocalTimeSpan(animTimeSpan);


    FbxTime keyTime;
    int keyIndex;

    for (size_t i = 0; i < skelNodes.size(); ++i) {
        FbxNode* boneNode = skelNodes[i];

        if (motion->IsBoneAnimated(i)) {
            const auto& posCurve = motion->mBonesPositions[i];
            const auto& rotCurve = motion->mBonesRotations[i];

            boneNode->LclRotation.GetCurveNode(animLayer, true);
            boneNode->LclTranslation.GetCurveNode(animLayer, true);

            FbxAnimCurve* offsetCurve[3] = {
                boneNode->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_X, true),
                boneNode->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Y, true),
                boneNode->LclTranslation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Z, true)
            };

            offsetCurve[0]->KeyModifyBegin();
            offsetCurve[1]->KeyModifyBegin();
            offsetCurve[2]->KeyModifyBegin();

            //#NOTE_SK: an empty curve here used to leave the FBX track with no keys at all,
            //          and an empty track does not mean "unanimated" to an importer - Blender
            //          reads it as a constant zero and drops the bone onto its parent's
            //          origin. Since almost no bone has a position curve, and the facial bones
            //          often have neither, that is what turned every exported character's head
            //          into a fan of spikes. Pin the bind pose down with a single key instead;
            //          the node still carries it, CreateFBXSkeleton put it there.
            if (posCurve.points.empty()) {
                const FbxDouble3 bindT = boneNode->LclTranslation.Get();

                keyTime.SetSecondDouble(0.0);

                for (int k = 0; k < 3; ++k) {
                    keyIndex = offsetCurve[k]->KeyAdd(keyTime);
                    offsetCurve[k]->KeySetValue(keyIndex, scast<float>(bindT[k]));
                    offsetCurve[k]->KeySetInterpolation(keyIndex, FbxAnimCurveDef::eInterpolationConstant);
                }
            } else {
                if (posCurve.points.size() == 1) {
                    FbxVector4 fv = MetroVecToFbxVec(vec3(posCurve.points.front().value));

                    keyTime.SetSecondDouble(0.0);

                    for (int k = 0; k < 3; ++k) {
                        keyIndex = offsetCurve[k]->KeyAdd(keyTime);
                        offsetCurve[k]->KeySetValue(keyIndex, scast<float>(fv[k]));
                        offsetCurve[k]->KeySetInterpolation(keyIndex, FbxAnimCurveDef::eInterpolationLinear);
                    }
                } else {
                    for (auto& pt : posCurve.points) {
                        FbxVector4 fv = MetroVecToFbxVec(vec3(pt.value));

                        keyTime.SetSecondDouble(scast<double>(pt.time));

                        for (int k = 0; k < 3; ++k) {
                            keyIndex = offsetCurve[k]->KeyAdd(keyTime);
                            offsetCurve[k]->KeySetValue(keyIndex, scast<float>(fv[k]));
                            offsetCurve[k]->KeySetInterpolation(keyIndex, FbxAnimCurveDef::eInterpolationCubic);
                        }
                    }
                }
            }

            offsetCurve[0]->KeyModifyEnd();
            offsetCurve[1]->KeyModifyEnd();
            offsetCurve[2]->KeyModifyEnd();

            FbxAnimCurve* rotationCurve[3] = {
                boneNode->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_X, true),
                boneNode->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Y, true),
                boneNode->LclRotation.GetCurve(animLayer, FBXSDK_CURVENODE_COMPONENT_Z, true)
            };

            rotationCurve[0]->KeyModifyBegin();
            rotationCurve[1]->KeyModifyBegin();
            rotationCurve[2]->KeyModifyBegin();

            // same story for the rotation track - see the note above the offset one
            if (rotCurve.points.empty()) {
                const FbxDouble3 bindR = boneNode->LclRotation.Get();

                keyTime.SetSecondDouble(0.0);

                for (int k = 0; k < 3; ++k) {
                    keyIndex = rotationCurve[k]->KeyAdd(keyTime);
                    rotationCurve[k]->KeySetValue(keyIndex, scast<float>(bindR[k]));
                    rotationCurve[k]->KeySetInterpolation(keyIndex, FbxAnimCurveDef::eInterpolationConstant);
                }
            } else {
                if (rotCurve.points.size() == 1) {
                    FbxVector4 fv = MetroRotToFbxRot(*rcast<const quat*>(&rotCurve.points.front().value));

                    keyTime.SetSecondDouble(0.0);

                    for (int k = 0; k < 3; ++k) {
                        keyIndex = rotationCurve[k]->KeyAdd(keyTime);
                        rotationCurve[k]->KeySetValue(keyIndex, scast<float>(fv[k]));
                        rotationCurve[k]->KeySetInterpolation(keyIndex, FbxAnimCurveDef::eInterpolationLinear);
                    }
                } else {
                    for (auto& pt : rotCurve.points) {
                        FbxVector4 fv = MetroRotToFbxRot(*rcast<const quat*>(&pt.value));

                        keyTime.SetSecondDouble(scast<double>(pt.time));

                        for (int k = 0; k < 3; ++k) {
                            keyIndex = rotationCurve[k]->KeyAdd(keyTime);
                            rotationCurve[k]->KeySetValue(keyIndex, scast<float>(fv[k]));
                            rotationCurve[k]->KeySetInterpolation(keyIndex, FbxAnimCurveDef::eInterpolationCubic);
                        }
                    }
                }
            }

            rotationCurve[0]->KeyModifyEnd();
            rotationCurve[1]->KeyModifyEnd();
            rotationCurve[2]->KeyModifyEnd();
        }
    }

    CorrectAnimTrackInterpolation(skelNodes, animLayer);
}

bool MetroModel::SaveAsFBX(const fs::path& filePath, VFXReader* vfxReader, MetroTexturesDatabase* database,
                           const bool withAnims, const bool embedTextures) {
    FbxManager* mgr = FbxManager::Create();
    if (!mgr) {
        return false;
    }

    fs::path modelFolder = filePath.parent_path();

    FbxIOSettings* ios = FbxIOSettings::Create(mgr, IOSROOT);
    mgr->SetIOSettings(ios);

    FbxScene* scene = FbxScene::Create(mgr, "Metro model");

    MyDict<CharString, FbxSurfacePhong*> fbxMaterials;
    for (size_t i = 0; i < mMeshes.size(); ++i) {
        const MetroMesh* mesh = mMeshes[i];
        if (!mesh->vertices.empty() && !mesh->faces.empty() && !mesh->materials.empty()) {
            const CharString& textureName = mesh->materials.front();

            auto it = fbxMaterials.find(textureName);
            if (it == fbxMaterials.end()) {
                CharString sourceName, bumpName;
                ResolveTextureNames(database, textureName, sourceName, bumpName);

                // write the images first, then point the material at what actually exists
                const CharString textureTgaName = WriteTextureSidecar(vfxReader, sourceName, modelFolder);
                const CharString bumpTgaName = WriteTextureSidecar(vfxReader, bumpName, modelFolder);

                FbxFileTexture* texture = nullptr;
                if (!textureTgaName.empty()) {
                    const CharString texturePath = (modelFolder / textureTgaName).u8string();

                    texture = FbxFileTexture::Create(mgr, textureName.c_str());
                    texture->SetFileName(texturePath.c_str());
                    texture->SetRelativeFileName(textureTgaName.c_str());
                    texture->SetTextureUse(FbxTexture::eStandard);
                    texture->SetMappingType(FbxTexture::eUV);
                    texture->SetMaterialUse(FbxFileTexture::eModelMaterial);
                    texture->UVSwap = false;
                    texture->SetTranslation(0.0, 0.0);
                    texture->SetScale(1.0, 1.0);
                    texture->SetRotation(0.0, 0.0);
                    texture->SetAlphaSource(FbxTexture::eNone);
                }

                FbxFileTexture* bump = nullptr;
                if (!bumpTgaName.empty()) {
                    const CharString bumpPath = (modelFolder / bumpTgaName).u8string();

                    bump = FbxFileTexture::Create(mgr, bumpName.c_str());
                    bump->SetFileName(bumpPath.c_str());
                    bump->SetRelativeFileName(bumpTgaName.c_str());
                    bump->SetTextureUse(FbxTexture::eBumpNormalMap);
                    bump->SetMappingType(FbxTexture::eUV);
                    bump->SetMaterialUse(FbxFileTexture::eModelMaterial);
                    bump->UVSwap = false;
                    bump->SetTranslation(0.0, 0.0);
                    bump->SetScale(1.0, 1.0);
                    bump->SetRotation(0.0, 0.0);
                }

                FbxSurfacePhong* material = FbxSurfacePhong::Create(mgr, textureName.c_str());
                material->Emissive = FbxDouble3(0.0, 0.0, 0.0);
                material->Diffuse = FbxDouble3(1.0, 1.0, 1.0);
                material->DiffuseFactor = 1.0;
                material->Specular = FbxDouble3(0.0, 0.0, 0.0);
                //#NOTE_SK: the SDK defaults ReflectionFactor to 1.0, and Blender reads that
                //          straight into Metallic - which is why every imported material came
                //          out looking like polished chrome
                material->ReflectionFactor = 0.0;
                material->TransparencyFactor = 0.0;
                material->TransparentColor = FbxDouble3(0.0, 0.0, 0.0);
                material->SpecularFactor = 0.0;
                material->Shininess = 0.0; // simple diffuse

                if (texture) {
                    material->Diffuse.ConnectSrcObject(texture);
                }

                if (bump) {
                    //#NOTE_SK: NormalMap only. Connecting the same image to Bump as well makes
                    //          importers materialise a second, redundant texture node.
                    material->NormalMap.ConnectSrcObject(bump);
                    material->BumpFactor = 1.0;
                }

                fbxMaterials[textureName] = material;
            }
        }
    }

    MyArray<FbxNode*> meshNodes;
    MyArray<FbxMesh*> fbxMeshes;
    // fbxMeshes skips empty meshes, so remember which source mesh each entry came from
    MyArray<size_t> fbxMeshSrcIdx;
    for (size_t i = 0; i < mMeshes.size(); ++i) {
        const MetroMesh* mesh = this->GetMesh(i);
        if (mesh->vertices.empty() || mesh->faces.empty()) {
            //#NOTE_SK: an empty mesh has no material either, and exporting it as an empty
            //          node just litters the scene
            continue;
        }

        CharString meshName = CharString("mesh_") + std::to_string(i);

        FbxMesh* fbxMesh = FbxMesh::Create(scene, meshName.c_str());

        // assign vertices
        fbxMesh->InitControlPoints(scast<int>(mesh->vertices.size()));
        FbxVector4* ptrCtrlPoints = fbxMesh->GetControlPoints();

        FbxGeometryElementNormal* normalElement = fbxMesh->CreateElementNormal();
        normalElement->SetMappingMode(FbxGeometryElement::eByControlPoint);
        normalElement->SetReferenceMode(FbxGeometryElement::eDirect);

        FbxGeometryElementUV* uvElement = fbxMesh->CreateElementUV("uv0");
        uvElement->SetMappingMode(FbxGeometryElement::eByControlPoint);
        uvElement->SetReferenceMode(FbxGeometryElement::eDirect);

        for (const MetroVertex& v : mesh->vertices) {
            *ptrCtrlPoints = MetroVecToFbxVec(v.pos);
            normalElement->GetDirectArray().Add(MetroVecToFbxVec(v.normal));
            uvElement->GetDirectArray().Add(FbxVector2(v.uv0.x, 1.0f - v.uv0.y));

            ++ptrCtrlPoints;
        }

        FbxGeometryElementMaterial* materialElement = fbxMesh->CreateElementMaterial();
        materialElement->SetMappingMode(FbxGeometryElement::eAllSame);
        materialElement->SetReferenceMode(FbxGeometryElement::eIndexToDirect);
        materialElement->GetIndexArray().Add(0);

        // build polygons
        for (const MetroFace& face : mesh->faces) {
            fbxMesh->BeginPolygon();
            fbxMesh->AddPolygon(scast<int>(face.c));
            fbxMesh->AddPolygon(scast<int>(face.b));
            fbxMesh->AddPolygon(scast<int>(face.a));
            fbxMesh->EndPolygon();
        }

        FbxNode* meshNode = FbxNode::Create(scene, meshName.c_str());
        meshNode->SetNodeAttribute(fbxMesh);
        scene->GetRootNode()->AddChild(meshNode);

        const CharString& textureName = mesh->materials.empty() ? kEmptyString : mesh->materials.front();
        auto it = fbxMaterials.find(textureName);
        if (it != fbxMaterials.end()) {
            meshNode->SetShadingMode(FbxNode::eTextureShading);
            meshNode->AddMaterial(it->second);
        }

        fbxMeshes.push_back(fbxMesh);
        fbxMeshSrcIdx.push_back(i);
        meshNodes.push_back(meshNode);
    }

    if (mSkeleton) {
        MyArray<FbxNode*> boneNodes;
        MyArray<FbxNode*> rootBoneNodes;
        CreateFBXSkeleton(scene, mSkeleton, boneNodes, rootBoneNodes);

        for (FbxNode* rootBoneNode : rootBoneNodes) {
            scene->GetRootNode()->AddChild(rootBoneNode);
        }

        LogPrintF(LogLevel::Info, "FBX export: %zu bones, %zu root bone(s)", boneNodes.size(), rootBoneNodes.size());

        FbxPose* bindPose = FbxPose::Create(scene, "BindPose");
        bindPose->SetIsBindPose(true);

        for (FbxNode* node : boneNodes) {
            bindPose->Add(node, node->EvaluateGlobalTransform());
        }

        for (size_t i = 0; i < fbxMeshes.size(); ++i) {
            FbxMesh* fbxMesh = fbxMeshes[i];
            FbxNode* meshNode = meshNodes[i];

            const MetroMesh* mesh = mMeshes[fbxMeshSrcIdx[i]];

            FbxAMatrix meshXMatrix = meshNode->EvaluateGlobalTransform();

            MyArray<ClusterInfo> clusters;
            CollectClusters(mesh, mSkeleton, clusters);

            FbxSkin* skin = FbxSkin::Create(scene, "");
            for (size_t i = 0; i < clusters.size(); ++i) {
                const ClusterInfo& cluster = clusters[i];
                if (!cluster.vertexIdxs.empty()) {
                    FbxCluster* fbxCluster = FbxCluster::Create(scene, "");
                    FbxNode* linkNode = boneNodes[i];
                    fbxCluster->SetLink(linkNode);
                    fbxCluster->SetLinkMode(FbxCluster::eTotalOne);
                    for (size_t j = 0; j < cluster.vertexIdxs.size(); ++j) {
                        fbxCluster->AddControlPointIndex(cluster.vertexIdxs[j], cluster.weigths[j]);
                    }
                    fbxCluster->SetTransformMatrix(meshXMatrix);
                    fbxCluster->SetTransformLinkMatrix(linkNode->EvaluateGlobalTransform());
                    skin->AddCluster(fbxCluster);
                }
            }

            fbxMesh->AddDeformer(skin);
            bindPose->Add(meshNode, meshXMatrix);
        }

        scene->AddPose(bindPose);

        if (withAnims) {
            for (size_t i = 0; i < mMotions.size(); ++i) {
                const MetroMotion* motion = mMotions[i];
                AddAnimTrackToScene(scene, motion, motion->GetName(), boneNodes);
            }
        }
    }

    // now export all this
    FbxDocumentInfo* info = scene->GetSceneInfo();
    if (info) {
        info->Original_ApplicationVendor = FbxString("iOrange");
        info->Original_ApplicationName = FbxString("MetroEX");
        info->mTitle = FbxString("Metro Exodus model");
        info->mComment = FbxString("Exported using MetroEX created by iOrange");
    }

    FbxGlobalSettings& settings = scene->GetGlobalSettings();
    settings.SetAxisSystem(FbxAxisSystem(FbxAxisSystem::eOpenGL));
    settings.SetOriginalUpAxis(FbxAxisSystem(FbxAxisSystem::eOpenGL));
    //#NOTE_SK: Metro works in metres. FbxSystemUnit(1.0) means one centimetre per unit, so
    //          importers were scaling the model down by a hundred - a two metre character
    //          arrived two centimetres tall.
    settings.SetSystemUnit(FbxSystemUnit::m);

    // export
    FbxExporter* exp = FbxExporter::Create(mgr, "");
    const int format = mgr->GetIOPluginRegistry()->GetNativeWriterFormat();

    exp->SetFileExportVersion(FBX_2011_00_COMPATIBLE);

    ios->SetBoolProp(EXP_FBX_MATERIAL, true);
    ios->SetBoolProp(EXP_FBX_TEXTURE, true);
    //#NOTE_SK: carry the images inside the .fbx as well. The sidecar .png files are still
    //          written next to it, but embedding means the material survives the file being
    //          moved, mailed or dropped into a project on its own - which is the whole point
    //          of "the materials just work".
    //
    //          A whole map is the exception: it names over a hundred textures, and embedding
    //          those turned a 30 MB export into 262 MB, of which Blender then failed to unpack
    //          the normal maps at all. The sidecar files are right beside it, so a level is
    //          exported plain.
    ios->SetBoolProp(EXP_FBX_EMBEDDED, embedTextures);
    ios->SetBoolProp(EXP_FBX_SHAPE, true);
    ios->SetBoolProp(EXP_FBX_GOBO, true);
    ios->SetBoolProp(EXP_FBX_ANIMATION, withAnims);
    ios->SetBoolProp(EXP_FBX_GLOBAL_SETTINGS, true);

    if (exp->Initialize(filePath.u8string().c_str(), format, ios)) {
        if (!exp->Export(scene)) {
            //FbxStatus& status = exp->GetStatus();
            //std::cerr << status.GetErrorString() << std::endl;
            return false;
        }
    }

    exp->Destroy();
    mgr->Destroy();

    return true;
}

bool MetroModel::IsAnimated() const {
    return mSkeleton != nullptr;
}

const AABBox& MetroModel::GetBBox() const {
    return mBBox;
}

const vec4& MetroModel::GetBSphere() const {
    return mBSphere;
}

size_t MetroModel::GetNumMeshes() const {
    return mMeshes.size();
}

const MetroMesh* MetroModel::GetMesh(const size_t idx) const {
    return mMeshes[idx];
}

const CharString& MetroModel::GetSkeletonPath() const {
    return mSkeletonPath;
}

const MetroSkeleton* MetroModel::GetSkeleton() const {
    return mSkeleton;
}

size_t MetroModel::GetNumMotions() const {
    return mMotions.size();
}

const MetroMotion* MetroModel::GetMotion(const size_t idx) const {
    return mMotions[idx];
}


// for string delimiter
StringArray SplitString(const CharString& s, const char delimiter) {
    StringArray result;

    size_t pos_start = 0, pos_end;
    CharString token;

    while ((pos_end = s.find(delimiter, pos_start)) != CharString::npos) {
        token = s.substr(pos_start, pos_end - pos_start);
        pos_start = pos_end + 1;
        result.emplace_back(token);
    }

    result.push_back(s.substr(pos_start));
    return result;
}

static void RemapBones(MetroVertex& v, const BytesArray& remap) {
    v.bones[0] = remap[v.bones[0]];
    v.bones[1] = remap[v.bones[1]];
    v.bones[2] = remap[v.bones[2]];
    v.bones[3] = remap[v.bones[3]];
}

void MetroModel::ReadSubChunks(MemStream& stream) {
    while (!stream.Ended()) {
        const size_t chunkId = stream.ReadTyped<uint32_t>();
        const size_t chunkSize = stream.ReadTyped<uint32_t>();
        const size_t chunkEnd = stream.GetCursor() + chunkSize;

        switch (chunkId) {
            case MC_HeaderChunk: {
                MdlHeader hdr;
                stream.ReadStruct(hdr);

                if (hdr.vscale <= MM_Epsilon) {
                    hdr.vscale = 1.0f;
                }

                if (mCurrentMesh) {
                    mCurrentMesh->vscale = hdr.vscale;
                    mCurrentMesh->bbox = hdr.bbox;
                    mCurrentMesh->type = hdr.type;
                    mCurrentMesh->shaderId = hdr.shaderId;
                } else {
                    mBBox = hdr.bbox;
                    mBSphere = hdr.bsphere;
                }
            } break;

            case MC_MaterialsChunk: {
                if (!mCurrentMesh) {
                    return;
                }

                mCurrentMesh->materials.resize(kMetroModelMaxMaterials);
                for (auto& s : mCurrentMesh->materials) {
                    s = stream.ReadStringZ();
                }
            } break;

            case MC_VerticesChunk: {
                if (mCurrentMesh) {
                    mCurrentMesh->skinned = false;

                    const size_t vertexType = stream.ReadTyped<uint32_t>();
                    const size_t numVertices = stream.ReadTyped<uint32_t>();

                    //#NOTE_SK: Exodus keeps a shadow geometry vertex count here, Redux does not
                    if (!mReduxAssets) {
                        const size_t numShadowVertices = stream.ReadTyped<uint16_t>();
                    }

                    // never trust the count - a model from a game we do not understand would
                    // otherwise send us reading structs way past the end of the buffer
                    if (numVertices > (stream.Remains() / sizeof(VertexStatic))) {
                        mParseError = true;
                        return;
                    }

                    mCurrentMesh->vertices.resize(numVertices);

                    const VertexStatic* srcVerts = rcast<const VertexStatic*>(stream.GetDataAtCursor());
                    MetroVertex* dstVerts = mCurrentMesh->vertices.data();

                    for (size_t i = 0; i < numVertices; ++i) {
                        *dstVerts = ConvertVertex(*srcVerts);
                        dstVerts->pos *= mCurrentMesh->vscale;
                        ++srcVerts;
                        ++dstVerts;
                    }
                }
            } break;

            case MC_SkinnedVerticesChunk: {
                if (mCurrentMesh) {
                    mCurrentMesh->skinned = true;

                    const size_t numBones = stream.ReadTyped<uint8_t>();

                    if (numBones > stream.Remains()) {
                        mParseError = true;
                        return;
                    }

                    mCurrentMesh->bonesRemap.resize(numBones);
                    stream.ReadToBuffer(mCurrentMesh->bonesRemap.data(), numBones);

                    //std::vector<MetroOBB> obbs(numBones);
                    //stream.ReadToBuffer(obbs.data(), obbs.size() * sizeof(MetroOBB));
                    stream.SkipBytes(numBones * sizeof(MetroOBB));

                    const size_t numVertices = stream.ReadTyped<uint32_t>();

                    if (!mReduxAssets) {
                        const size_t numShadowVertices = stream.ReadTyped<uint16_t>();
                    } else {
                        //#NOTE_SK: Redux has no per-mesh scale in the header (the field is always
                        //          zero there) - it quantises skinned positions against a fixed
                        //          12 unit box instead. Measured over all 2669 skinned meshes in
                        //          Metro 2033 Redux the ratio comes out at a median of 12.0002.
                        mCurrentMesh->vscale = 12.0f;
                    }

                    if (numVertices > (stream.Remains() / sizeof(VertexSkinned))) {
                        mParseError = true;
                        return;
                    }

                    mCurrentMesh->vertices.resize(numVertices);

                    const VertexSkinned* srcVerts = rcast<const VertexSkinned*>(stream.GetDataAtCursor());
                    MetroVertex* dstVerts = mCurrentMesh->vertices.data();

                    for (size_t i = 0; i < numVertices; ++i) {
                        *dstVerts = ConvertVertex(*srcVerts);
                        dstVerts->pos *= mCurrentMesh->vscale;
                        RemapBones(*dstVerts, mCurrentMesh->bonesRemap);

                        ++srcVerts;
                        ++dstVerts;
                    }
                }
            } break;

            case MC_FacesChunk: {
                if (mCurrentMesh) {
                    size_t numFaces = 0;

                    if (mReduxAssets && !mCurrentMesh->skinned) {
                        //#NOTE_SK: for its static meshes Redux writes the number of *indices*
                        //          rather than triangles, and keeps no shadow geometry counter
                        //          after it. Its skinned meshes use the Exodus layout as-is.
                        numFaces = stream.ReadTyped<uint32_t>() / 3;
                    } else {
                        numFaces = mCurrentMesh->skinned ? stream.ReadTyped<uint16_t>() : stream.ReadTyped<uint32_t>();
                        const size_t numShadowFaces = stream.ReadTyped<uint16_t>();
                    }

                    if (numFaces > (stream.Remains() / sizeof(MetroFace))) {
                        mParseError = true;
                        return;
                    }

                    mCurrentMesh->faces.resize(numFaces);
                    stream.ReadToBuffer(mCurrentMesh->faces.data(), numFaces * sizeof(MetroFace));
                }
            } break;

            case MC_SubMeshesChunk: {
                MemStream meshesStream = stream.Substream(chunkSize);
                size_t nextMeshId = 0;
                while (!meshesStream.Ended()) {
                    const size_t subMeshId = meshesStream.ReadTyped<uint32_t>();
                    const size_t subMeshSize = meshesStream.ReadTyped<uint32_t>();

                    if (subMeshId == nextMeshId) {
                        mCurrentMesh = new MetroMesh();
                        mMeshes.push_back(mCurrentMesh);

                        MemStream subStream = meshesStream.Substream(subMeshSize);
                        this->ReadSubChunks(subStream);
                        ++nextMeshId;

                        meshesStream.SkipBytes(subMeshSize);
                    } else {
                        break;
                    }
                }
                mCurrentMesh = nullptr;
            } break;

            case MC_MeshesInline: {
                stream.SkipBytes(16); // wtf ???
                MemStream subStream = stream.Substream(chunkSize - 16);
                this->ReadSubChunks(subStream);
            } break;

            case MC_MeshesLinks: {
                const size_t numStrings = stream.ReadTyped<uint32_t>();

                // one string cannot be shorter than its terminator, so this many of them
                // simply cannot fit - the count is garbage and the file is not ours
                if (numStrings > stream.Remains()) {
                    mParseError = true;
                    return;
                }

                StringArray links;
                for (size_t i = 0; i < numStrings; ++i) {
                    CharString linksString = stream.ReadStringZ();
                    if (!linksString.empty()) {
                        StringArray splittedLinks = SplitString(linksString, ',');
                        links.insert(links.end(), splittedLinks.begin(), splittedLinks.end());
                    }
                }

                if (mVFXReader && !links.empty()) {
                    this->LoadLinkedMeshes(links);
                }
            } break;

            case MC_SkeletonLink: {
                CharString skeletonRef = stream.ReadStringZ();
                mSkeletonPath = "content\\meshes\\" + skeletonRef + ".skeleton.bin";
                const size_t fileIdx = mVFXReader->FindFile(mSkeletonPath);
                if (MetroFile::InvalidFileIdx != fileIdx) {
                    MemStream stream = mVFXReader->ExtractFile(fileIdx);
                    if (stream) {
                        mSkeleton = new MetroSkeleton();
                        if (!mSkeleton->LoadFromData(stream)) {
                            MySafeDelete(mSkeleton);
                        }
                    }
                }
            } break;

            case MC_SkeletonInline: {
                mSkeleton = new MetroSkeleton();
                if (!mSkeleton->LoadFromData(stream.Substream(chunkSize))) {
                    MySafeDelete(mSkeleton);
                }
            } break;
        }

        stream.SetCursor(chunkEnd);
    }
}

void MetroModel::LoadLinkedMeshes(const StringArray& links) {
    mCurrentMesh = nullptr;

    for (const CharString& lnk : links) {
        //#NOTE_SK: reading lnk[1] without checking the length walks past the end of an
        //          empty link, and garbage chunk data produces empty links very easily
        if (lnk.empty()) {
            continue;
        }

        size_t fileIdx = MetroFile::InvalidFileIdx;

        if (lnk.size() > 1 && lnk[0] == '.' && lnk[1] == '\\') { // relative path
            const MetroFile* folder = mVFXReader->GetParentFolder(mThisFileIdx);
            fileIdx = mVFXReader->FindFile(lnk.substr(2) + ".mesh", folder);
        } else {
            CharString meshFilePath = "content\\meshes\\" + lnk + ".mesh";
            fileIdx = mVFXReader->FindFile(meshFilePath);
        }
        if (MetroFile::InvalidFileIdx != fileIdx) {
            MemStream stream = mVFXReader->ExtractFile(fileIdx);
            if (stream) {
                this->ReadSubChunks(stream);
            }

            mCurrentMesh = nullptr;
        }
    }
}

void MetroModel::LoadMotions() {
    CharString motionsStr;
    if (mSkeleton) {
        motionsStr = mSkeleton->GetMotionsStr();
    }

    if (motionsStr.empty()) {
        return;
    }

    MyArray<size_t> motionFiles;

    StringArray motionFolders = SplitString(motionsStr, ',');
    StringArray motionPaths;
    for (const CharString& f : motionFolders) {
        CharString fullFolderPath = "content\\motions\\" + f + "\\";

        const auto& v = mVFXReader->FindFilesInFolder(fullFolderPath, ".m2");

        for (const size_t idx : v) {
            motionPaths.push_back(fullFolderPath + mVFXReader->GetFile(idx).name);
        }

        motionFiles.insert(motionFiles.end(), v.begin(), v.end());
    }


    const size_t numBones = mSkeleton->GetNumBones();

    //#NOTE_SK: Metro shares one enormous animation library per skeleton - a human character
    //          points at 3638 .m2 files, and loading them all costs several gigabytes and
    //          many minutes, which makes the tool unusable on exactly the models people care
    //          about. Props and weapons have a handful and are unaffected by this ceiling.
    const size_t kMaxMotionsPerModel = 256;

    if (motionFiles.size() > kMaxMotionsPerModel) {
        LogPrintF(LogLevel::Warning,
                  "this skeleton has %zu motions available, loading the first %zu - the rest are skipped to keep memory and export time sane",
                  motionFiles.size(), kMaxMotionsPerModel);
        motionFiles.resize(kMaxMotionsPerModel);
    }

    mMotions.reserve(motionFiles.size());

    //#NOTE_SK: the root bones are where a level animation hides its world placement, so they
    //          are the ones worth re-anchoring. A skeleton can have more than one root.
    MyArray<size_t> rootBones;
    for (size_t b = 0; b < numBones; ++b) {
        if (mSkeleton->GetBoneParentIdx(b) == MetroBone::InvalidIdx) {
            rootBones.push_back(b);
        }
    }

    size_t i = 0, reAnchored = 0, wrongSkeleton = 0;
    float furthest = 0.0f;
    for (const size_t idx : motionFiles) {
        MemStream stream = mVFXReader->ExtractFile(idx);
        if (stream) {
            const MetroFile& mf = mVFXReader->GetFile(idx);
            CharString motionName = fs::path(mf.name).stem().u8string();

            MetroMotion* motion = new MetroMotion(motionName);
            motion->SetPath(motionPaths[i]);

            //#NOTE_SK: a motions folder is shared, and can hold motions built for a different
            //          skeleton that happens to have the same number of bones. Matching the
            //          bone count alone would let those through to be applied bone by bone to
            //          a skeleton they were never meant for.
            const bool loaded = motion->LoadFromData(stream);
            const bool rightShape = loaded && motion->GetNumBones() == numBones;
            const bool rightSkeleton = rightShape && scast<uint32_t>(motion->GetBonesCRC()) == mSkeleton->GetBonesCRC();

            if (rightSkeleton) {
                float shifted = 0.0f;
                for (const size_t rb : rootBones) {
                    shifted = std::max(shifted, motion->AnchorBonePosition(rb, mSkeleton->GetBonePosition(rb)));
                }

                //#NOTE_SK: a handful of motions hang the placement on a bone that is not a
                //          root. A bone's translation is measured from its parent, so a limb
                //          cannot honestly sit hundreds of metres away - anything past this
                //          is a level coordinate that wandered in, never a pose.
                const float kImpossibleBoneOffset = 10.0f;
                for (size_t b = 0; b < numBones; ++b) {
                    shifted = std::max(shifted, motion->AnchorBonePosition(b, mSkeleton->GetBonePosition(b),
                                                                          kImpossibleBoneOffset));
                }

                if (shifted > 1.0f) {
                    ++reAnchored;
                    furthest = std::max(furthest, shifted);
                }

                mMotions.push_back(motion);
            } else {
                if (rightShape) {
                    ++wrongSkeleton;
                }
                MySafeDelete(motion);
            }
        }
        ++i;
    }

    LogPrintF(LogLevel::Info, "LoadMotions: kept %zu motions", mMotions.size());
    if (wrongSkeleton) {
        LogPrintF(LogLevel::Warning, "LoadMotions: %zu motions belong to another skeleton and were skipped", wrongSkeleton);
    }
    if (reAnchored) {
        LogPrintF(LogLevel::Info,
                  "LoadMotions: %zu motions carried a level placement in their root bone and were re-anchored (furthest was %.1f m)",
                  reAnchored, furthest);
    }
}
