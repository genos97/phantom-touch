#include "video/D3D11Renderer.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dxgi1_5.h>
#include <d3dcommon.h>

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
#ifndef DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
#define DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT 64
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget,
    UINT Flags1, UINT Flags2,
    ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);

// Immutable Microsoft COM IIDs (defined inline for guaranteed static linking across SDK versions)
const GUID kIID_ID3D10Multithread = {
    0x9B7E4E00, 0x342C, 0x4106, { 0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0 }
};
const GUID kIID_IDXGIDevice1 = {
    0x77DB970F, 0x6276, 0x48BA, { 0xBA, 0x28, 0x07, 0x01, 0x43, 0xB4, 0x39, 0x2C }
};
const GUID kIID_IDXGIFactory2 = {
    0x50C83A1C, 0xE072, 0x4C48, { 0x87, 0xB0, 0x36, 0x30, 0xFA, 0x36, 0xA6, 0xD0 }
};
const GUID kIID_IDXGISwapChain2 = {
    0xA8BE2AC4, 0x199F, 0x4946, { 0xB3, 0x31, 0x79, 0x59, 0x9F, 0xB9, 0x8D, 0xE7 }
};
const GUID kIID_IDXGIFactory5 = {
    0x7632E1F5, 0xEE65, 0x4DCA, { 0x87, 0xFD, 0x84, 0xCD, 0x75, 0xF8, 0x83, 0x8D }
};

template <typename T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct QuadCB {
    float rectNdc[4]; // x0, y0 (bottom-left NDC), x1, y1 (top-right NDC)
    float uvRect[4];  // u0, v0, u1, v1 (exact hardware crop window inside NV12 surface)
    float tint[4];    // r, g, b, a for dynamic vector sprites
};

const char* kShaderSrc = R"(
cbuffer QuadCB : register(b0) {
    float4 rectNdc;
    float4 uvRect;
    float4 tint;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut vsMain(uint id : SV_VertexID) {
    float2 uvs[4] = {
        float2(0.0, 0.0),
        float2(1.0, 0.0),
        float2(0.0, 1.0),
        float2(1.0, 1.0)
    };
    float2 t = uvs[id];
    float x = lerp(rectNdc.x, rectNdc.z, t.x);
    float y = lerp(rectNdc.w, rectNdc.y, t.y);
    VSOut o;
    o.pos = float4(x, y, 0.0, 1.0);
    o.uv = float2(lerp(uvRect.x, uvRect.z, t.x), lerp(uvRect.y, uvRect.w, t.y));
    return o;
}

Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState      samp  : register(s0);

float4 psNV12(VSOut i) : SV_TARGET {
    float y = (texY.Sample(samp, i.uv).r - 0.0627451) * 1.16438356;
    float2 c = texUV.Sample(samp, i.uv).rg - float2(0.5, 0.5);
    float r = saturate(y + 1.792741 * c.y);
    float g = saturate(y - 0.213249 * c.x - 0.532909 * c.y);
    float b = saturate(y + 2.112402 * c.x);
    return float4(r, g, b, 1.0);
}

Texture2D<float4> texHud : register(t0);

float4 psHud(VSOut i) : SV_TARGET {
    return texHud.Sample(samp, i.uv);
}

float4 psYOnly(VSOut i) : SV_TARGET {
    return float4(texY.Sample(samp, i.uv).r, 0.0, 0.0, 1.0);
}

float4 psMarker(VSOut i) : SV_TARGET {
    float2 d = i.uv - float2(0.5, 0.5);
    float dist = length(d) * 2.0;
    if (dist > 1.0) discard;
    float rim = smoothstep(0.66, 0.84, dist);
    float edge = 1.0 - smoothstep(0.88, 1.0, dist);
    float3 col = lerp(tint.rgb, float3(1.0, 1.0, 1.0), rim * 0.72);
    float a = tint.a * edge;
    return float4(col * a, a);
}
)";

// Official GDI+ PNG encoder CLSID: {557CF406-1A04-11D3-9A73-0000F81EF32E}
const CLSID kClsidPngEncoder = {
    0x557CF406, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E }
};

uint8_t clampByte(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

} // namespace

D3D11Renderer::D3D11Renderer() {
    InitializeCriticalSection(&d3dLock_);
    InitializeCriticalSection(&frameLock_);
}

D3D11Renderer::~D3D11Renderer() {
    shutdown();
    DeleteCriticalSection(&frameLock_);
    DeleteCriticalSection(&d3dLock_);
}

bool D3D11Renderer::init(HWND targetHwnd, int initialW, int initialH, std::string& outError, bool vsyncEnabled) {
    shutdown();
    if (!targetHwnd || !IsWindow(targetHwnd)) {
        outError = "Invalid HWND for D3D11Renderer.";
        return false;
    }

    EnterCriticalSection(&d3dLock_);

    targetHwnd_ = targetHwnd;
    savedHwnd_ = targetHwnd;
    backbufW_ = (initialW > 8) ? initialW : 640;
    backbufH_ = (initialH > 8) ? initialH : 360;
    savedW_ = backbufW_;
    savedH_ = backbufH_;
    vsyncEnabled_ = vsyncEnabled;
    occluded_ = false;
    InterlockedExchange(&deviceLost_, 0);

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL gotLevel = D3D_FEATURE_LEVEL_11_0;

    // Prefer HARDWARE device with VIDEO_SUPPORT (enables DXVA / NVDEC / QuickSync zero-copy H.264 decode!)
    HRESULT hr = D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, 4, D3D11_SDK_VERSION,
        &device_, &gotLevel, &ctx_);

    if (FAILED(hr)) {
        // Retry HARDWARE without D3D_FEATURE_LEVEL_11_1 / VIDEO_SUPPORT (older drivers)
        hr = D3D11CreateDevice(
            NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
            levels + 1, 3, D3D11_SDK_VERSION,
            &device_, &gotLevel, &ctx_);
    }

    if (SUCCEEDED(hr) && device_ && ctx_) {
        hardwareAccel_ = true;
    } else {
        // Fallback cleanly to Windows WARP software D3D11 rasterizer (also enables headless unit tests!)
        hr = D3D11CreateDevice(
            NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
            levels + 1, 3, D3D11_SDK_VERSION,
            &device_, &gotLevel, &ctx_);
        if (FAILED(hr) || !device_ || !ctx_) {
            LeaveCriticalSection(&d3dLock_);
            outError = "Failed creating Direct3D 11 device.";
            shutdown();
            return false;
        }
        hardwareAccel_ = false;
    }

    // Enable D3D11 multithread protection (required for DXVA MFT + video-thread rendering!)
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(device_->QueryInterface(kIID_ID3D10Multithread, reinterpret_cast<void**>(&mt))) && mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    // Cap GPU swap chain latency to 1 frame (eliminates 30-50ms of queued display lag!)
    IDXGIDevice1* dxgiDev1 = nullptr;
    if (SUCCEEDED(device_->QueryInterface(kIID_IDXGIDevice1, reinterpret_cast<void**>(&dxgiDev1))) && dxgiDev1) {
        dxgiDev1->SetMaximumFrameLatency(1);
        dxgiDev1->Release();
    }

    if (!createSwapChainLocked(targetHwnd_, backbufW_, backbufH_, vsyncEnabled_)) {
        LeaveCriticalSection(&d3dLock_);
        outError = "Failed creating DXGI swap chain.";
        shutdown();
        return false;
    }

    // Check if GPU supports direct DXGI_FORMAT_NV12 Shader Resource Views (D3D11.1+ WDDM 1.2+)
    supportsGpuNv12Srv_ = false;
    if (hardwareAccel_) {
        UINT fmtSupport = 0;
        if (SUCCEEDED(device_->CheckFormatSupport(DXGI_FORMAT_NV12, &fmtSupport)) &&
            (fmtSupport & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0 &&
            (fmtSupport & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) != 0) {
            supportsGpuNv12Srv_ = true;
        }
    }

    if (!compileShadersLocked(outError)) {
        LeaveCriticalSection(&d3dLock_);
        shutdown();
        return false;
    }

    // Constant buffer for NDC rect
    D3D11_BUFFER_DESC cbd;
    ZeroMemory(&cbd, sizeof(cbd));
    cbd.ByteWidth = sizeof(QuadCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&cbd, NULL, &cbQuad_))) {
        LeaveCriticalSection(&d3dLock_);
        outError = "Failed creating D3D11 constant buffer.";
        shutdown();
        return false;
    }

    // Bilinear sampler for smooth scaling
    D3D11_SAMPLER_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sd, &samplerLinear_);

    // Premultiplied-alpha blend state for same-frame GDI+ PARGB keymap HUD compositing
    D3D11_BLEND_DESC bd;
    ZeroMemory(&bd, sizeof(bd));
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&bd, &blendPremul_);

    D3D11_RASTERIZER_DESC rd;
    ZeroMemory(&rd, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    device_->CreateRasterizerState(&rd, &rasterNoCull_);

    std::printf("[D3D11] Initialized (hw=%d flip=%d tearing=%d vsync=%d waitable=%d gpuNV12=%d)\n",
                hardwareAccel_ ? 1 : 0, flipModel_ ? 1 : 0,
                tearingSupported_ ? 1 : 0, vsyncEnabled_ ? 1 : 0,
                frameLatencyWaitable_ ? 1 : 0, supportsGpuNv12Srv_ ? 1 : 0);
    std::fflush(stdout);

    LeaveCriticalSection(&d3dLock_);
    return true;
}

void D3D11Renderer::closeWaitableObjectLocked() {
    if (frameLatencyWaitable_) {
        CloseHandle(frameLatencyWaitable_);
        frameLatencyWaitable_ = NULL;
    }
}

bool D3D11Renderer::createSwapChainLocked(HWND hwnd, int w, int h, bool vsync) {
    if (!device_ || !hwnd || !IsWindow(hwnd)) return false;
    if (w < 8) w = 8;
    if (h < 8) h = 8;

    if (ctx_) ctx_->OMSetRenderTargets(0, NULL, NULL);
    safeRelease(rtv_);
    closeWaitableObjectLocked();
    safeRelease(swapChain_);

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) && dxgiDev) {
        if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter) {
            adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
            adapter->Release();
        }
        dxgiDev->Release();
    }
    if (!factory) return false;

    tearingSupported_ = false;
    IDXGIFactory5* factory5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(kIID_IDXGIFactory5, reinterpret_cast<void**>(&factory5))) && factory5) {
        BOOL allowTearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                    &allowTearing, sizeof(allowTearing))) && allowTearing) {
            tearingSupported_ = true;
        }
        factory5->Release();
    }

    flipModel_ = false;
    swapChainFlags_ = 0;
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(kIID_IDXGIFactory2, reinterpret_cast<void**>(&factory2))) && factory2) {
        DXGI_SWAP_CHAIN_DESC1 scd1;
        ZeroMemory(&scd1, sizeof(scd1));
        scd1.Width = static_cast<UINT>(w);
        scd1.Height = static_cast<UINT>(h);
        scd1.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd1.SampleDesc.Count = 1;
        scd1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd1.BufferCount = 2;
        scd1.Scaling = DXGI_SCALING_STRETCH;
        scd1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (vsync) {
            scd1.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        } else {
            scd1.Flags = tearingSupported_ ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u;
        }

        IDXGISwapChain1* sc1 = nullptr;
        HRESULT hr = factory2->CreateSwapChainForHwnd(device_, hwnd, &scd1, NULL, NULL, &sc1);
        if (FAILED(hr) && scd1.Flags != 0) {
            scd1.Flags = 0;
            if (!vsync) tearingSupported_ = false;
            hr = factory2->CreateSwapChainForHwnd(device_, hwnd, &scd1, NULL, NULL, &sc1);
        }
        if (SUCCEEDED(hr) && sc1) {
            swapChain_ = sc1;
            flipModel_ = true;
            swapChainFlags_ = scd1.Flags;
            if (vsync && (swapChainFlags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
                IDXGISwapChain2* sc2 = nullptr;
                if (SUCCEEDED(sc1->QueryInterface(kIID_IDXGISwapChain2, reinterpret_cast<void**>(&sc2))) && sc2) {
                    sc2->SetMaximumFrameLatency(1);
                    frameLatencyWaitable_ = sc2->GetFrameLatencyWaitableObject();
                    sc2->Release();
                }
            }
        }
        factory2->Release();
    }

    if (!swapChain_) {
        DXGI_SWAP_CHAIN_DESC scd;
        ZeroMemory(&scd, sizeof(scd));
        scd.BufferCount = 2;
        scd.BufferDesc.Width = static_cast<UINT>(w);
        scd.BufferDesc.Height = static_cast<UINT>(h);
        scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow = hwnd;
        scd.SampleDesc.Count = 1;
        scd.Windowed = TRUE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        HRESULT hr = factory->CreateSwapChain(device_, &scd, &swapChain_);
        if (FAILED(hr) || !swapChain_) {
            factory->Release();
            return false;
        }
        flipModel_ = false;
        tearingSupported_ = false;
        swapChainFlags_ = 0;
    }

    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();

    if (!createRenderTargetLocked()) {
        return false;
    }

    targetHwnd_ = hwnd;
    savedHwnd_ = hwnd;
    backbufW_ = w;
    backbufH_ = h;
    savedW_ = w;
    savedH_ = h;
    vsyncEnabled_ = vsync;
    occluded_ = false;
    return true;
}

bool D3D11Renderer::setVsync(bool enabled) {
    EnterCriticalSection(&d3dLock_);
    if (!device_ || !targetHwnd_ || !IsWindow(targetHwnd_)) {
        vsyncEnabled_ = enabled;
        LeaveCriticalSection(&d3dLock_);
        return true;
    }
    if (vsyncEnabled_ == enabled && swapChain_) {
        LeaveCriticalSection(&d3dLock_);
        return true;
    }
    bool ok = createSwapChainLocked(targetHwnd_, backbufW_, backbufH_, enabled);
    LeaveCriticalSection(&d3dLock_);
    return ok;
}

bool D3D11Renderer::rebindSwapChainTarget(HWND newHwnd, int width, int height) {
    if (!newHwnd || !IsWindow(newHwnd)) return false;
    EnterCriticalSection(&d3dLock_);
    if (!device_) {
        LeaveCriticalSection(&d3dLock_);
        return false;
    }
    bool ok = createSwapChainLocked(newHwnd, width, height, vsyncEnabled_);
    LeaveCriticalSection(&d3dLock_);
    return ok;
}

std::string D3D11Renderer::presentationModeLabel() const {
    EnterCriticalSection(&d3dLock_);
    bool flip = flipModel_;
    bool vsync = vsyncEnabled_;
    bool waitable = (frameLatencyWaitable_ != NULL);
    bool tear = tearingSupported_;
    LeaveCriticalSection(&d3dLock_);
    if (!flip) return "LegacyBlt";
    if (vsync) return waitable ? "Flip+VSync(Waitable)" : "Flip+VSync";
    return tear ? "Flip+Tearing" : "Flip";
}

bool D3D11Renderer::createRenderTargetLocked() {
    safeRelease(rtv_);
    if (!swapChain_ || !device_) return false;
    ID3D11Texture2D* backBuf = nullptr;
    if (FAILED(swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuf))) || !backBuf) {
        return false;
    }
    HRESULT hr = device_->CreateRenderTargetView(backBuf, NULL, &rtv_);
    backBuf->Release();
    return SUCCEEDED(hr) && rtv_ != nullptr;
}

bool D3D11Renderer::compileShadersLocked(std::string& outError) {
    HMODULE hComp = LoadLibraryExW(L"d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hComp) hComp = LoadLibraryExW(L"d3dcompiler_43.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hComp) hComp = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!hComp) {
        outError = "d3dcompiler_47.dll not found on system.";
        return false;
    }
    PFN_D3DCompile fnCompile = reinterpret_cast<PFN_D3DCompile>(
        reinterpret_cast<void*>(GetProcAddress(hComp, "D3DCompile")));
    if (!fnCompile) {
        FreeLibrary(hComp);
        outError = "D3DCompile export missing in d3dcompiler_47.dll.";
        return false;
    }

    auto compileOne = [&](const char* entry, const char* target, ID3DBlob** outBlob) -> bool {
        ID3DBlob* errBlob = nullptr;
        HRESULT hr = fnCompile(kShaderSrc, std::strlen(kShaderSrc), "PhantomD3D11.hlsl",
                               NULL, NULL, entry, target, 0, 0, outBlob, &errBlob);
        if (FAILED(hr)) {
            if (errBlob) {
                outError = std::string("HLSL compile error (") + entry + "): " +
                           std::string(static_cast<const char*>(errBlob->GetBufferPointer()),
                                       errBlob->GetBufferSize());
                errBlob->Release();
            } else {
                outError = std::string("HLSL compile failed for ") + entry;
            }
            return false;
        }
        if (errBlob) errBlob->Release();
        return true;
    };

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psNV12Blob = nullptr;
    ID3DBlob* psHudBlob = nullptr;
    ID3DBlob* psYOnlyBlob = nullptr;
    ID3DBlob* psMarkerBlob = nullptr;
    bool ok = compileOne("vsMain", "vs_4_0", &vsBlob) &&
              compileOne("psNV12", "ps_4_0", &psNV12Blob) &&
              compileOne("psHud", "ps_4_0", &psHudBlob) &&
              compileOne("psYOnly", "ps_4_0", &psYOnlyBlob) &&
              compileOne("psMarker", "ps_4_0", &psMarkerBlob);

    if (ok) {
        if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &vsQuad_)) ||
            FAILED(device_->CreatePixelShader(psNV12Blob->GetBufferPointer(), psNV12Blob->GetBufferSize(), NULL, &psNV12_)) ||
            FAILED(device_->CreatePixelShader(psHudBlob->GetBufferPointer(), psHudBlob->GetBufferSize(), NULL, &psHud_)) ||
            FAILED(device_->CreatePixelShader(psYOnlyBlob->GetBufferPointer(), psYOnlyBlob->GetBufferSize(), NULL, &psYOnly_)) ||
            FAILED(device_->CreatePixelShader(psMarkerBlob->GetBufferPointer(), psMarkerBlob->GetBufferSize(), NULL, &psMarker_))) {
            outError = "Failed creating D3D11 shader objects.";
            ok = false;
        }
    }
    // Release all ID3DBlob COM objects BEFORE unloading d3dcompiler_47.dll!
    safeRelease(vsBlob);
    safeRelease(psNV12Blob);
    safeRelease(psHudBlob);
    safeRelease(psYOnlyBlob);
    safeRelease(psMarkerBlob);
    FreeLibrary(hComp);
    return ok;
}

void D3D11Renderer::shutdown() {
    EnterCriticalSection(&d3dLock_);
    safeRelease(probeStageTex_);
    safeRelease(probeRtv_);
    safeRelease(probeRtTex_);
    safeRelease(srvHud_);
    safeRelease(texHud_);
    safeRelease(srvNV12UV_);
    safeRelease(srvNV12Y_);
    safeRelease(texNV12Gpu_);
    safeRelease(srvUV_);
    safeRelease(texUV_);
    safeRelease(srvY_);
    safeRelease(texY_);
    safeRelease(rasterNoCull_);
    safeRelease(blendPremul_);
    safeRelease(samplerLinear_);
    safeRelease(cbQuad_);
    safeRelease(psMarker_);
    safeRelease(psYOnly_);
    safeRelease(psHud_);
    safeRelease(psNV12_);
    safeRelease(vsQuad_);
    safeRelease(rtv_);
    closeWaitableObjectLocked();
    safeRelease(swapChain_);
    safeRelease(ctx_);
    safeRelease(device_);
    texVideoW_ = 0;
    texVideoH_ = 0;
    texGpuW_ = 0;
    texGpuH_ = 0;
    gpuVideoActive_ = false;
    texHudW_ = 0;
    texHudH_ = 0;
    backbufW_ = 0;
    backbufH_ = 0;
    targetHwnd_ = NULL;
    hardwareAccel_ = false;
    flipModel_ = false;
    tearingSupported_ = false;
    supportsGpuNv12Srv_ = false;
    occluded_ = false;
    swapChainFlags_ = 0;
    LeaveCriticalSection(&d3dLock_);
}

bool D3D11Renderer::rebuildDevice(std::string& outError) {
    HWND hwndCopy = savedHwnd_;
    int wCopy = savedW_;
    int hCopy = savedH_;
    bool vsyncCopy = vsyncEnabled_;
    if (!hwndCopy || !IsWindow(hwndCopy)) {
        outError = "Cannot rebuild D3D11 device: window destroyed.";
        return false;
    }
    if (backbufW_ >= 8) wCopy = backbufW_;
    if (backbufH_ >= 8) hCopy = backbufH_;

    // Preserve staged HUD across device reset so HUD re-uploads automatically
    std::vector<uint8_t> hudCopy;
    int hw = 0, hh = 0;
    bool hadHud = false;
    EnterCriticalSection(&frameLock_);
    if (hasHud_ && !stagedHud_.empty()) {
        hudCopy = stagedHud_;
        hw = stagedHudW_;
        hh = stagedHudH_;
        hadHud = true;
    }
    LeaveCriticalSection(&frameLock_);

    if (!init(hwndCopy, wCopy, hCopy, outError, vsyncCopy)) {
        return false;
    }
    if (hadHud) {
        updateHudBitmap(hudCopy.data(), hw, hh);
    }
    return true;
}

void D3D11Renderer::resize(int width, int height) {
    if (width < 8 || height < 8) return;
    EnterCriticalSection(&d3dLock_);
    if (!swapChain_ || !ctx_ || (width == backbufW_ && height == backbufH_)) {
        LeaveCriticalSection(&d3dLock_);
        return;
    }

    ctx_->OMSetRenderTargets(0, NULL, NULL);
    safeRelease(rtv_);
    HRESULT hr = swapChain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                           DXGI_FORMAT_UNKNOWN, swapChainFlags_);
    if (SUCCEEDED(hr) && createRenderTargetLocked()) {
        backbufW_ = width;
        backbufH_ = height;
        savedW_ = width;
        savedH_ = height;
    } else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        InterlockedExchange(&deviceLost_, 1);
    }
    LeaveCriticalSection(&d3dLock_);
}

void D3D11Renderer::submitNV12Frame(const DecodedNV12Frame& frame) {
    if (frame.width < 16 || frame.height < 16 || frame.data.empty()) return;
    EnterCriticalSection(&frameLock_);
    stagedFrame_.width = frame.width;
    stagedFrame_.height = frame.height;
    stagedFrame_.stride = frame.stride;
    stagedFrame_.surfaceH = frame.surfaceH;
    stagedFrame_.data = frame.data;
    videoDirty_ = true;
    hasFrame_ = true;
    LeaveCriticalSection(&frameLock_);
}

bool D3D11Renderer::hasVideoFrame() const {
    EnterCriticalSection(&frameLock_);
    bool h = hasFrame_;
    LeaveCriticalSection(&frameLock_);
    return h;
}

void D3D11Renderer::clearVideoFrame() {
    EnterCriticalSection(&d3dLock_);
    gpuVideoActive_ = false;
    EnterCriticalSection(&frameLock_);
    stagedFrame_.data.clear();
    stagedFrame_.width = 0;
    stagedFrame_.height = 0;
    videoDirty_ = false;
    hasFrame_ = false;
    LeaveCriticalSection(&frameLock_);
    LeaveCriticalSection(&d3dLock_);
}

void D3D11Renderer::updateHudBitmap(const void* bgraPremulPixels, int width, int height) {
    if (!bgraPremulPixels || width < 8 || height < 8) return;
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    EnterCriticalSection(&frameLock_);
    stagedHud_.resize(bytes);
    std::memcpy(stagedHud_.data(), bgraPremulPixels, bytes);
    stagedHudW_ = width;
    stagedHudH_ = height;
    hudDirty_ = true;
    hasHud_ = true;
    LeaveCriticalSection(&frameLock_);
}

void D3D11Renderer::clearHudBitmap() {
    EnterCriticalSection(&frameLock_);
    stagedHud_.clear();
    stagedHudW_ = 0;
    stagedHudH_ = 0;
    hudDirty_ = false;
    hasHud_ = false;
    LeaveCriticalSection(&frameLock_);
}

void D3D11Renderer::setDynamicHudMarkers(const DynamicHudMarkers& markers) {
    EnterCriticalSection(&frameLock_);
    dynMarkers_ = markers;
    LeaveCriticalSection(&frameLock_);
}

bool D3D11Renderer::ensureVideoTexturesLocked(uint32_t w, uint32_t h) {
    if (texY_ && texUV_ && texVideoW_ == w && texVideoH_ == h) return true;
    safeRelease(srvY_);
    safeRelease(texY_);
    safeRelease(srvUV_);
    safeRelease(texUV_);
    texVideoW_ = 0;
    texVideoH_ = 0;

    D3D11_TEXTURE2D_DESC td;
    ZeroMemory(&td, sizeof(td));
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device_->CreateTexture2D(&td, NULL, &texY_)) ||
        FAILED(device_->CreateShaderResourceView(texY_, NULL, &srvY_))) {
        safeRelease(srvY_); safeRelease(texY_);
        return false;
    }

    td.Width = w / 2;
    td.Height = h / 2;
    td.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(device_->CreateTexture2D(&td, NULL, &texUV_)) ||
        FAILED(device_->CreateShaderResourceView(texUV_, NULL, &srvUV_))) {
        safeRelease(srvY_); safeRelease(texY_);
        safeRelease(srvUV_); safeRelease(texUV_);
        return false;
    }

    texVideoW_ = w;
    texVideoH_ = h;
    return true;
}

bool D3D11Renderer::ensureGpuNV12TextureLocked(uint32_t w, uint32_t h) {
    if (!supportsGpuNv12Srv_) return false;
    if (texNV12Gpu_ && srvNV12Y_ && srvNV12UV_ && texGpuW_ == w && texGpuH_ == h) return true;

    safeRelease(srvNV12UV_);
    safeRelease(srvNV12Y_);
    safeRelease(texNV12Gpu_);
    texGpuW_ = 0;
    texGpuH_ = 0;

    D3D11_TEXTURE2D_DESC td;
    ZeroMemory(&td, sizeof(td));
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(device_->CreateTexture2D(&td, NULL, &texNV12Gpu_)) || !texNV12Gpu_) {
        supportsGpuNv12Srv_ = false;
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ZeroMemory(&srvDesc, sizeof(srvDesc));
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    if (FAILED(device_->CreateShaderResourceView(texNV12Gpu_, &srvDesc, &srvNV12Y_))) {
        safeRelease(texNV12Gpu_);
        supportsGpuNv12Srv_ = false;
        return false;
    }

    srvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(device_->CreateShaderResourceView(texNV12Gpu_, &srvDesc, &srvNV12UV_))) {
        safeRelease(srvNV12Y_);
        safeRelease(texNV12Gpu_);
        supportsGpuNv12Srv_ = false;
        return false;
    }

    texGpuW_ = w;
    texGpuH_ = h;
    return true;
}

bool D3D11Renderer::ensureHudTextureLocked(int w, int h) {
    if (texHud_ && texHudW_ == w && texHudH_ == h) return true;
    safeRelease(srvHud_);
    safeRelease(texHud_);
    texHudW_ = 0;
    texHudH_ = 0;

    D3D11_TEXTURE2D_DESC td;
    ZeroMemory(&td, sizeof(td));
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device_->CreateTexture2D(&td, NULL, &texHud_)) ||
        FAILED(device_->CreateShaderResourceView(texHud_, NULL, &srvHud_))) {
        safeRelease(srvHud_); safeRelease(texHud_);
        return false;
    }
    texHudW_ = w;
    texHudH_ = h;
    return true;
}

void D3D11Renderer::updateConstantBufferLocked(float x0Ndc, float y0Ndc, float x1Ndc, float y1Ndc,
                                               float u0, float v0, float u1, float v1,
                                               float r, float g, float b, float a) {
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx_->Map(cbQuad_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        QuadCB* cb = reinterpret_cast<QuadCB*>(ms.pData);
        cb->rectNdc[0] = x0Ndc;
        cb->rectNdc[1] = y0Ndc;
        cb->rectNdc[2] = x1Ndc;
        cb->rectNdc[3] = y1Ndc;
        cb->uvRect[0] = u0;
        cb->uvRect[1] = v0;
        cb->uvRect[2] = u1;
        cb->uvRect[3] = v1;
        cb->tint[0] = r;
        cb->tint[1] = g;
        cb->tint[2] = b;
        cb->tint[3] = a;
        ctx_->Unmap(cbQuad_, 0);
    }
}

bool D3D11Renderer::ensureProbeTargetsLocked() {
    if (probeRtTex_ && probeRtv_ && probeStageTex_) return true;
    safeRelease(probeStageTex_);
    safeRelease(probeRtv_);
    safeRelease(probeRtTex_);

    D3D11_TEXTURE2D_DESC td;
    ZeroMemory(&td, sizeof(td));
    td.Width = 16;
    td.Height = 16;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;

    if (FAILED(device_->CreateTexture2D(&td, NULL, &probeRtTex_)) || !probeRtTex_ ||
        FAILED(device_->CreateRenderTargetView(probeRtTex_, NULL, &probeRtv_)) || !probeRtv_) {
        safeRelease(probeRtv_);
        safeRelease(probeRtTex_);
        return false;
    }

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&td, NULL, &probeStageTex_)) || !probeStageTex_) {
        safeRelease(probeRtv_);
        safeRelease(probeRtTex_);
        return false;
    }
    return true;
}

bool D3D11Renderer::sampleYRectGpuLocked(float normX, float normY, float& outMeanY) {
    ID3D11ShaderResourceView* activeY = gpuVideoActive_ ? srvNV12Y_ : srvY_;
    if (!device_ || !ctx_ || !activeY || !psYOnly_ || !ensureProbeTargetsLocked()) return false;

    uint32_t texW = gpuVideoActive_ ? texGpuW_ : texVideoW_;
    uint32_t texH = gpuVideoActive_ ? texGpuH_ : texVideoH_;
    uint32_t cropX = gpuVideoActive_ ? gpuCropX_ : 0u;
    uint32_t cropY = gpuVideoActive_ ? gpuCropY_ : 0u;
    uint32_t cropW = (gpuVideoActive_ && gpuCropW_ > 0) ? gpuCropW_ : texW;
    uint32_t cropH = (gpuVideoActive_ && gpuCropH_ > 0) ? gpuCropH_ : texH;
    if (texW < 16 || texH < 16 || cropW < 16 || cropH < 16) return false;

    float cx = static_cast<float>(cropX) + std::max(0.02f, std::min(0.98f, normX)) * static_cast<float>(cropW);
    float cy = static_cast<float>(cropY) + std::max(0.02f, std::min(0.98f, normY)) * static_cast<float>(cropH);
    float x0 = std::max(static_cast<float>(cropX), cx - 8.0f);
    float x1 = std::min(static_cast<float>(cropX + cropW), x0 + 16.0f);
    float y0 = std::max(static_cast<float>(cropY), cy - 8.0f);
    float y1 = std::min(static_cast<float>(cropY + cropH), y0 + 16.0f);

    float u0 = x0 / static_cast<float>(texW);
    float v0 = y0 / static_cast<float>(texH);
    float u1 = x1 / static_cast<float>(texW);
    float v1 = y1 / static_cast<float>(texH);

    ctx_->OMSetRenderTargets(1, &probeRtv_, NULL);
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = 16.0f;
    vp.Height = 16.0f;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(rasterNoCull_);
    ctx_->IASetInputLayout(NULL);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vsQuad_, NULL, 0);
    ctx_->VSSetConstantBuffers(0, 1, &cbQuad_);
    ctx_->PSSetSamplers(0, 1, &samplerLinear_);
    ctx_->OMSetBlendState(NULL, NULL, 0xFFFFFFFFu);

    updateConstantBufferLocked(-1.0f, -1.0f, 1.0f, 1.0f, u0, v0, u1, v1);
    ctx_->PSSetShader(psYOnly_, NULL, 0);
    ctx_->PSSetShaderResources(0, 1, &activeY);
    ctx_->Draw(4, 0);

    ID3D11ShaderResourceView* nullSrv = NULL;
    ctx_->PSSetShaderResources(0, 1, &nullSrv);
    ctx_->OMSetRenderTargets(0, NULL, NULL);
    ctx_->CopyResource(probeStageTex_, probeRtTex_);

    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(ctx_->Map(probeStageTex_, 0, D3D11_MAP_READ, 0, &ms)) || !ms.pData) {
        return false;
    }
    uint32_t sum = 0;
    const uint8_t* ptr = static_cast<const uint8_t*>(ms.pData);
    for (int r = 0; r < 16; ++r) {
        const uint8_t* row = ptr + static_cast<size_t>(r) * ms.RowPitch;
        for (int c = 0; c < 16; ++c) {
            sum += row[c];
        }
    }
    ctx_->Unmap(probeStageTex_, 0);
    outMeanY = static_cast<float>(sum) / 256.0f;
    return true;
}

void D3D11Renderer::setProbeFrameEvent(HANDLE evt) {
    EnterCriticalSection(&frameLock_);
    probeFrameEvent_ = evt;
    LeaveCriticalSection(&frameLock_);
}

void D3D11Renderer::setProbeSampleRect(bool active, float normX, float normY) {
    EnterCriticalSection(&frameLock_);
    probeNormX_ = normX;
    probeNormY_ = normY;
    InterlockedExchange(&probeActive_, active ? 1 : 0);
    LeaveCriticalSection(&frameLock_);
}

bool D3D11Renderer::getLastProbeSample(float& outMeanY, int64_t& outPresentQpc, uint64_t& outSeq) const {
    EnterCriticalSection(&frameLock_);
    bool ok = (probeSeq_ > 0);
    outMeanY = lastProbeMeanY_;
    outPresentQpc = lastProbePresentQpc_;
    outSeq = probeSeq_;
    LeaveCriticalSection(&frameLock_);
    return ok;
}

bool D3D11Renderer::sampleGridBrightness(float outGridY[9]) const {
    if (!outGridY) return false;
    D3D11Renderer* self = const_cast<D3D11Renderer*>(this);
    EnterCriticalSection(&d3dLock_);
    bool ok = true;
    const float coords[3] = { 0.25f, 0.50f, 0.75f };
    int idx = 0;
    for (int gy = 0; gy < 3 && ok; ++gy) {
        for (int gx = 0; gx < 3 && ok; ++gx) {
            if (!self->sampleYRectGpuLocked(coords[gx], coords[gy], outGridY[idx++])) {
                ok = false;
            }
        }
    }
    LeaveCriticalSection(&d3dLock_);
    return ok;
}

bool D3D11Renderer::submitAndRenderGpuNV12(ID3D11Texture2D* srcTex, uint32_t subresourceIdx,
                                           uint32_t cropX, uint32_t cropY, uint32_t outW, uint32_t outH,
                                           const ViewportRect& videoRectInHost, bool drawHudOverlay,
                                           FrameTiming* ioTiming) {
    if (!srcTex || outW < 16 || outH < 16) return false;
    EnterCriticalSection(&d3dLock_);
    if (!isInitialized() || !rtv_ || !supportsGpuNv12Srv_) {
        LeaveCriticalSection(&d3dLock_);
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc;
    srcTex->GetDesc(&srcDesc);
    if (srcDesc.Width < 16 || srcDesc.Height < 16 || !ensureGpuNV12TextureLocked(srcDesc.Width, srcDesc.Height)) {
        LeaveCriticalSection(&d3dLock_);
        return false;
    }

    // Full-subresource NV12 GPU copy (pSrcBox = NULL): 100% spec-compliant across ALL NVIDIA/Intel/AMD drivers!
    // Exact macroblock aperture crop is applied zero-cost in the vertex shader via uvRect.
    ctx_->CopySubresourceRegion(texNV12Gpu_, 0, 0, 0, 0, srcTex, subresourceIdx, NULL);

    uint32_t cx = cropX & ~1u;
    uint32_t cy = cropY & ~1u;
    uint32_t cw = outW & ~1u;
    uint32_t ch = outH & ~1u;
    if (cx + cw > srcDesc.Width)  cw = (srcDesc.Width > cx) ? ((srcDesc.Width - cx) & ~1u) : srcDesc.Width;
    if (cy + ch > srcDesc.Height) ch = (srcDesc.Height > cy) ? ((srcDesc.Height - cy) & ~1u) : srcDesc.Height;
    gpuCropX_ = cx;
    gpuCropY_ = cy;
    gpuCropW_ = cw;
    gpuCropH_ = ch;

    gpuVideoActive_ = true;
    EnterCriticalSection(&frameLock_);
    hasFrame_ = true;
    videoDirty_ = false;
    LeaveCriticalSection(&frameLock_);

    renderLocked(videoRectInHost, drawHudOverlay, ioTiming);
    LeaveCriticalSection(&d3dLock_);
    return true;
}

void D3D11Renderer::submitAndRenderCpuNV12(const uint8_t* yPlane, const uint8_t* uvPlane, uint32_t stride,
                                           uint32_t cropX, uint32_t cropY, uint32_t outW, uint32_t outH,
                                           const ViewportRect& videoRectInHost, bool drawHudOverlay,
                                           FrameTiming* ioTiming) {
    if (!yPlane || !uvPlane || outW < 16 || outH < 16) return;
    EnterCriticalSection(&d3dLock_);
    if (!isInitialized() || !rtv_) {
        LeaveCriticalSection(&d3dLock_);
        return;
    }

    if (ensureVideoTexturesLocked(outW, outH)) {
        D3D11_MAPPED_SUBRESOURCE msY;
        if (SUCCEEDED(ctx_->Map(texY_, 0, D3D11_MAP_WRITE_DISCARD, 0, &msY))) {
            uint8_t* dstY = static_cast<uint8_t*>(msY.pData);
            for (uint32_t row = 0; row < outH; ++row) {
                std::memcpy(dstY + static_cast<size_t>(row) * msY.RowPitch,
                            yPlane + static_cast<size_t>(cropY + row) * stride + cropX, outW);
            }
            ctx_->Unmap(texY_, 0);
        }
        D3D11_MAPPED_SUBRESOURCE msUV;
        if (SUCCEEDED(ctx_->Map(texUV_, 0, D3D11_MAP_WRITE_DISCARD, 0, &msUV))) {
            uint8_t* dstUV = static_cast<uint8_t*>(msUV.pData);
            for (uint32_t row = 0; row < outH / 2; ++row) {
                std::memcpy(dstUV + static_cast<size_t>(row) * msUV.RowPitch,
                            uvPlane + static_cast<size_t>(cropY / 2 + row) * stride + cropX, outW);
            }
            ctx_->Unmap(texUV_, 0);
        }

        gpuVideoActive_ = false;
        EnterCriticalSection(&frameLock_);
        hasFrame_ = true;
        videoDirty_ = false;
        LeaveCriticalSection(&frameLock_);

        renderLocked(videoRectInHost, drawHudOverlay, ioTiming);
    }

    LeaveCriticalSection(&d3dLock_);
}

void D3D11Renderer::render(const ViewportRect& videoRectInHost, bool drawHudOverlay, FrameTiming* ioTiming) {
    EnterCriticalSection(&d3dLock_);
    renderLocked(videoRectInHost, drawHudOverlay, ioTiming);
    LeaveCriticalSection(&d3dLock_);
}

void D3D11Renderer::renderLocked(const ViewportRect& videoRectInHost, bool drawHudOverlay, FrameTiming* ioTiming) {
    if (!isInitialized() || !rtv_ || backbufW_ < 8 || backbufH_ < 8 || deviceLost_ != 0) return;

    if (occluded_) {
        HRESULT thr = swapChain_->Present(0, DXGI_PRESENT_TEST);
        if (thr == DXGI_STATUS_OCCLUDED) {
            return;
        }
        if (thr == DXGI_ERROR_DEVICE_REMOVED || thr == DXGI_ERROR_DEVICE_RESET) {
            InterlockedExchange(&deviceLost_, 1);
            return;
        }
        occluded_ = false;
    }

    if (vsyncEnabled_ && frameLatencyWaitable_) {
        WaitForSingleObject(frameLatencyWaitable_, 32);
    }

    // 1. Upload staged NV12 frame if submitNV12Frame() was used (unit tests / external caller)
    bool readyVideo = false;
    EnterCriticalSection(&frameLock_);
    readyVideo = hasFrame_;
    if (videoDirty_ && hasFrame_ && stagedFrame_.width >= 16 && stagedFrame_.height >= 16) {
        const uint32_t w = stagedFrame_.width;
        const uint32_t h = stagedFrame_.height;
        const uint32_t stride = stagedFrame_.stride;
        const uint32_t surfH = stagedFrame_.surfaceH;
        if (ensureVideoTexturesLocked(w, h) &&
            stagedFrame_.data.size() >= static_cast<size_t>(stride) * surfH * 3 / 2) {
            D3D11_MAPPED_SUBRESOURCE msY;
            if (SUCCEEDED(ctx_->Map(texY_, 0, D3D11_MAP_WRITE_DISCARD, 0, &msY))) {
                const uint8_t* srcY = stagedFrame_.data.data();
                uint8_t* dstY = static_cast<uint8_t*>(msY.pData);
                for (uint32_t row = 0; row < h; ++row) {
                    std::memcpy(dstY + static_cast<size_t>(row) * msY.RowPitch,
                                srcY + static_cast<size_t>(row) * stride, w);
                }
                ctx_->Unmap(texY_, 0);
            }
            D3D11_MAPPED_SUBRESOURCE msUV;
            if (SUCCEEDED(ctx_->Map(texUV_, 0, D3D11_MAP_WRITE_DISCARD, 0, &msUV))) {
                const uint8_t* srcUV = stagedFrame_.data.data() + static_cast<size_t>(stride) * surfH;
                uint8_t* dstUV = static_cast<uint8_t*>(msUV.pData);
                for (uint32_t row = 0; row < h / 2; ++row) {
                    std::memcpy(dstUV + static_cast<size_t>(row) * msUV.RowPitch,
                                srcUV + static_cast<size_t>(row) * stride, w);
                }
                ctx_->Unmap(texUV_, 0);
            }
            gpuVideoActive_ = false;
            videoDirty_ = false;
        }
    }

    // 2. Upload fresh frosted-glass HUD bitmap ONLY when modified!
    bool canDrawHud = false;
    if (drawHudOverlay && hasHud_) {
        if (hudDirty_ && stagedHudW_ >= 8 && stagedHudH_ >= 8 && !stagedHud_.empty()) {
            if (ensureHudTextureLocked(stagedHudW_, stagedHudH_)) {
                D3D11_MAPPED_SUBRESOURCE msH;
                if (SUCCEEDED(ctx_->Map(texHud_, 0, D3D11_MAP_WRITE_DISCARD, 0, &msH))) {
                    const uint32_t rowBytes = static_cast<uint32_t>(stagedHudW_) * 4;
                    const uint8_t* src = stagedHud_.data();
                    uint8_t* dst = static_cast<uint8_t*>(msH.pData);
                    for (int row = 0; row < stagedHudH_; ++row) {
                        std::memcpy(dst + static_cast<size_t>(row) * msH.RowPitch,
                                    src + static_cast<size_t>(row) * rowBytes, rowBytes);
                    }
                    ctx_->Unmap(texHud_, 0);
                }
                hudDirty_ = false;
            }
        }
        canDrawHud = (srvHud_ != nullptr);
    }
    DynamicHudMarkers dm = dynMarkers_;
    LeaveCriticalSection(&frameLock_);

    // 3. Clear dark emulator canvas backdrop (#0A0D13)
    const float bg[4] = { 10.0f / 255.0f, 13.0f / 255.0f, 19.0f / 255.0f, 1.0f };
    ctx_->OMSetRenderTargets(1, &rtv_, NULL);
    ctx_->ClearRenderTargetView(rtv_, bg);

    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(backbufW_);
    vp.Height = static_cast<float>(backbufH_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(rasterNoCull_);
    ctx_->IASetInputLayout(NULL);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vsQuad_, NULL, 0);
    ctx_->VSSetConstantBuffers(0, 1, &cbQuad_);
    ctx_->PSSetSamplers(0, 1, &samplerLinear_);

    // Convert aspect-fit video rect (pixels inside ViewportHost) -> NDC [-1..1]
    float x0 = (static_cast<float>(videoRectInHost.x) / static_cast<float>(backbufW_)) * 2.0f - 1.0f;
    float x1 = (static_cast<float>(videoRectInHost.x + videoRectInHost.w) / static_cast<float>(backbufW_)) * 2.0f - 1.0f;
    float yTop = 1.0f - (static_cast<float>(videoRectInHost.y) / static_cast<float>(backbufH_)) * 2.0f;
    float yBot = 1.0f - (static_cast<float>(videoRectInHost.y + videoRectInHost.h) / static_cast<float>(backbufH_)) * 2.0f;

    float vidU0 = 0.0f, vidV0 = 0.0f, vidU1 = 1.0f, vidV1 = 1.0f;
    if (gpuVideoActive_ && texGpuW_ > 0 && texGpuH_ > 0 && gpuCropW_ > 0 && gpuCropH_ > 0) {
        vidU0 = static_cast<float>(gpuCropX_) / static_cast<float>(texGpuW_);
        vidV0 = static_cast<float>(gpuCropY_) / static_cast<float>(texGpuH_);
        vidU1 = static_cast<float>(gpuCropX_ + gpuCropW_) / static_cast<float>(texGpuW_);
        vidV1 = static_cast<float>(gpuCropY_ + gpuCropH_) / static_cast<float>(texGpuH_);
    }

    // 4. Pass 1: Draw decoded phone NV12 video frame (prefer zero-copy GPU NV12 views!)
    ID3D11ShaderResourceView* activeY  = gpuVideoActive_ ? srvNV12Y_  : srvY_;
    ID3D11ShaderResourceView* activeUV = gpuVideoActive_ ? srvNV12UV_ : srvUV_;
    if (readyVideo && activeY && activeUV) {
        updateConstantBufferLocked(x0, yBot, x1, yTop, vidU0, vidV0, vidU1, vidV1);
        ctx_->OMSetBlendState(NULL, NULL, 0xFFFFFFFFu);
        ID3D11ShaderResourceView* srvs[2] = { activeY, activeUV };
        ctx_->PSSetShader(psNV12_, NULL, 0);
        ctx_->PSSetShaderResources(0, 2, srvs);
        ctx_->Draw(4, 0);
        ID3D11ShaderResourceView* nullSrvs[2] = { NULL, NULL };
        ctx_->PSSetShaderResources(0, 2, nullSrvs);
    }

    // 5. Pass 2: Draw frosted-glass keymap HUD overlay in the EXACT SAME FRAME!
    if (canDrawHud) {
        if (!readyVideo || vidU0 != 0.0f || vidV0 != 0.0f || vidU1 != 1.0f || vidV1 != 1.0f) {
            updateConstantBufferLocked(x0, yBot, x1, yTop, 0.0f, 0.0f, 1.0f, 1.0f);
        }
        const float blendFactor[4] = { 0, 0, 0, 0 };
        ctx_->OMSetBlendState(blendPremul_, blendFactor, 0xFFFFFFFFu);
        ctx_->PSSetShader(psHud_, NULL, 0);
        ctx_->PSSetShaderResources(0, 1, &srvHud_);
        ctx_->Draw(4, 0);
        ID3D11ShaderResourceView* nullSrv = NULL;
        ctx_->PSSetShaderResources(0, 1, &nullSrv);
    }

    // 5b. Pass 3 (S5): Zero-upload dynamic GPU markers (crosshair, joystick thumb dot, aim dot, mode flash)
    if (drawHudOverlay && (dm.active || dm.modePulseAlpha > 0.01f) && psMarker_ && videoRectInHost.w > 8 && videoRectInHost.h > 8) {
        const float blendFactor[4] = { 0, 0, 0, 0 };
        ctx_->OMSetBlendState(blendPremul_, blendFactor, 0xFFFFFFFFu);
        ctx_->PSSetShader(psMarker_, NULL, 0);
        ctx_->PSSetConstantBuffers(0, 1, &cbQuad_);

        auto drawMarkerDot = [&](float normX, float normY, float radPx, float r, float g, float b, float a) {
            float cxPx = static_cast<float>(videoRectInHost.x) + normX * static_cast<float>(videoRectInHost.w);
            float cyPx = static_cast<float>(videoRectInHost.y) + normY * static_cast<float>(videoRectInHost.h);
            float mx0 = ((cxPx - radPx) / static_cast<float>(backbufW_)) * 2.0f - 1.0f;
            float mx1 = ((cxPx + radPx) / static_cast<float>(backbufW_)) * 2.0f - 1.0f;
            float myT = 1.0f - ((cyPx - radPx) / static_cast<float>(backbufH_)) * 2.0f;
            float myB = 1.0f - ((cyPx + radPx) / static_cast<float>(backbufH_)) * 2.0f;
            updateConstantBufferLocked(mx0, myB, mx1, myT, 0.0f, 0.0f, 1.0f, 1.0f, r, g, b, a);
            ctx_->Draw(4, 0);
        };

        if (dm.active) {
            float op = (dm.opacity < 0.15f) ? 0.15f : (dm.opacity > 1.0f ? 1.0f : dm.opacity);
            if (dm.showCrosshair) {
                drawMarkerDot(0.5f, 0.5f, 3.0f, 0.17f, 0.90f, 0.61f, 0.88f);
            }
            if (dm.joyDown) {
                drawMarkerDot(dm.joyX, dm.joyY, 6.5f, 0.17f, 0.90f, 0.61f, 0.82f * op);
            }
            if (dm.aimFingerDown) {
                if (dm.fireHeld) {
                    drawMarkerDot(dm.aimX, dm.aimY, 4.5f, 1.00f, 0.36f, 0.42f, 0.78f * op);
                } else {
                    drawMarkerDot(dm.aimX, dm.aimY, 4.5f, 0.00f, 0.83f, 1.00f, 0.78f * op);
                }
            }
        }
        if (dm.modePulseAlpha > 0.01f) {
            float py0 = 1.0f - (static_cast<float>(videoRectInHost.y) / static_cast<float>(backbufH_)) * 2.0f;
            float py1 = 1.0f - (static_cast<float>(videoRectInHost.y + 4) / static_cast<float>(backbufH_)) * 2.0f;
            float px0 = (static_cast<float>(videoRectInHost.x) / static_cast<float>(backbufW_)) * 2.0f - 1.0f;
            float px1 = (static_cast<float>(videoRectInHost.x + videoRectInHost.w) / static_cast<float>(backbufW_)) * 2.0f - 1.0f;
            float a = (dm.modePulseAlpha > 1.0f) ? 1.0f : dm.modePulseAlpha;
            if (dm.modePulseAim) {
                updateConstantBufferLocked(px0, py1, px1, py0, 0.5f, 0.5f, 0.5f, 0.5f, 0.17f, 0.90f, 0.61f, a * 0.85f);
            } else {
                updateConstantBufferLocked(px0, py1, px1, py0, 0.5f, 0.5f, 0.5f, 0.5f, 1.00f, 0.72f, 0.30f, a * 0.85f);
            }
            ctx_->Draw(4, 0);
        }
    }

    UINT syncInterval = vsyncEnabled_ ? 1u : 0u;
    UINT presentFlags = (!vsyncEnabled_ && flipModel_ && tearingSupported_) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const int64_t qpcCall = LatencyTelemetry::nowQpc();
    HRESULT phr = swapChain_->Present(syncInterval, presentFlags);
    const int64_t qpcRet = LatencyTelemetry::nowQpc();
    lastPresentHr_ = phr;
    if (ioTiming) {
        ioTiming->qpcPresentCall = qpcCall;
        ioTiming->qpcPresentRet = qpcRet;
    }
    if (phr == DXGI_STATUS_OCCLUDED) {
        occluded_ = true;
    } else if (phr == DXGI_ERROR_DEVICE_REMOVED || phr == DXGI_ERROR_DEVICE_RESET) {
        InterlockedExchange(&deviceLost_, 1);
    } else {
        occluded_ = false;
        InterlockedExchange(&lastPresentTick_, static_cast<LONG>(GetTickCount()));
        if (probeActive_ != 0 && readyVideo && activeY) {
            float nx = 0.5f, ny = 0.5f;
            HANDLE evt = NULL;
            EnterCriticalSection(&frameLock_);
            nx = probeNormX_;
            ny = probeNormY_;
            evt = probeFrameEvent_;
            LeaveCriticalSection(&frameLock_);
            float meanY = 0.0f;
            if (sampleYRectGpuLocked(nx, ny, meanY)) {
                EnterCriticalSection(&frameLock_);
                lastProbeMeanY_ = meanY;
                lastProbePresentQpc_ = qpcRet;
                ++probeSeq_;
                LeaveCriticalSection(&frameLock_);
                if (evt) SetEvent(evt);
            }
        }
    }
}

bool D3D11Renderer::convertNV12ToBGRA(const DecodedNV12Frame& nv12, std::vector<uint8_t>& outBgra) {
    if (nv12.width < 2 || nv12.height < 2 || nv12.stride < nv12.width) return false;
    if (nv12.data.size() < static_cast<size_t>(nv12.stride) * nv12.surfaceH * 3 / 2) return false;

    const uint32_t w = nv12.width;
    const uint32_t h = nv12.height;
    outBgra.resize(static_cast<size_t>(w) * h * 4);

    const uint8_t* yPlane = nv12.data.data();
    const uint8_t* uvPlane = nv12.data.data() + static_cast<size_t>(nv12.stride) * nv12.surfaceH;

    for (uint32_t row = 0; row < h; ++row) {
        const uint8_t* yRow = yPlane + static_cast<size_t>(row) * nv12.stride;
        const uint8_t* uvRow = uvPlane + static_cast<size_t>(row / 2) * nv12.stride;
        uint8_t* dst = outBgra.data() + static_cast<size_t>(row) * w * 4;

        for (uint32_t col = 0; col < w; ++col) {
            int y = (static_cast<int>(yRow[col]) - 16) * 298;
            int u = static_cast<int>(uvRow[(col & ~1u)]) - 128;
            int v = static_cast<int>(uvRow[(col & ~1u) + 1]) - 128;

            int r = (y + 459 * v + 128) >> 8;
            int g = (y - 55 * u - 136 * v + 128) >> 8;
            int b = (y + 541 * u + 128) >> 8;

            dst[col * 4 + 0] = clampByte(b);
            dst[col * 4 + 1] = clampByte(g);
            dst[col * 4 + 2] = clampByte(r);
            dst[col * 4 + 3] = 255;
        }
    }
    return true;
}

bool D3D11Renderer::saveLatestFramePng(const std::string& savePath, std::string& outError) const {
    DecodedNV12Frame snap;
    bool gotSnap = false;

    EnterCriticalSection(&d3dLock_);
    if (gpuVideoActive_ && texNV12Gpu_ && device_ && ctx_ && texGpuW_ >= 16 && texGpuH_ >= 16) {
        // On-demand GPU->CPU staging readback (zero overhead during normal gameplay!)
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width = texGpuW_;
        td.Height = texGpuH_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        ID3D11Texture2D* stage = nullptr;
        if (SUCCEEDED(device_->CreateTexture2D(&td, NULL, &stage)) && stage) {
            ctx_->CopyResource(stage, texNV12Gpu_);
            D3D11_MAPPED_SUBRESOURCE ms;
            if (SUCCEEDED(ctx_->Map(stage, 0, D3D11_MAP_READ, 0, &ms)) && ms.pData) {
                uint32_t outW = (gpuCropW_ >= 16) ? gpuCropW_ : texGpuW_;
                uint32_t outH = (gpuCropH_ >= 16) ? gpuCropH_ : texGpuH_;
                uint32_t cx = gpuCropX_;
                uint32_t cy = gpuCropY_;
                snap.width = outW;
                snap.height = outH;
                snap.stride = outW;
                snap.surfaceH = outH;
                snap.data.resize(static_cast<size_t>(outW) * outH * 3 / 2);
                const uint8_t* src = static_cast<const uint8_t*>(ms.pData);
                uint8_t* dstY = snap.data.data();
                for (uint32_t r = 0; r < outH; ++r) {
                    std::memcpy(dstY + static_cast<size_t>(r) * outW,
                                src + static_cast<size_t>(cy + r) * ms.RowPitch + cx, outW);
                }
                const uint8_t* srcUV = src + static_cast<size_t>(texGpuH_) * ms.RowPitch;
                uint8_t* dstUV = dstY + static_cast<size_t>(outW) * outH;
                for (uint32_t r = 0; r < outH / 2; ++r) {
                    std::memcpy(dstUV + static_cast<size_t>(r) * outW,
                                srcUV + static_cast<size_t>(cy / 2 + r) * ms.RowPitch + cx, outW);
                }
                ctx_->Unmap(stage, 0);
                gotSnap = true;
            }
            stage->Release();
        }
    } else if (!gpuVideoActive_ && texY_ && texUV_ && device_ && ctx_ && texVideoW_ >= 16 && texVideoH_ >= 16) {
        // Read back from CPU-uploaded dynamic textures via staging textures
        D3D11_TEXTURE2D_DESC tdY;
        ZeroMemory(&tdY, sizeof(tdY));
        tdY.Width = texVideoW_;
        tdY.Height = texVideoH_;
        tdY.MipLevels = 1;
        tdY.ArraySize = 1;
        tdY.Format = DXGI_FORMAT_R8_UNORM;
        tdY.SampleDesc.Count = 1;
        tdY.Usage = D3D11_USAGE_STAGING;
        tdY.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        D3D11_TEXTURE2D_DESC tdUV = tdY;
        tdUV.Width = texVideoW_ / 2;
        tdUV.Height = texVideoH_ / 2;
        tdUV.Format = DXGI_FORMAT_R8G8_UNORM;

        ID3D11Texture2D* stY = nullptr;
        ID3D11Texture2D* stUV = nullptr;
        if (SUCCEEDED(device_->CreateTexture2D(&tdY, NULL, &stY)) && stY &&
            SUCCEEDED(device_->CreateTexture2D(&tdUV, NULL, &stUV)) && stUV) {
            ctx_->CopyResource(stY, texY_);
            ctx_->CopyResource(stUV, texUV_);
            D3D11_MAPPED_SUBRESOURCE msY, msUV;
            if (SUCCEEDED(ctx_->Map(stY, 0, D3D11_MAP_READ, 0, &msY)) && msY.pData) {
                if (SUCCEEDED(ctx_->Map(stUV, 0, D3D11_MAP_READ, 0, &msUV)) && msUV.pData) {
                    snap.width = texVideoW_;
                    snap.height = texVideoH_;
                    snap.stride = texVideoW_;
                    snap.surfaceH = texVideoH_;
                    snap.data.resize(static_cast<size_t>(texVideoW_) * texVideoH_ * 3 / 2);
                    const uint8_t* srcY = static_cast<const uint8_t*>(msY.pData);
                    uint8_t* dstY = snap.data.data();
                    for (uint32_t r = 0; r < texVideoH_; ++r) {
                        std::memcpy(dstY + static_cast<size_t>(r) * texVideoW_,
                                    srcY + static_cast<size_t>(r) * msY.RowPitch, texVideoW_);
                    }
                    const uint8_t* srcUV = static_cast<const uint8_t*>(msUV.pData);
                    uint8_t* dstUV = dstY + static_cast<size_t>(texVideoW_) * texVideoH_;
                    for (uint32_t r = 0; r < texVideoH_ / 2; ++r) {
                        std::memcpy(dstUV + static_cast<size_t>(r) * texVideoW_,
                                    srcUV + static_cast<size_t>(r) * msUV.RowPitch, texVideoW_);
                    }
                    ctx_->Unmap(stUV, 0);
                    gotSnap = true;
                }
                ctx_->Unmap(stY, 0);
            }
        }
        safeRelease(stUV);
        safeRelease(stY);
    }

    if (!gotSnap) {
        EnterCriticalSection(&frameLock_);
        if (hasFrame_ && !stagedFrame_.data.empty()) {
            snap = stagedFrame_;
            gotSnap = true;
        }
        LeaveCriticalSection(&frameLock_);
    }
    LeaveCriticalSection(&d3dLock_);

    if (!gotSnap) {
        outError = "No decoded video frame available yet.";
        return false;
    }

    std::vector<uint8_t> bgra;
    if (!convertNV12ToBGRA(snap, bgra)) {
        outError = "Failed converting frame to BGRA.";
        return false;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, savePath.c_str(), -1, NULL, 0);
    if (wlen <= 1) {
        outError = "Invalid screenshot save path.";
        return false;
    }
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, savePath.c_str(), -1, &wpath[0], wlen);

    Gdiplus::Bitmap bmp(static_cast<INT>(snap.width), static_cast<INT>(snap.height),
                        static_cast<INT>(snap.width * 4), PixelFormat32bppARGB, bgra.data());
    Gdiplus::Status st = bmp.Save(wpath.c_str(), &kClsidPngEncoder, NULL);
    if (st != Gdiplus::Ok) {
        outError = "Failed encoding PNG screenshot.";
        return false;
    }
    return true;
}
