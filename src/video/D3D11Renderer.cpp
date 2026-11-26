#include "video/D3D11Renderer.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <d3d11.h>
#include <dxgi.h>
#include <d3dcommon.h>

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

template <typename T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct QuadCB {
    float rectNdc[4]; // x0, y0 (bottom-left NDC), x1, y1 (top-right NDC)
};

const char* kShaderSrc = R"(
cbuffer QuadCB : register(b0) {
    float4 rectNdc;
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
    float2 uv = uvs[id];
    float x = lerp(rectNdc.x, rectNdc.z, uv.x);
    float y = lerp(rectNdc.w, rectNdc.y, uv.y);
    VSOut o;
    o.pos = float4(x, y, 0.0, 1.0);
    o.uv = uv;
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
    InitializeCriticalSection(&frameLock_);
}

D3D11Renderer::~D3D11Renderer() {
    shutdown();
    DeleteCriticalSection(&frameLock_);
}

bool D3D11Renderer::init(HWND targetHwnd, int initialW, int initialH, std::string& outError) {
    shutdown();
    if (!targetHwnd || !IsWindow(targetHwnd)) {
        outError = "Invalid HWND for D3D11Renderer.";
        return false;
    }

    targetHwnd_ = targetHwnd;
    backbufW_ = (initialW > 8) ? initialW : 640;
    backbufH_ = (initialH > 8) ? initialH : 360;

    DXGI_SWAP_CHAIN_DESC scd;
    ZeroMemory(&scd, sizeof(scd));
    scd.BufferCount = 2;
    scd.BufferDesc.Width = static_cast<UINT>(backbufW_);
    scd.BufferDesc.Height = static_cast<UINT>(backbufH_);
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = targetHwnd_;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL gotLevel = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        levels, 3, D3D11_SDK_VERSION,
        &scd, &swapChain_, &device_, &gotLevel, &ctx_);

    if (SUCCEEDED(hr)) {
        hardwareAccel_ = true;
    } else {
        // Fallback cleanly to Windows WARP software D3D11 rasterizer (also enables headless unit tests!)
        hr = D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
            levels, 3, D3D11_SDK_VERSION,
            &scd, &swapChain_, &device_, &gotLevel, &ctx_);
        if (FAILED(hr)) {
            outError = "Failed creating Direct3D 11 device and swap chain.";
            shutdown();
            return false;
        }
        hardwareAccel_ = false;
    }

    // Disable Alt+Enter DXGI fullscreen hijack so our own Shell F11 fullscreen stays in control
    IDXGIDevice* dxgiDev = nullptr;
    if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) && dxgiDev) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter) {
            IDXGIFactory* factory = nullptr;
            if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))) && factory) {
                factory->MakeWindowAssociation(targetHwnd_, DXGI_MWA_NO_ALT_ENTER);
                factory->Release();
            }
            adapter->Release();
        }
        dxgiDev->Release();
    }

    if (!createRenderTarget()) {
        outError = "Failed creating D3D11 render target view.";
        shutdown();
        return false;
    }
    if (!compileShaders(outError)) {
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

    return true;
}

bool D3D11Renderer::createRenderTarget() {
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

bool D3D11Renderer::compileShaders(std::string& outError) {
    HMODULE hComp = LoadLibraryA("d3dcompiler_47.dll");
    if (!hComp) hComp = LoadLibraryA("d3dcompiler_43.dll");
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
    bool ok = compileOne("vsMain", "vs_4_0", &vsBlob) &&
              compileOne("psNV12", "ps_4_0", &psNV12Blob) &&
              compileOne("psHud", "ps_4_0", &psHudBlob);

    if (ok) {
        if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &vsQuad_)) ||
            FAILED(device_->CreatePixelShader(psNV12Blob->GetBufferPointer(), psNV12Blob->GetBufferSize(), NULL, &psNV12_)) ||
            FAILED(device_->CreatePixelShader(psHudBlob->GetBufferPointer(), psHudBlob->GetBufferSize(), NULL, &psHud_))) {
            outError = "Failed creating D3D11 shader objects.";
            ok = false;
        }
    }
    // Release all ID3DBlob COM objects BEFORE unloading d3dcompiler_47.dll!
    safeRelease(vsBlob);
    safeRelease(psNV12Blob);
    safeRelease(psHudBlob);
    FreeLibrary(hComp);
    return ok;
}

void D3D11Renderer::shutdown() {
    safeRelease(srvHud_);
    safeRelease(texHud_);
    safeRelease(srvUV_);
    safeRelease(texUV_);
    safeRelease(srvY_);
    safeRelease(texY_);
    safeRelease(rasterNoCull_);
    safeRelease(blendPremul_);
    safeRelease(samplerLinear_);
    safeRelease(cbQuad_);
    safeRelease(psHud_);
    safeRelease(psNV12_);
    safeRelease(vsQuad_);
    safeRelease(rtv_);
    safeRelease(swapChain_);
    safeRelease(ctx_);
    safeRelease(device_);
    texVideoW_ = 0;
    texVideoH_ = 0;
    texHudW_ = 0;
    texHudH_ = 0;
    backbufW_ = 0;
    backbufH_ = 0;
    targetHwnd_ = NULL;
    hardwareAccel_ = false;
}

void D3D11Renderer::resize(int width, int height) {
    if (!swapChain_ || !ctx_ || width < 8 || height < 8) return;
    if (width == backbufW_ && height == backbufH_) return;

    ctx_->OMSetRenderTargets(0, NULL, NULL);
    safeRelease(rtv_);
    HRESULT hr = swapChain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                           DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr) && createRenderTarget()) {
        backbufW_ = width;
        backbufH_ = height;
    }
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
    EnterCriticalSection(&frameLock_);
    stagedFrame_.data.clear();
    stagedFrame_.width = 0;
    stagedFrame_.height = 0;
    videoDirty_ = false;
    hasFrame_ = false;
    LeaveCriticalSection(&frameLock_);
}

void D3D11Renderer::updateHudBitmap(const void* bgraPremulPixels, int width, int height) {
    if (!bgraPremulPixels || width < 8 || height < 8) return;
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    stagedHud_.resize(bytes);
    std::memcpy(stagedHud_.data(), bgraPremulPixels, bytes);
    stagedHudW_ = width;
    stagedHudH_ = height;
    hudDirty_ = true;
    hasHud_ = true;
}

void D3D11Renderer::clearHudBitmap() {
    stagedHud_.clear();
    stagedHudW_ = 0;
    stagedHudH_ = 0;
    hudDirty_ = false;
    hasHud_ = false;
}

bool D3D11Renderer::ensureVideoTextures(uint32_t w, uint32_t h) {
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

bool D3D11Renderer::ensureHudTexture(int w, int h) {
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

void D3D11Renderer::updateConstantBuffer(float x0Ndc, float y0Ndc, float x1Ndc, float y1Ndc) {
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx_->Map(cbQuad_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        QuadCB* cb = reinterpret_cast<QuadCB*>(ms.pData);
        cb->rectNdc[0] = x0Ndc;
        cb->rectNdc[1] = y0Ndc;
        cb->rectNdc[2] = x1Ndc;
        cb->rectNdc[3] = y1Ndc;
        ctx_->Unmap(cbQuad_, 0);
    }
}

void D3D11Renderer::render(const ViewportRect& videoRectInHost, bool drawHudOverlay) {
    if (!isInitialized() || !rtv_ || backbufW_ < 8 || backbufH_ < 8) return;

    // 1. Upload fresh NV12 frame if decode thread delivered one
    bool readyVideo = false;
    EnterCriticalSection(&frameLock_);
    readyVideo = hasFrame_;
    if (videoDirty_ && hasFrame_ && stagedFrame_.width >= 16 && stagedFrame_.height >= 16) {
        const uint32_t w = stagedFrame_.width;
        const uint32_t h = stagedFrame_.height;
        const uint32_t stride = stagedFrame_.stride;
        const uint32_t surfH = stagedFrame_.surfaceH;
        if (ensureVideoTextures(w, h) &&
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
            videoDirty_ = false;
        }
    }
    LeaveCriticalSection(&frameLock_);

    // 2. Upload fresh frosted-glass HUD bitmap if modified (zero work between HUD ticks!)
    if (drawHudOverlay && hudDirty_ && hasHud_ && stagedHudW_ >= 8 && stagedHudH_ >= 8) {
        if (ensureHudTexture(stagedHudW_, stagedHudH_)) {
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
    updateConstantBuffer(x0, yBot, x1, yTop);

    // 4. Pass 1: Draw decoded phone NV12 video frame
    if (readyVideo && srvY_ && srvUV_) {
        ctx_->OMSetBlendState(NULL, NULL, 0xFFFFFFFFu);
        ID3D11ShaderResourceView* srvs[2] = { srvY_, srvUV_ };
        ctx_->PSSetShader(psNV12_, NULL, 0);
        ctx_->PSSetShaderResources(0, 2, srvs);
        ctx_->Draw(4, 0);
        ID3D11ShaderResourceView* nullSrvs[2] = { NULL, NULL };
        ctx_->PSSetShaderResources(0, 2, nullSrvs);
    }

    // 5. Pass 2: Draw frosted-glass keymap HUD overlay in the EXACT SAME FRAME!
    if (drawHudOverlay && hasHud_ && srvHud_) {
        const float blendFactor[4] = { 0, 0, 0, 0 };
        ctx_->OMSetBlendState(blendPremul_, blendFactor, 0xFFFFFFFFu);
        ctx_->PSSetShader(psHud_, NULL, 0);
        ctx_->PSSetShaderResources(0, 1, &srvHud_);
        ctx_->Draw(4, 0);
        ID3D11ShaderResourceView* nullSrv = NULL;
        ctx_->PSSetShaderResources(0, 1, &nullSrv);
    }

    swapChain_->Present(0, 0);
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
    EnterCriticalSection(&frameLock_);
    if (!hasFrame_ || stagedFrame_.data.empty()) {
        LeaveCriticalSection(&frameLock_);
        outError = "No decoded video frame available yet.";
        return false;
    }
    snap = stagedFrame_;
    LeaveCriticalSection(&frameLock_);

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
