#pragma once

#include "ui/GdiPlusDraw.hpp"
#include "video/H264Decoder.hpp"
#include "video/LatencyTelemetry.hpp"

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
//   4. Renders S5 dynamic per-frame HUD sprites (crosshair, joystick dot, aim finger dot) on GPU
//      with zero GDI+ redraws or texture uploads.
struct DynamicHudMarkers {
    bool active;
    bool showCrosshair;
    bool aimFingerDown;
    float aimX;
    float aimY;
    bool fireHeld;
    bool joyDown;
    float joyX;
    float joyY;
    float opacity;
    float modePulseAlpha;
    bool modePulseAim;

    DynamicHudMarkers()
        : active(false), showCrosshair(false), aimFingerDown(false),
          aimX(0.5f), aimY(0.5f), fireHeld(false), joyDown(false),
          joyX(0.16f), joyY(0.75f), opacity(0.75f),
          modePulseAlpha(0.0f), modePulseAim(false) {}
};

class D3D11Renderer {
public:
    D3D11Renderer();
    ~D3D11Renderer();

    bool init(HWND targetHwnd, int initialW, int initialH, std::string& outError, bool vsyncEnabled = false);
    void shutdown();
    bool isInitialized() const { return device_ != nullptr && swapChain_ != nullptr; }
    bool isHardwareAccelerated() const { return hardwareAccel_; }
    bool isFlipModel() const { return flipModel_; }
    bool isVsyncEnabled() const { return vsyncEnabled_; }
    bool setVsync(bool enabled);
    bool rebindSwapChainTarget(HWND newHwnd, int width, int height);
    std::string presentationModeLabel() const;
    HRESULT lastPresentHResult() const { return lastPresentHr_; }
    bool isOccluded() const { return occluded_; }
    void clearOccluded() { occluded_ = false; }
    bool hasRtv() const { return rtv_ != nullptr; }
    int backbufW() const { return backbufW_; }
    bool supportsGpuNv12() const { return supportsGpuNv12Srv_; }
    bool isDeviceLost() const { return deviceLost_ != 0; }
    bool rebuildDevice(std::string& outError);
    ID3D11Device* d3dDevice() const { return device_; }

    // Resizes swap chain backbuffer to match ViewportHost client dimensions (thread-safe)
    void resize(int width, int height);

    // Zero-copy GPU DXVA fast path: called directly from the video decode thread!
    // Copies cropped NV12 slice in VRAM and presents immediately off the Win32 UI thread.
    bool submitAndRenderGpuNV12(ID3D11Texture2D* srcTex, uint32_t subresourceIdx,
                                uint32_t cropX, uint32_t cropY, uint32_t outW, uint32_t outH,
                                const ViewportRect& videoRectInHost, bool drawHudOverlay,
                                FrameTiming* ioTiming = nullptr);

    // Zero-intermediate-copy CPU NV12 fast path: uploads directly from Media Foundation buffer
    // to D3D11 dynamic Y/UV textures and presents immediately off the Win32 UI thread.
    void submitAndRenderCpuNV12(const uint8_t* yPlane, const uint8_t* uvPlane, uint32_t stride,
                                uint32_t cropX, uint32_t cropY, uint32_t outW, uint32_t outH,
                                const ViewportRect& videoRectInHost, bool drawHudOverlay,
                                FrameTiming* ioTiming = nullptr);

    // Round-trip probe 16x16 Y-plane sampler (LATENCY_PLAN 0.2; zero overhead when inactive)
    void setProbeFrameEvent(HANDLE evt);
    void setProbeSampleRect(bool active, float normX, float normY);
    bool getLastProbeSample(float& outMeanY, int64_t& outPresentQpc, uint64_t& outSeq) const;
    bool sampleGridBrightness(float outGridY[9]) const;

    // Staged NV12 frame path (used by unit tests and external callers)
    void submitNV12Frame(const DecodedNV12Frame& frame);
    bool hasVideoFrame() const;
    void clearVideoFrame();
    DWORD lastPresentTick() const { return static_cast<DWORD>(lastPresentTick_); }
    void markStreamActivated() { InterlockedExchange(&lastPresentTick_, static_cast<LONG>(GetTickCount())); }

    // Thread-safe: submits latest premultiplied-alpha BGRA HUD surface (from HudOverlay/HudRenderer)
    void updateHudBitmap(const void* bgraPremulPixels, int width, int height);
    void clearHudBitmap();

    // S5: Zero-upload dynamic GPU markers (crosshair, active finger dots) updated from InputEngine telemetry
    void setDynamicHudMarkers(const DynamicHudMarkers& markers);

    // Thread-safe: renders dark canvas + letterboxed NV12 video + same-frame HUD overlay and Presents!
    void render(const ViewportRect& videoRectInHost, bool drawHudOverlay, FrameTiming* ioTiming = nullptr);
    // Non-blocking UI-thread variant: returns immediately if video thread currently holds d3dLock_.
    void tryRender(const ViewportRect& videoRectInHost, bool drawHudOverlay, FrameTiming* ioTiming = nullptr);

    // Instant local PNG screenshot straight from the latest GPU/CPU NV12 frame (zero ADB lag!)
    bool saveLatestFramePng(const std::string& savePath, std::string& outError,
                            std::vector<uint8_t>* outDib = nullptr) const;

    // Pure helpers for unit tests and screenshot clipboard (CF_DIB)
    static bool convertNV12ToBGRA(const DecodedNV12Frame& nv12, std::vector<uint8_t>& outBgra);
    static bool buildDibFromBgra(const uint8_t* bgraTopDown, uint32_t w, uint32_t h, std::vector<uint8_t>& outDib);
    static bool buildDibFromPngFile(const std::string& pngPathUtf8, std::vector<uint8_t>& outDib);

private:
    bool createSwapChainLocked(HWND hwnd, int w, int h, bool vsync);
    void closeWaitableObjectLocked();
    bool createRenderTargetLocked();
    bool compileShadersLocked(std::string& outError);
    bool ensureVideoTexturesLocked(uint32_t w, uint32_t h);
    bool ensureGpuNV12TextureLocked(uint32_t w, uint32_t h);
    bool ensureHudTextureLocked(int w, int h);
    bool ensureProbeTargetsLocked();
    bool sampleYRectGpuLocked(float normX, float normY, float& outMeanY);
    void updateConstantBufferLocked(float x0Ndc, float y0Ndc, float x1Ndc, float y1Ndc,
                                    float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f,
                                    float r = 1.0f, float g = 1.0f, float b = 1.0f, float a = 1.0f);
    void renderLocked(const ViewportRect& videoRectInHost, bool drawHudOverlay, FrameTiming* ioTiming = nullptr);

    mutable CRITICAL_SECTION d3dLock_;   // Protects all D3D11/DXGI calls (ALWAYS acquired before frameLock_)
    mutable CRITICAL_SECTION frameLock_; // Protects stagedFrame_ / stagedHud_ / hasFrame_

    HWND targetHwnd_ = NULL;
    HWND savedHwnd_ = NULL;
    int savedW_ = 640;
    int savedH_ = 360;
    int backbufW_ = 0;
    int backbufH_ = 0;
    bool hardwareAccel_ = false;
    bool flipModel_ = false;
    bool tearingSupported_ = false;
    bool vsyncEnabled_ = false;
    HANDLE frameLatencyWaitable_ = NULL;
    bool supportsGpuNv12Srv_ = false;
    bool occluded_ = false;
    volatile LONG deviceLost_ = 0;
    UINT swapChainFlags_ = 0;
    volatile LONG lastPresentTick_ = 0;
    HRESULT lastPresentHr_ = S_OK;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;

    ID3D11VertexShader* vsQuad_ = nullptr;
    ID3D11PixelShader* psNV12_ = nullptr;
    ID3D11PixelShader* psHud_ = nullptr;
    ID3D11PixelShader* psYOnly_ = nullptr;
    ID3D11PixelShader* psMarker_ = nullptr;
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
    size_t stagedHudBytes_ = 0;
    int stagedHudW_ = 0;
    int stagedHudH_ = 0;
    bool hudDirty_ = false;
    bool hasHud_ = false;
    DynamicHudMarkers dynMarkers_;

    // 16x16 probe Y-plane readback targets
    ID3D11Texture2D* probeRtTex_ = nullptr;
    ID3D11RenderTargetView* probeRtv_ = nullptr;
    ID3D11Texture2D* probeStageTex_ = nullptr;
    volatile LONG probeActive_ = 0;
    float probeNormX_ = 0.5f;
    float probeNormY_ = 0.5f;
    float lastProbeMeanY_ = 0.0f;
    int64_t lastProbePresentQpc_ = 0;
    uint64_t probeSeq_ = 0;
    HANDLE probeFrameEvent_ = NULL;
};
