#pragma once

#include "ui/GdiPlusDraw.hpp"
#include "video/H264Decoder.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11Buffer;
struct ID3D11SamplerState;
struct ID3D11BlendState;
struct ID3D11RasterizerState;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

// Thread-safe Direct3D 11 viewport renderer (Windows 10/11 Flip-Model + DXVA GPU Zero-Copy):
//   1. Accepts GPU DXVA ID3D11Texture2D NV12 surfaces directly in VRAM (zero CPU copies!)
//      or falls back seamlessly to direct CPU NV12 upload (zero intermediate copies).
//   2. Renders BT.709 NV12 -> RGB inside the exact letterboxed phone viewport rect.
//   3. Composites the frosted-glass GDI+ keymap HUD (premultiplied BGRA) in the SAME frame.
//   4. Safe to present directly from the video decode thread so the Win32 UI / input-hook
//      thread stays <1% CPU and never blocks raw mouse aim or keyboard hooks!
class D3D11Renderer {
public:
    D3D11Renderer();
    ~D3D11Renderer();

    bool init(HWND targetHwnd, int initialW, int initialH, std::string& outError);
    void shutdown();
    bool isInitialized() const { return device_ != nullptr && swapChain_ != nullptr; }
    bool isHardwareAccelerated() const { return hardwareAccel_; }
    bool isFlipModel() const { return flipModel_; }
    bool supportsGpuNv12() const { return supportsGpuNv12Srv_; }
    ID3D11Device* d3dDevice() const { return device_; }

    // Resizes swap chain backbuffer to match ViewportHost client dimensions (thread-safe)
    void resize(int width, int height);

    // Zero-copy GPU DXVA fast path: called directly from the video decode thread!
    // Copies cropped NV12 slice in VRAM and presents immediately off the Win32 UI thread.
    bool submitAndRenderGpuNV12(ID3D11Texture2D* srcTex, uint32_t subresourceIdx,
                                uint32_t cropX, uint32_t cropY, uint32_t outW, uint32_t outH,
                                const ViewportRect& videoRectInHost, bool drawHudOverlay);

    // Zero-intermediate-copy CPU NV12 fast path: uploads directly from Media Foundation buffer
    // to D3D11 dynamic Y/UV textures and presents immediately off the Win32 UI thread.
    void submitAndRenderCpuNV12(const uint8_t* yPlane, const uint8_t* uvPlane, uint32_t stride,
                                uint32_t cropX, uint32_t cropY, uint32_t outW, uint32_t outH,
                                const ViewportRect& videoRectInHost, bool drawHudOverlay);

    // Staged NV12 frame path (used by unit tests and external callers)
    void submitNV12Frame(const DecodedNV12Frame& frame);
    bool hasVideoFrame() const;
    void clearVideoFrame();
    DWORD lastPresentTick() const { return static_cast<DWORD>(lastPresentTick_); }

    // Thread-safe: submits latest premultiplied-alpha BGRA HUD surface (from HudOverlay/HudRenderer)
    void updateHudBitmap(const void* bgraPremulPixels, int width, int height);
    void clearHudBitmap();

    // Thread-safe: renders dark canvas + letterboxed NV12 video + same-frame HUD overlay and Presents!
    void render(const ViewportRect& videoRectInHost, bool drawHudOverlay);

    // Instant local PNG screenshot straight from the latest GPU/CPU NV12 frame (zero ADB lag!)
    bool saveLatestFramePng(const std::string& savePath, std::string& outError) const;

    // Pure helper for unit tests: converts an NV12 frame into tightly packed 32-bit BGRA
    static bool convertNV12ToBGRA(const DecodedNV12Frame& nv12, std::vector<uint8_t>& outBgra);

private:
    bool createRenderTargetLocked();
    bool compileShadersLocked(std::string& outError);
    bool ensureVideoTexturesLocked(uint32_t w, uint32_t h);
    bool ensureGpuNV12TextureLocked(uint32_t w, uint32_t h);
    bool ensureHudTextureLocked(int w, int h);
    void updateConstantBufferLocked(float x0Ndc, float y0Ndc, float x1Ndc, float y1Ndc,
                                    float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f);
    void renderLocked(const ViewportRect& videoRectInHost, bool drawHudOverlay);

    mutable CRITICAL_SECTION d3dLock_;   // Protects all D3D11/DXGI calls (ALWAYS acquired before frameLock_)
    mutable CRITICAL_SECTION frameLock_; // Protects stagedFrame_ / stagedHud_ / hasFrame_

    HWND targetHwnd_ = NULL;
    int backbufW_ = 0;
    int backbufH_ = 0;
    bool hardwareAccel_ = false;
    bool flipModel_ = false;
    bool tearingSupported_ = false;
    bool supportsGpuNv12Srv_ = false;
    UINT swapChainFlags_ = 0;
    volatile LONG lastPresentTick_ = 0;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;

    ID3D11VertexShader* vsQuad_ = nullptr;
    ID3D11PixelShader* psNV12_ = nullptr;
    ID3D11PixelShader* psHud_ = nullptr;
    ID3D11Buffer* cbQuad_ = nullptr;
    ID3D11SamplerState* samplerLinear_ = nullptr;
    ID3D11BlendState* blendPremul_ = nullptr;
    ID3D11RasterizerState* rasterNoCull_ = nullptr;

    // CPU-uploaded NV12 textures (fallback / unit tests)
    ID3D11Texture2D* texY_ = nullptr;
    ID3D11ShaderResourceView* srvY_ = nullptr;
    ID3D11Texture2D* texUV_ = nullptr;
    ID3D11ShaderResourceView* srvUV_ = nullptr;
    uint32_t texVideoW_ = 0;
    uint32_t texVideoH_ = 0;

    // Zero-copy GPU-only DXVA NV12 texture + Y/UV shader views
    ID3D11Texture2D* texNV12Gpu_ = nullptr;
    ID3D11ShaderResourceView* srvNV12Y_ = nullptr;
    ID3D11ShaderResourceView* srvNV12UV_ = nullptr;
    uint32_t texGpuW_ = 0;
    uint32_t texGpuH_ = 0;
    uint32_t gpuCropX_ = 0;
    uint32_t gpuCropY_ = 0;
    uint32_t gpuCropW_ = 0;
    uint32_t gpuCropH_ = 0;
    bool gpuVideoActive_ = false;

    ID3D11Texture2D* texHud_ = nullptr;
    ID3D11ShaderResourceView* srvHud_ = nullptr;
    int texHudW_ = 0;
    int texHudH_ = 0;

    DecodedNV12Frame stagedFrame_;
    bool videoDirty_ = false;
    bool hasFrame_ = false;

    std::vector<uint8_t> stagedHud_;
    int stagedHudW_ = 0;
    int stagedHudH_ = 0;
    bool hudDirty_ = false;
    bool hasHud_ = false;
};
