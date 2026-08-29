#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

#include <chrono>

#include "metro/MetroModel.h"
#include "metro/MetroSkeleton.h"
#include "metro/MetroMotion.h"
#include "metro/MetroTexture.h"
#include "metro/VFXReader.h"
#include "metro/MetroTexturesDatabase.h"

#include "RenderPanel.h"

// model viewer shaders
#include "shaders/ModelViewerVS.hlsl.h"
#include "shaders/ModelViewerPS.hlsl.h"

// cubemap viewer shaders
#include "shaders/CubemapViewerVS.hlsl.h"
#include "shaders/CubemapViewerPS.hlsl.h"

// String to std::string wrapper
#include <msclr/marshal_cppstd.h>

namespace MetroEX {

    // Device creation combinations, from the most desirable down. The fallbacks all matter on real
    // machines: the D3D11 debug layer ships in the optional "Graphics Tools" Windows feature and its
    // absence fails device creation outright (DXGI_ERROR_SDK_COMPONENT_MISSING), an 11.0-only runtime
    // rejects the whole array if it starts with 11_1, and a box with no usable 3D driver (VM, RDP
    // session, GPU in a bad state) can still render through the WARP software rasterizer.
    struct DeviceAttempt {
        D3D_DRIVER_TYPE driverType;
        UINT            skipLevels;     // how many of the leading feature levels to drop
        bool            debugLayer;
    };

    static const DeviceAttempt kDeviceAttempts[] = {
        { D3D_DRIVER_TYPE_HARDWARE, 0, true  },
        { D3D_DRIVER_TYPE_HARDWARE, 0, false },
        { D3D_DRIVER_TYPE_HARDWARE, 1, false },
        { D3D_DRIVER_TYPE_WARP,     1, false }
    };

    RenderPanel::RenderPanel()
        : Panel()
        , mSwapChain(nullptr)
        , mDevice(nullptr)
        , mDeviceContext(nullptr)
        , mRenderTargetView(nullptr)
        , mDepthStencilBuffer(nullptr)
        , mDepthStencilState(nullptr)
        , mDepthStencilView(nullptr)
        , mRasterState(nullptr)
        // model viewer
        , mModelViewerVS(nullptr)
        , mModelViewerPS(nullptr)
        , mModelInputLayout(nullptr)
        , mModelConstantBuffer(nullptr)
        , mBackBufferWidth(0)
        , mBackBufferHeight(0)
        , mWhiteTex(nullptr)
        , mWhiteSRV(nullptr)
        , mModelGeometries(nullptr)
        , mModelTextures(nullptr)
        // model viewer stuff
        , mModel(nullptr)
        , mVFXReader(nullptr)
        , mDatabase(nullptr)
        // animation
        , mAnimation(nullptr)
        , mAnimTimer(nullptr)
        // cubemap viewer stuff
        , mCamera(nullptr)
        , mCubemap(nullptr)
        , mCubemapTexture(nullptr)
        , mCubemapViewerVS(nullptr)
        , mCubemapViewerPS(nullptr)
        //
        , mViewingParams(nullptr)
        , mConstantBufferData(nullptr)
    {
        this->components = gcnew System::ComponentModel::Container();

        mModelTextures = gcnew System::Collections::Generic::Dictionary<String^, IntPtr>(0);
        mCubemapTexture = new RenderTexture;
        mCubemapTexture->tex = nullptr;
        mCubemapTexture->srv = nullptr;

        mAnimation = new Animation;

        mAnimTimer = gcnew Timer(this->components);
        mAnimTimer->Interval = 16;
        mAnimTimer->Tick += gcnew System::EventHandler(this, &RenderPanel::AnimationTimer_Tick);
        mAnimTimer->Stop();
    }

    bool RenderPanel::InitGraphics() {
        HRESULT result;
        ID3D11Texture2D* backBuffer = nullptr;

        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        D3D11_TEXTURE2D_DESC depthBufferDesc = {};
        D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
        D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc = {};
        D3D11_RASTERIZER_DESC rasterDesc = {};
        D3D11_VIEWPORT viewport = {};

        swapChainDesc.BufferCount = 1;
        swapChainDesc.BufferDesc.Width = this->Size.Width;
        swapChainDesc.BufferDesc.Height = this->Size.Height;
        swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferDesc.RefreshRate.Numerator = 0;
        swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
        swapChainDesc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
        swapChainDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.OutputWindow = (HWND)this->Handle.ToPointer();
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.Windowed = true;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        //#NOTE_SK: a zero-sized back buffer makes D3D11CreateDeviceAndSwapChain bail out with
        //          E_INVALIDARG, and the panel can legitimately be 0x0 while the form is still
        //          being laid out
        if (!swapChainDesc.BufferDesc.Width) {
            swapChainDesc.BufferDesc.Width = 1;
        }
        if (!swapChainDesc.BufferDesc.Height) {
            swapChainDesc.BufferDesc.Height = 1;
        }

        //#NOTE_SK: our shaders are SM 5.0, so 11_0 is the floor here - 10_x would only fail later
        //          on CreateVertexShader
        const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        const UINT numFeatureLevels = scast<UINT>(sizeof(featureLevels) / sizeof(featureLevels[0]));
        D3D_FEATURE_LEVEL acquiredFeatureLevel = D3D_FEATURE_LEVEL_11_0;

        pin_ptr<IDXGISwapChain*> swapChainPtr(&mSwapChain);
        pin_ptr<ID3D11Device*> devicePtr(&mDevice);
        pin_ptr<ID3D11DeviceContext*> contextPtr(&mDeviceContext);

        UINT deviceFlags = 0;
#ifdef _DEBUG
        deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        result = E_FAIL;
        for (const DeviceAttempt& attempt : kDeviceAttempts) {
            //#NOTE_SK: in a release build the debug-layer retry is identical to the plain one, skip it
            if (attempt.debugLayer && !(deviceFlags & D3D11_CREATE_DEVICE_DEBUG)) {
                continue;
            }

            const UINT flags = attempt.debugLayer ? deviceFlags : (deviceFlags & ~scast<UINT>(D3D11_CREATE_DEVICE_DEBUG));

            result = D3D11CreateDeviceAndSwapChain(nullptr, attempt.driverType,
                                                   nullptr, flags,
                                                   featureLevels + attempt.skipLevels,
                                                   numFeatureLevels - attempt.skipLevels,
                                                   D3D11_SDK_VERSION, &swapChainDesc,
                                                   swapChainPtr, devicePtr, &acquiredFeatureLevel, contextPtr);
            if (SUCCEEDED(result)) {
                break;
            }
        }
        if (FAILED(result)) {
            mInitError = String::Format(L"Failed to create a Direct3D 11 device and swap chain (HRESULT 0x{0:X8}). No Direct3D 11-capable GPU driver and no software fallback available.", scast<int>(result));
            return false;
        }

        mCamera = new Camera();
        mCamera->SetViewport(ivec4(0, 0, this->Size.Width, this->Size.Height));
        mCamera->SetViewPlanes(0.0f, 1.0f);
        mCamera->LookAt(vec3(0.0f), vec3(0.0f, 0.0f, 1.0f));

        if (!this->CreateRenderTargets()) {
            mInitError = L"Failed to create the Direct3D 11 render targets.";
            return false;
        }

        rasterDesc.AntialiasedLineEnable = false;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        rasterDesc.DepthBias = 0;
        rasterDesc.DepthBiasClamp = 0.0f;
        rasterDesc.DepthClipEnable = true;
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.FrontCounterClockwise = false;
        rasterDesc.MultisampleEnable = false;
        rasterDesc.ScissorEnable = false;
        rasterDesc.SlopeScaledDepthBias = 0.0f;

        pin_ptr<ID3D11RasterizerState*> rsPtr(&mRasterState);
        result = mDevice->CreateRasterizerState(&rasterDesc, rsPtr);
        if (FAILED(result)) {
            mInitError = String::Format(L"Direct3D 11 setup failed at RenderPanel.cpp:{0}, HRESULT 0x{1:X8}", __LINE__, scast<int>(result));
            return false;
        }

        mDeviceContext->RSSetState(mRasterState);

        depthStencilDesc.DepthEnable = true;
        depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        depthStencilDesc.StencilEnable = false;

        pin_ptr<ID3D11DepthStencilState*> dssPtr(&mDepthStencilState);
        result = mDevice->CreateDepthStencilState(&depthStencilDesc, dssPtr);
        if (FAILED(result)) {
            mInitError = String::Format(L"Direct3D 11 setup failed at RenderPanel.cpp:{0}, HRESULT 0x{1:X8}", __LINE__, scast<int>(result));
            return false;
        }

        mDeviceContext->OMSetDepthStencilState(mDepthStencilState, 1);

        pin_ptr<ID3D11VertexShader*> vsPtr(&mModelViewerVS);
        result = mDevice->CreateVertexShader(sModelViewerVSData, sizeof(sModelViewerVSData), nullptr, vsPtr);
        if (FAILED(result)) {
            mInitError = String::Format(L"Direct3D 11 setup failed at RenderPanel.cpp:{0}, HRESULT 0x{1:X8}", __LINE__, scast<int>(result));
            return false;
        }

        pin_ptr<ID3D11PixelShader*> psPtr(&mModelViewerPS);
        result = mDevice->CreatePixelShader(sModelViewerPSData, sizeof(sModelViewerPSData), nullptr, psPtr);
        if (FAILED(result)) {
            mInitError = String::Format(L"Direct3D 11 setup failed at RenderPanel.cpp:{0}, HRESULT 0x{1:X8}", __LINE__, scast<int>(result));
            return false;
        }

        D3D11_INPUT_ELEMENT_DESC vsDesc[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(MetroVertex, pos),     D3D11_INPUT_PER_VERTEX_DATA, 0},
            { "TEXCOORD", 0, DXGI_FORMAT_R8G8B8A8_UINT,      0, offsetof(MetroVertex, bones),   D3D11_INPUT_PER_VERTEX_DATA, 0},
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(MetroVertex, normal),  D3D11_INPUT_PER_VERTEX_DATA, 0},
            { "TEXCOORD", 2, DXGI_FORMAT_R8G8B8A8_UNORM,     0, offsetof(MetroVertex, weights), D3D11_INPUT_PER_VERTEX_DATA, 0},
            { "TEXCOORD", 3, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(MetroVertex, uv0),     D3D11_INPUT_PER_VERTEX_DATA, 0}
        };
        pin_ptr<ID3D11InputLayout*> ilPtr(&mModelInputLayout);
        result = mDevice->CreateInputLayout(vsDesc, sizeof(vsDesc) / sizeof(vsDesc[0]), sModelViewerVSData, sizeof(sModelViewerVSData), ilPtr);
        if (FAILED(result)) {
            mInitError = String::Format(L"Direct3D 11 setup failed at RenderPanel.cpp:{0}, HRESULT 0x{1:X8}", __LINE__, scast<int>(result));
            return false;
        }

        mViewingParams = new ViewingParams;
        mViewingParams->rotation = vec2(0.0f);
        mViewingParams->offset = vec2(0.0f);
        mViewingParams->nearFar = vec2(0.001f, 50.0f);
        mViewingParams->zoom = 1.0f;

        mConstantBufferData = new ConstantBufferData;
        mConstantBufferData->modelBSphere = vec4(1.0f);
        mConstantBufferData->matModel = MatIdentity;
        mConstantBufferData->matView = MatIdentity;
        mConstantBufferData->matProjection = MatIdentity;

        pin_ptr<ID3D11Buffer*> cbPtr(&mModelConstantBuffer);
        D3D11_BUFFER_DESC desc = {};
        D3D11_SUBRESOURCE_DATA subData = {};

        desc.ByteWidth = sizeof(ConstantBufferData);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        subData.pSysMem = mConstantBufferData;
        result = mDevice->CreateBuffer(&desc, &subData, cbPtr);
        if (FAILED(result)) {
            mInitError = String::Format(L"Direct3D 11 setup failed at RenderPanel.cpp:{0}, HRESULT 0x{1:X8}", __LINE__, scast<int>(result));
            return false;
        }

        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.MaxAnisotropy = 4;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        samplerDesc.MinLOD = -(std::numeric_limits<float>::max)();
        samplerDesc.MaxLOD = (std::numeric_limits<float>::max)();

        pin_ptr<ID3D11SamplerState*> samplerStatePtr(&mModelTextureSampler);
        result = mDevice->CreateSamplerState(&samplerDesc, samplerStatePtr);
        if (FAILED(result)) {
            mInitError = String::Format(L"Direct3D 11 setup failed at RenderPanel.cpp:{0}, HRESULT 0x{1:X8}", __LINE__, scast<int>(result));
            return false;
        }

        pin_ptr<ID3D11VertexShader*> cubeVsPtr(&mCubemapViewerVS);
        result = mDevice->CreateVertexShader(sCubemapViewerVSData, sizeof(sCubemapViewerVSData), nullptr, cubeVsPtr);
        if (FAILED(result)) {
            mInitError = String::Format(L"Direct3D 11 setup failed at RenderPanel.cpp:{0}, HRESULT 0x{1:X8}", __LINE__, scast<int>(result));
            return false;
        }

        //#NOTE_SK: the model pixel shader clips anything whose albedo alpha is below 0.1, and
        //          sampling an unbound texture yields all zeroes - so a mesh we could not find
        //          a texture for used to vanish entirely rather than show up untextured. That
        //          is the normal case for the Redux games, whose textures are Crunch encoded
        //          and which we cannot decode yet. Bind this instead and they shade in white.
        {
            const uint32_t whitePixel = 0xFFFFFFFF;

            D3D11_TEXTURE2D_DESC whiteDesc = {};
            whiteDesc.Width = 1;
            whiteDesc.Height = 1;
            whiteDesc.MipLevels = 1;
            whiteDesc.ArraySize = 1;
            whiteDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            whiteDesc.SampleDesc.Count = 1;
            whiteDesc.Usage = D3D11_USAGE_IMMUTABLE;
            whiteDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA whiteData = {};
            whiteData.pSysMem = &whitePixel;
            whiteData.SysMemPitch = sizeof(whitePixel);

            pin_ptr<ID3D11Texture2D*> whiteTexPtr(&mWhiteTex);
            if (SUCCEEDED(mDevice->CreateTexture2D(&whiteDesc, &whiteData, whiteTexPtr))) {
                pin_ptr<ID3D11ShaderResourceView*> whiteSrvPtr(&mWhiteSRV);
                mDevice->CreateShaderResourceView(mWhiteTex, nullptr, whiteSrvPtr);
            }
        }

        pin_ptr<ID3D11PixelShader*> cubePsPtr(&mCubemapViewerPS);
        result = mDevice->CreatePixelShader(sCubemapViewerPSData, sizeof(sCubemapViewerPSData), nullptr, cubePsPtr);
        if (FAILED(result)) {
            mInitError = String::Format(L"Direct3D 11 setup failed at RenderPanel.cpp:{0}, HRESULT 0x{1:X8}", __LINE__, scast<int>(result));
            return false;
        }

        return true;
    }

    String^ RenderPanel::GetInitError() {
        return mInitError;
    }

    void RenderPanel::SetModel(MetroModel* model, VFXReader* vfxReader, MetroTexturesDatabase* database) {
        mCubemap = nullptr;

        if (mModel != model) {
            MySafeDelete(mModel);

            mModel = model;
            mVFXReader = vfxReader;
            mDatabase = database;

            mCurrentMotion = nullptr;

            this->ResetAnimation();
            this->CreateModelGeometries();
            this->CreateTextures();
            this->UpdateProjectionAndReset();

            LogPrintF(LogLevel::Info, "viewer: %zu meshes, bsphere = (%.4f, %.4f, %.4f) r = %.4f, panel %dx%d",
                      mModel ? mModel->GetNumMeshes() : 0,
                      mConstantBufferData->modelBSphere.x, mConstantBufferData->modelBSphere.y,
                      mConstantBufferData->modelBSphere.z, mConstantBufferData->modelBSphere.w,
                      this->Width, this->Height);

            this->Render();
        }
    }

    MetroModel* RenderPanel::GetModel() {
        return mModel;
    }

    void RenderPanel::SetCubemap(MetroTexture* cubemap) {
        MySafeDelete(mModel);

        mCubemap = cubemap;

        this->CreateModelGeometries();
        this->CreateTextures();
        this->Render();
    }

    void RenderPanel::SwitchMotion(const size_t idx) {
        if (mModel && mModel->IsAnimated()) {
            mCurrentMotion = mModel->GetMotion(idx);
            mAnimation->time = 0.0f;
        }
    }

    bool RenderPanel::IsPlayingAnim() {
        return mAnimTimer->Enabled;
    }

    void RenderPanel::PlayAnim(const bool play) {
        if (play && mModel && mModel->IsAnimated() && mCurrentMotion) {
            this->ResetAnimation();
            mAnimTimer->Start();
        } else {
            mAnimTimer->Stop();
        }
    }


    bool RenderPanel::CreateRenderTargets() {
        if (!mDevice || !mDeviceContext || !mSwapChain) {
            return false;
        }

        // must match the size the swap chain was actually (re)sized to, and never be zero
        const UINT rtWidth = (this->Size.Width > 0) ? scast<UINT>(this->Size.Width) : 1u;
        const UINT rtHeight = (this->Size.Height > 0) ? scast<UINT>(this->Size.Height) : 1u;

        MySafeRelease(mRenderTargetView);
        MySafeRelease(mDepthStencilBuffer);
        MySafeRelease(mDepthStencilView);

        ID3D11Texture2D* backBuffer = nullptr;
        pin_ptr<ID3D11Texture2D*> backBufferPtr(&backBuffer);
        HRESULT hr = mSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)backBufferPtr);
        if (FAILED(hr)) {
            return false;
        }
        pin_ptr<ID3D11RenderTargetView*> rtvPtr(&mRenderTargetView);
        hr = mDevice->CreateRenderTargetView(backBuffer, nullptr, rtvPtr);
        backBuffer->Release();

        D3D11_TEXTURE2D_DESC depthBufferDesc = {};
        D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
        D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc = {};

        depthBufferDesc.Width = rtWidth;
        depthBufferDesc.Height = rtHeight;
        depthBufferDesc.MipLevels = 1;
        depthBufferDesc.ArraySize = 1;
        depthBufferDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthBufferDesc.SampleDesc.Count = 1;
        depthBufferDesc.SampleDesc.Quality = 0;
        depthBufferDesc.Usage = D3D11_USAGE_DEFAULT;
        depthBufferDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;


        pin_ptr<ID3D11Texture2D*> dsbPtr(&mDepthStencilBuffer);
        hr = mDevice->CreateTexture2D(&depthBufferDesc, nullptr, dsbPtr);
        if (FAILED(hr)) {
            return false;
        }

        depthStencilViewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthStencilViewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

        pin_ptr<ID3D11DepthStencilView*> dsvPtr(&mDepthStencilView);
        hr = mDevice->CreateDepthStencilView(mDepthStencilBuffer, &depthStencilViewDesc, dsvPtr);
        if (FAILED(hr)) {
            return false;
        }

        mDeviceContext->OMSetRenderTargets(1, rtvPtr, mDepthStencilView);

        D3D11_VIEWPORT viewport = {};
        viewport.Width = scast<float>(rtWidth);
        viewport.Height = scast<float>(rtHeight);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;

        mDeviceContext->RSSetViewports(1, &viewport);

        mCamera->SetViewport(ivec4(0, 0, scast<int>(rtWidth), scast<int>(rtHeight)));

        mBackBufferWidth = scast<int>(rtWidth);
        mBackBufferHeight = scast<int>(rtHeight);

        return true;
    }

    void RenderPanel::UpdateModelMatrix() {
        mConstantBufferData->matModel = MatRotate(Deg2Rad(mViewingParams->rotation.x), 0.0f, 1.0f, 0.0f) * MatRotate(-Deg2Rad(mViewingParams->rotation.y), 0.0f, 0.0f, 1.0f);
        mConstantBufferData->matModel[3] = vec4(mViewingParams->offset.x, mViewingParams->offset.y, 0.0f, 1.0f);
    }

    void RenderPanel::UpdateViewMatrix() {
        if (mModel) {
            const float r = mConstantBufferData->modelBSphere.w * mViewingParams->zoom;
            mConstantBufferData->matView = MatLookAt(vec3(-r, r, r), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        }
    }

    void RenderPanel::UpdateProjectionAndReset() {
        mViewingParams->zoom = 1.0f;

        const float r = mConstantBufferData->modelBSphere.w;
        this->UpdateViewMatrix();

        //#NOTE_SK: a near plane at r/1000 gave a near:far ratio of 1:100000, which collapsed
        //          the whole model into the last thousandth of a 24 bit depth buffer and made
        //          it z-fight with itself. The camera sits at r*sqrt(3), so r/20 is still far
        //          closer than anything can get while leaving usable precision.
        mViewingParams->nearFar = vec2(r * 0.05f, r * 100.0f);

        const int w = this->Width;
        const int h = this->Height;
        mConstantBufferData->matProjection = MatPerspective(Deg2Rad(60.0f), scast<float>(w) / scast<float>(h), mViewingParams->nearFar.x, mViewingParams->nearFar.y);

        mConstantBufferData->matModel = MatIdentity;
        mViewingParams->rotation = vec2(0.0f);
        mViewingParams->offset = vec2(0.0f);
    }

    void RenderPanel::CreateModelGeometries() {
        if (!mDevice) {
            return;
        }

        if (mModelGeometries) {
            for each (RenderGeometry* rg in mModelGeometries) {
                if (rg) {
                    MySafeRelease(rg->vb);
                    MySafeRelease(rg->ib);
                    delete rg;
                }
            }

            MySafeDelete(mModelGeometries);
        }

        if (!mModel) {
            return;
        }

        const size_t numMeshes = mModel->GetNumMeshes();
        mModelGeometries = gcnew array<RenderGeometry*>(scast<int>(numMeshes));

        mConstantBufferData->modelBSphere = vec4(0.0f, 0.0f, 0.0f, 1.0f);

        AABBox modelBBox;
        modelBBox.Reset();
        for (size_t i = 0; i < numMeshes; ++i) {
            const MetroMesh* mesh = mModel->GetMesh(i);
            if (!mesh->vertices.empty() && !mesh->faces.empty()) {
                RenderGeometry* rg = new RenderGeometry;

                AABBox bbox;
                bbox.Reset();

                rg->texture = nullptr;
                rg->numFaces = mesh->faces.size();

                D3D11_BUFFER_DESC desc = {};
                D3D11_SUBRESOURCE_DATA subData = {};

                //vb
                desc.ByteWidth = scast<UINT>(mesh->vertices.size() * sizeof(MetroVertex));
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                subData.pSysMem = mesh->vertices.data();
                HRESULT hrVB = mDevice->CreateBuffer(&desc, &subData, &rg->vb);

                //ib
                desc.ByteWidth = scast<UINT>(mesh->faces.size() * sizeof(MetroFace));
                desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
                subData.pSysMem = mesh->faces.data();
                HRESULT hrIB = mDevice->CreateBuffer(&desc, &subData, &rg->ib);

                if (FAILED(hrVB) || FAILED(hrIB)) {
                    LogPrintF(LogLevel::Error, "mesh %zu: failed to create buffers, vb=0x%08x ib=0x%08x", i, hrVB, hrIB);
                }

                for (const MetroVertex& v : mesh->vertices) {
                    bbox.Absorb(v.pos);
                }

                modelBBox.Absorb(bbox.minimum);
                modelBBox.Absorb(bbox.maximum);

                mConstantBufferData->modelBSphere = vec4(modelBBox.Center(), Length(modelBBox.Extent()));

                mModelGeometries[scast<int>(i)] = rg;
            } else {
                mModelGeometries[scast<int>(i)] = nullptr;
            }
        }
    }

    void RenderPanel::CreateTextures() {
        for each (IntPtr ptr in mModelTextures->Values) {
            RenderTexture* rt = rcast<RenderTexture*>(ptr.ToPointer());
            MySafeRelease(rt->srv);
            MySafeRelease(rt->tex);
        }
        mModelTextures->Clear();

        MySafeRelease(mCubemapTexture->srv);
        MySafeRelease(mCubemapTexture->tex);

        if (mCubemap) {
            this->CreateRenderTexture(mCubemap, mCubemapTexture);
            return;
        } else if (!mModel || !mVFXReader) {
            return;
        }

        //#NOTE_SK: the textures database only exists in Exodus archives, the Redux games
        //          have no textures_handles_storage.bin at all - so mDatabase is null there
        //          and dereferencing it blindly used to take the whole app down the moment
        //          you clicked a model.
        //
        //          Extensions worth trying, best first. The Redux games keep most of their
        //          textures as Crunch-compressed .512c/.1024c/.2048c which we cannot decode
        //          yet, and those models simply show up untextured.
        const char* kTextureExts[] = { ".2048", ".2048c", ".1024", ".1024c", ".512", ".512c", ".dds" };

        const size_t numMeshes = mModel->GetNumMeshes();
        for (size_t i = 0; i < numMeshes; ++i) {
            const MetroMesh* mesh = mModel->GetMesh(i);
            RenderGeometry* rg = mModelGeometries[scast<int>(i)];
            if (!rg || mesh->materials.empty()) {
                continue;
            }

            const CharString& textureName = mesh->materials.front();
            if (textureName.empty()) {
                continue;
            }

            const CharString& sourceName = mDatabase ? mDatabase->GetSourceName(textureName) : kEmptyString;

            String^ texNameManaged = msclr::interop::marshal_as<String^>(textureName);

            if (mModelTextures->ContainsKey(texNameManaged)) {
                rg->texture = rcast<RenderTexture*>(mModelTextures[texNameManaged].ToPointer());
                continue;
            }

            const CharString texturePath = CharString("content\\textures\\") + (sourceName.empty() ? textureName : sourceName);

            for (const char* ext : kTextureExts) {
                const size_t textureIdx = mVFXReader->FindFile(texturePath + ext);
                if (textureIdx == MetroFile::InvalidFileIdx) {
                    continue;
                }

                MemStream stream = mVFXReader->ExtractFile(textureIdx);
                if (!stream) {
                    continue;
                }

                const MetroFile& mf = mVFXReader->GetFile(textureIdx);


                MetroTexture texture;
                if (texture.LoadFromData(stream, mf.name)) {
                    LogPrintF(LogLevel::Info, "viewer texture: %s -> %zux%zu, %zu mips", mf.name.c_str(), texture.GetWidth(), texture.GetHeight(), texture.GetNumMips());
                    RenderTexture* rt = new RenderTexture;
                    rt->tex = nullptr;
                    rt->srv = nullptr;
                    this->CreateRenderTexture(&texture, rt);


                    if (rt->srv) {
                        mModelTextures->Add(texNameManaged, IntPtr(rt));
                        rg->texture = rt;
                    } else {
                        delete rt;
                    }
                    break;
                }
            }
        }
    }

    void RenderPanel::CreateRenderTexture(const MetroTexture* srcTexture, RenderTexture* rt) {
        D3D11_TEXTURE2D_DESC desc = {};

        //#NOTE_SK: Redux textures come in as BC1/BC2/BC3, Exodus ones as BC7, and hdr
        //          cubemaps as BC6H - the block size differs between BC1 and the rest,
        //          which the subresource pitch below depends on
        DXGI_FORMAT textureFormat = DXGI_FORMAT_BC7_TYPELESS;
        DXGI_FORMAT srvFormat = DXGI_FORMAT_BC7_UNORM;

        if (srcTexture->IsCubemap()) {
            textureFormat = DXGI_FORMAT_BC6H_TYPELESS;
            srvFormat = DXGI_FORMAT_BC6H_UF16;
        } else {
            switch (srcTexture->GetFormat()) {
                case MetroTexture::TextureFormat::BC1: {
                    textureFormat = DXGI_FORMAT_BC1_TYPELESS;
                    srvFormat = DXGI_FORMAT_BC1_UNORM;
                } break;

                case MetroTexture::TextureFormat::BC2: {
                    textureFormat = DXGI_FORMAT_BC2_TYPELESS;
                    srvFormat = DXGI_FORMAT_BC2_UNORM;
                } break;

                case MetroTexture::TextureFormat::BC3: {
                    textureFormat = DXGI_FORMAT_BC3_TYPELESS;
                    srvFormat = DXGI_FORMAT_BC3_UNORM;
                } break;

                default:
                    break;
            }
        }

        const UINT bytesPerBlock = scast<UINT>(srcTexture->GetBytesPerBlock());

        desc.Width = scast<UINT>(srcTexture->GetWidth());
        desc.Height = scast<UINT>(srcTexture->GetHeight());
        desc.MipLevels = scast<UINT>(srcTexture->GetNumMips());
        desc.ArraySize = srcTexture->IsCubemap() ? 6 : 1;
        desc.Format = textureFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (srcTexture->IsCubemap()) {
            desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        }

        MyArray<D3D11_SUBRESOURCE_DATA> subDesc(desc.ArraySize * desc.MipLevels);

        size_t counter = 0;
        const uint8_t* dataPtr = srcTexture->GetRawData();
        for (size_t i = 0; i < desc.ArraySize; ++i) {
            UINT mipWidth = desc.Width;
            UINT mipHeight = desc.Height;
            for (size_t j = 0; j < desc.MipLevels; ++j, ++counter) {
                const UINT numBlocksW = std::max<UINT>(1, (mipWidth + 3) / 4);
                const UINT numBlocksH = std::max<UINT>(1, (mipHeight + 3) / 4);

                subDesc[counter].pSysMem = dataPtr;
                subDesc[counter].SysMemPitch = numBlocksW * bytesPerBlock;
                subDesc[counter].SysMemSlicePitch = subDesc[counter].SysMemPitch * numBlocksH;

                dataPtr += subDesc[counter].SysMemSlicePitch;

                mipWidth >>= 1;
                mipHeight >>= 1;
            }
        }
        pin_ptr<ID3D11Texture2D*> texPtr(&rt->tex);
        HRESULT hr = mDevice->CreateTexture2D(&desc, subDesc.data(), texPtr);
        if (SUCCEEDED(hr)) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = srvFormat;
            srvDesc.ViewDimension = srcTexture->IsCubemap() ? D3D11_SRV_DIMENSION_TEXTURECUBE : D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = ~0u;
            srvDesc.Texture2D.MostDetailedMip = 0;

            pin_ptr<ID3D11ShaderResourceView*> srvPtr(&rt->srv);
            hr = mDevice->CreateShaderResourceView(rt->tex, &srvDesc, srvPtr);
            if (FAILED(hr)) {
                MySafeRelease(rt->tex);
            }
        }
    }


    struct HierarchyBone {
        AnimBone                srcBone;
        MyArray<HierarchyBone*> children;
    };

    //#NOTE_SK: guarded against two things that a skeleton from an unfamiliar game can hand us:
    //          a cycle in the parent links (which used to recurse until the stack gave out) and
    //          more bones than the fixed 256 slot array can hold.
    static void FlattenHierarchyToArray(AnimBone*& arr, AnimBone* arrEnd, const HierarchyBone* hb, MyArray<bool>& visited) {
        // parents go first
        for (const HierarchyBone* next : hb->children) {
            if (arr >= arrEnd || next->srcBone.idx >= visited.size() || visited[next->srcBone.idx]) {
                continue;
            }
            visited[next->srcBone.idx] = true;
            *arr = next->srcBone;
            ++arr;
        }

        // children go next
        for (const HierarchyBone* next : hb->children) {
            if (arr >= arrEnd) {
                break;
            }
            FlattenHierarchyToArray(arr, arrEnd, next, visited);
        }
    }

    void RenderPanel::ResetAnimation() {
        mAnimTimer->Stop();

        for (auto& b : mConstantBufferData->bones) {
            b = MatIdentity;
        }

        mAnimation->time = 0.0f;
        if (mModel && mModel->IsAnimated()) {
            const MetroSkeleton* skeleton = mModel->GetSkeleton();

            //#NOTE_SK: Animation::bones and the shader constant buffer are both fixed at 256
            //          entries. A skeleton bigger than that used to run straight off the end
            //          of both, so clamp and say so rather than corrupt the heap.
            const size_t kMaxAnimBones = sizeof(mAnimation->bones) / sizeof(mAnimation->bones[0]);
            const size_t skeletonBones = skeleton->GetNumBones();
            const size_t numBones = std::min<size_t>(skeletonBones, kMaxAnimBones);

            if (skeletonBones > kMaxAnimBones) {
                LogPrintF(LogLevel::Warning, "skeleton has %zu bones, only the first %zu can be animated",
                          skeletonBones, kMaxAnimBones);
            }

            if (!numBones) {
                return;
            }

            MyArray<HierarchyBone> hierarchy(numBones);

            size_t rootBoneIdx = 0;
            for (size_t i = 0; i < numBones; ++i) {
                AnimBone& b = mAnimation->bones[i];
                b.idx = i;
                b.parentIdx = skeleton->GetBoneParentIdx(i);

                hierarchy[b.idx].srcBone = b;

                //#NOTE_SK: a bone parented to itself, or to something outside the range we
                //          kept, would either loop forever below or index out of bounds
                if (b.parentIdx != MetroBone::InvalidIdx && b.parentIdx < numBones && b.parentIdx != i) {
                    hierarchy[b.parentIdx].children.push_back(&hierarchy[b.idx]);
                } else {
                    rootBoneIdx = i;
                }

                mAnimation->bindPoseInv[i] = MatInverse(skeleton->GetBoneFullTransform(i));
            }

            // now we flatten our hierarchy so that parent bones are always come befor their children
            MyArray<bool> visited(numBones, false);
            visited[rootBoneIdx] = true;

            mAnimation->bones[0] = mAnimation->bones[rootBoneIdx];
            AnimBone* arr = &mAnimation->bones[1];
            AnimBone* arrEnd = &mAnimation->bones[0] + kMaxAnimBones;
            FlattenHierarchyToArray(arr, arrEnd, &hierarchy[rootBoneIdx], visited);
        }
    }

    void RenderPanel::UpdateAnimation(const float dt) {
        if (mModel && mModel->IsAnimated() && mCurrentMotion) {
            const MetroSkeleton* skeleton = mModel->GetSkeleton();

            const size_t numBones = std::min<size_t>(mCurrentMotion->GetNumBones(), skeleton->GetNumBones());
            const float animLen = mCurrentMotion->GetMotionTimeInSeconds();

            if (animLen <= 0.0f) {
                return;
            }

            if (mAnimation->time >= animLen) {
                mAnimation->time -= animLen;
            }

            const size_t key = scast<size_t>(std::floorf((mAnimation->time / animLen) * mCurrentMotion->GetNumKeys()));

            for (size_t i = 0; i < numBones; ++i) {
                const AnimBone& b = mAnimation->bones[i];
                if (b.idx >= numBones) {
                    continue;
                }

                mat4& m = mConstantBufferData->bones[b.idx];

                //#NOTE_SK: a motion only carries curves for the bones it actually moves. The
                //          rest used to get an identity local transform, which throws away
                //          their bind pose and tears the model apart - most of a machine or a
                //          character is not animated by any single clip. Those bones have to
                //          keep sitting where the skeleton put them.
                //
                //          The same applies one level down, per attribute: a bone can be
                //          listed as animated and still have an empty curve for its rotation
                //          or its offset. Nearly every bone has an empty position curve, and
                //          taking that as a zero offset collapses the bone onto its parent -
                //          which is what shredded the characters' faces, since the facial
                //          bones are the ones whose curves are most often left empty.
                m = skeleton->GetBoneTransform(b.idx);

                if (mCurrentMotion->IsBoneAnimated(b.idx)) {
                    if (mCurrentMotion->HasBoneRotation(b.idx)) {
                        const quat q = mCurrentMotion->GetBoneRotation(b.idx, key);
                        const vec4 keepT = m[3];
                        m = MatFromQuat(q);
                        m[3] = keepT;
                    }

                    if (mCurrentMotion->HasBonePosition(b.idx)) {
                        const vec3 t = mCurrentMotion->GetBonePosition(b.idx, key);
                        m[3] = vec4(t, 1.0f);
                    }
                }

                if (b.parentIdx != MetroBone::InvalidIdx && b.parentIdx < numBones) {
                    m = mConstantBufferData->bones[b.parentIdx] * m;
                }
            }

            for (size_t i = 0; i < numBones; ++i) {
                mat4& m = mConstantBufferData->bones[i];
                m = m * mAnimation->bindPoseInv[i];
            }

            mAnimation->time += dt;

            this->Render();
        }
    }

    void RenderPanel::Render() {
        static const float clearColor[4] = { 40.0f / 255.0f, 113.0f / 255.0f, 134.0f / 255.0f, 1.0f };

        if (mDeviceContext && mSwapChain) {
            //#NOTE_SK: the panel is laid out while it is still hidden, and a control that is
            //          resized while invisible never delivers OnResize to us. The swap chain
            //          would then stay at whatever size it had when the device was created -
            //          often 1x1 - and Present would stretch that single pixel across the
            //          whole viewer, which looks exactly like a viewer that draws nothing.
            //          So reconcile the two here, right before we draw.
            const int wantW = (this->Size.Width > 0) ? this->Size.Width : 1;
            const int wantH = (this->Size.Height > 0) ? this->Size.Height : 1;

            if (wantW != mBackBufferWidth || wantH != mBackBufferHeight) {
                MySafeRelease(mRenderTargetView);
                MySafeRelease(mDepthStencilView);
                MySafeRelease(mDepthStencilBuffer);

                if (SUCCEEDED(mSwapChain->ResizeBuffers(1, wantW, wantH, DXGI_FORMAT_UNKNOWN, 0))) {
                    this->CreateRenderTargets();
                }

                if (mConstantBufferData && mViewingParams) {
                    mConstantBufferData->matProjection = MatPerspective(Deg2Rad(60.0f),
                                                                        scast<float>(wantW) / scast<float>(wantH),
                                                                        mViewingParams->nearFar.x,
                                                                        mViewingParams->nearFar.y);
                }
            }

            if (!mRenderTargetView || !mDepthStencilView) {
                return;
            }

            mDeviceContext->ClearRenderTargetView(mRenderTargetView, clearColor);
            mDeviceContext->ClearDepthStencilView(mDepthStencilView, D3D11_CLEAR_DEPTH, 1.0f, 0);

            if (mModel) {
                mat4 modelView = mConstantBufferData->matView * mConstantBufferData->matModel;
                mConstantBufferData->matModelViewProj = mConstantBufferData->matProjection * modelView;
            } else if (mCubemap) {
                mat4 modelView = mCamera->GetTransform();
                mConstantBufferData->matView = modelView;
                mConstantBufferData->matModelViewProj = mCamera->GetProjection() * modelView;
                mConstantBufferData->camParams.x = Deg2Rad(mCamera->GetFovY());
                mConstantBufferData->camParams.y = scast<float>(this->Size.Width) / scast<float>(this->Size.Height);
            }

            D3D11_MAPPED_SUBRESOURCE subRes = {};
            mDeviceContext->Map(mModelConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &subRes);
            memcpy(subRes.pData, mConstantBufferData, sizeof(ConstantBufferData));
            mDeviceContext->Unmap(mModelConstantBuffer, 0);

            pin_ptr<ID3D11Buffer*> cbPtr(&mModelConstantBuffer);
            mDeviceContext->VSSetConstantBuffers(0, 1, cbPtr);
            mDeviceContext->PSSetConstantBuffers(0, 1, cbPtr);

            mDeviceContext->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            pin_ptr<ID3D11SamplerState*> samplerPtr(&mModelTextureSampler);
            mDeviceContext->PSSetSamplers(0, 1, samplerPtr);

            if (mModel) {
                mDeviceContext->IASetInputLayout(mModelInputLayout);
                mDeviceContext->VSSetShader(mModelViewerVS, nullptr, 0);
                mDeviceContext->PSSetShader(mModelViewerPS, nullptr, 0);

                if (mModelGeometries) {
                    for each (RenderGeometry* rg in mModelGeometries) {
                        if (rg) {
                            ID3D11ShaderResourceView* texSRV = (rg->texture && rg->texture->srv) ? rg->texture->srv : mWhiteSRV;
                            pin_ptr<ID3D11ShaderResourceView*> srvPtr(&texSRV);
                            mDeviceContext->PSSetShaderResources(0, 1, srvPtr);

                            const UINT stride = sizeof(MetroVertex);
                            const UINT offset = 0;
                            mDeviceContext->IASetVertexBuffers(0, 1, &rg->vb, &stride, &offset);
                            mDeviceContext->IASetIndexBuffer(rg->ib, DXGI_FORMAT_R16_UINT, 0);

                            mDeviceContext->DrawIndexed(scast<UINT>(rg->numFaces * 3), 0, 0);
                        }
                    }
                }
            } else if (mCubemap) {
                mDeviceContext->IASetInputLayout(nullptr);
                mDeviceContext->VSSetShader(mCubemapViewerVS, nullptr, 0);
                mDeviceContext->PSSetShader(mCubemapViewerPS, nullptr, 0);

                pin_ptr<ID3D11ShaderResourceView*> srvPtr(&mCubemapTexture->srv);
                mDeviceContext->PSSetShaderResources(0, 1, srvPtr);

                mDeviceContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
                mDeviceContext->IASetIndexBuffer(nullptr, scast<DXGI_FORMAT>(0), 0);

                mDeviceContext->Draw(3, 0);
            }

            mSwapChain->Present(0, 0);
        }
    }

    void RenderPanel::OnResize(System::EventArgs^ e) {
        Panel::OnResize(e);

        if (mSwapChain) {
            //#NOTE_SK: the panel collapses to 0x0 when the form is minimized or the splitter is
            //          dragged all the way over - ResizeBuffers rejects that, and the projection
            //          aspect would end up as a NaN
            const int w = (this->Size.Width > 0) ? this->Size.Width : 1;
            const int h = (this->Size.Height > 0) ? this->Size.Height : 1;

            // every view onto the back buffer has to go before ResizeBuffers will touch it
            MySafeRelease(mRenderTargetView);
            MySafeRelease(mDepthStencilView);
            MySafeRelease(mDepthStencilBuffer);

            HRESULT hr = mSwapChain->ResizeBuffers(1, w, h, DXGI_FORMAT_UNKNOWN, 0);
            if (SUCCEEDED(hr)) {
                this->CreateRenderTargets();
            }

            if (mConstantBufferData && mViewingParams) {
                mConstantBufferData->matProjection = MatPerspective(Deg2Rad(60.0f), scast<float>(w) / scast<float>(h), mViewingParams->nearFar.x, mViewingParams->nearFar.y);
            }
        }
    }

    void RenderPanel::OnPaint(System::Windows::Forms::PaintEventArgs^ e) {
        this->Render();
    }

    void RenderPanel::OnMouseDown(System::Windows::Forms::MouseEventArgs^ e) {
        vec2 mp(scast<float>(e->X), scast<float>(e->Y));

        if (e->Button == System::Windows::Forms::MouseButtons::Left) {
            mViewingParams->lastLMPos = mp;
        } else if (e->Button == System::Windows::Forms::MouseButtons::Right) {
            mViewingParams->lastRMPos = mp;
        }

        Panel::OnMouseDown(e);
    }

    void RenderPanel::OnMouseMove(System::Windows::Forms::MouseEventArgs^ e) {
        vec2 mp(scast<float>(e->X), scast<float>(e->Y));

        if (e->Button == System::Windows::Forms::MouseButtons::Left) {
            vec2 delta = mp - mViewingParams->lastLMPos;
            mViewingParams->lastLMPos = mp;

            mViewingParams->rotation += delta * 0.3f;

            if (mViewingParams->rotation.x < 0.0f) {
                mViewingParams->rotation.x += 360.0f;
            } else if (mViewingParams->rotation.x > 360.0f) {
                mViewingParams->rotation.x -= 360.0f;
            }

            if (mViewingParams->rotation.y < 0.0f) {
                mViewingParams->rotation.y += 360.0f;
            } else if (mViewingParams->rotation.y > 360.0f) {
                mViewingParams->rotation.y -= 360.0f;
            }

            mCamera->Rotate(delta.x * 0.1f, delta.y * 0.1f);

            this->UpdateModelMatrix();
            this->Render();

        }

        Panel::OnMouseMove(e);
    }

    void RenderPanel::OnMouseWheel(System::Windows::Forms::MouseEventArgs^ e) {
        if (e->Delta > 0) {
            mViewingParams->zoom = std::min<float>(mViewingParams->zoom + 0.1f, 5.0f);
        } else if (e->Delta < 0) {
            mViewingParams->zoom = std::max<float>(mViewingParams->zoom - 0.1f, 0.1f);
        }

        this->UpdateViewMatrix();
        this->Render();
    }

    void RenderPanel::AnimationTimer_Tick(System::Object^, System::EventArgs^) {
        static double sLastTimeMS = -1.0;

        std::chrono::system_clock::time_point tp = std::chrono::system_clock::now();
        std::chrono::system_clock::time_point::duration epoch = tp.time_since_epoch();

        const double currentTimeMS = scast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count());

        if (sLastTimeMS < 0.0) {
            sLastTimeMS = currentTimeMS;
        }

        const double dtSeconds = (currentTimeMS - sLastTimeMS) * 0.001;
        sLastTimeMS = currentTimeMS;

        this->UpdateAnimation(scast<float>(dtSeconds));
    }
}
