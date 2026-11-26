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

// Single-pass Direct3D 11 viewport renderer:
//   1. Uploads decoded NV12 (Y + UV) frames from Windows Media Foundation H264Decoder
//   2. Renders BT.709 NV12 -> RGB inside the exact letterboxed phone viewport rect
//   3. Composites the frosted-glass GDI+ keymap HUD (premultiplied BGRA) in the SAME frame
//      (zero separate floating overlay window!)
//   4. Captures instant zero-latency PNG screenshots directly from the latest decoded frame.
class D3D11Renderer {
public:
    D3D11Renderer();
    ~D3D11Renderer();

    bool init(HWND targetHwnd, int initialW, int initialH, std::string& outError);
    void shutdown();
    bool isInitialized() const { return device_ != nullptr && swapChain_ != nullptr; }
    bool isHardwareAccelerated() const { return hardwareAccel_; }

    // Resizes swap chain backbuffer to match ViewportHost client dimensions
    void resize(int width, int height);

    // Thread-safe: called from ScrcpyBridge's video decode thread whenever Media Foundation emits a frame.
    void submitNV12Frame(const DecodedNV12Frame& frame);
    bool hasVideoFrame() const;
    void clearVideoFrame();

    // UI-thread: submits latest premultiplied-alpha BGRA HUD surface (from HudOverlay/HudRenderer)
    void updateHudBitmap(const void* bgraPremulPixels, int width, int height);
    void clearHudBitmap();

    // UI-thread: renders dark canvas + letterboxed NV12 video + same-frame HUD overlay and Presents!
    void render(const ViewportRect& videoRectInHost, bool drawHudOverlay);

    // Instant local PNG screenshot straight from the latest decoded NV12 frame (zero ADB lag!)
    bool saveLatestFramePng(const std::string& savePath, std::string& outError) const;

    // Pure helper for unit tests: converts an NV12 frame into tightly packed 32-bit BGRA
    static bool convertNV12ToBGRA(const DecodedNV12Frame& nv12, std::vector<uint8_t>& outBgra);

private:
    bool createRenderTarget();
    bool compileShaders(std::string& outError);
    bool ensureVideoTextures(uint32_t w, uint32_t h);
    bool ensureHudTexture(int w, int h);
    void updateConstantBuffer(float x0Ndc, float y0Ndc, float x1Ndc, float y1Ndc);

    HWND targetHwnd_ = NULL;
    int backbufW_ = 0;
    int backbufH_ = 0;
    bool hardwareAccel_ = false;

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

    ID3D11Texture2D* texY_ = nullptr;
    ID3D11ShaderResourceView* srvY_ = nullptr;
    ID3D11Texture2D* texUV_ = nullptr;
    ID3D11ShaderResourceView* srvUV_ = nullptr;
    uint32_t texVideoW_ = 0;
    uint32_t texVideoH_ = 0;

    ID3D11Texture2D* texHud_ = nullptr;
    ID3D11ShaderResourceView* srvHud_ = nullptr;
    int texHudW_ = 0;
    int texHudH_ = 0;

    mutable CRITICAL_SECTION frameLock_;
    DecodedNV12Frame stagedFrame_;
    bool videoDirty_ = false;
    bool hasFrame_ = false;

    std::vector<uint8_t> stagedHud_;
    int stagedHudW_ = 0;
    int stagedHudH_ = 0;
    bool hudDirty_ = false;
    bool hasHud_ = false;
};
