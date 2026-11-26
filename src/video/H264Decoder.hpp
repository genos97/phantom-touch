#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

struct DecodedNV12Frame {
    uint32_t width = 0;       // Display width (cropped)
    uint32_t height = 0;      // Display height (cropped)
    uint32_t stride = 0;      // Row pitch in bytes (>= width)
    uint32_t surfaceH = 0;    // Coded Y-plane height (UV plane starts at stride * surfaceH)
    std::vector<uint8_t> data; // Compact NV12 buffer: Y (stride * surfaceH) + UV (stride * surfaceH / 2)
};

struct ID3D11Device;
struct ID3D11Texture2D;
struct IMFTransform;
struct IMFSample;
struct IMFDXGIDeviceManager;

typedef void (*DecodedFrameFn)(void* ctx, const DecodedNV12Frame& frame);
typedef void (*DecodedGpuFrameFn)(void* ctx, ID3D11Texture2D* nv12Tex, uint32_t subresourceIdx,
                                  uint32_t cropX, uint32_t cropY, uint32_t outW, uint32_t outH);
typedef void (*DecodedCpuViewFn)(void* ctx, const uint8_t* yPlane, const uint8_t* uvPlane,
                                 uint32_t stride, uint32_t cropX, uint32_t cropY,
                                 uint32_t outW, uint32_t outH);

// Hardware-accelerated (DXVA / D3D11 Media Foundation) Annex-B H.264 decoder -> NV12.
// Uses Windows built-in CMSH264DecoderMFT with zero external codec DLLs.
// When given a video-capable ID3D11Device, decodes directly on the GPU ASIC (NVDEC / QuickSync)
// and emits zero-copy ID3D11Texture2D NV12 surfaces. Falls back seamlessly to CPU NV12 otherwise.
class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    bool init(uint32_t initialW, uint32_t initialH, std::string& outError,
              ID3D11Device* d3dDevice = nullptr);
    void shutdown();
    bool isInitialized() const { return mft_ != nullptr; }
    bool isHardwareAccelerated() const { return dxvaActive_; }

    void setFrameCallback(DecodedFrameFn fn, void* ctx);
    void setGpuFrameCallback(DecodedGpuFrameFn fn, void* ctx);
    void setCpuViewCallback(DecodedCpuViewFn fn, void* ctx);

    // Feeds one framed scrcpy H.264 packet (config SPS/PPS or encoded frame).
    // Invokes the configured frame callback synchronously for each decoded frame produced.
    // Returns true if at least one frame was decoded.
    bool decodePacket(const uint8_t* data, size_t size, uint64_t ptsUs,
                      bool isConfig, bool isKeyFrame);

    void flush();

private:
    bool configureOutputNV12();
    void updateOutputFormatMetadata();
    bool drainOutputs(bool& decodedAny);

    IMFTransform* mft_ = nullptr;
    IMFSample* reusableOutSample_ = nullptr;
    IMFDXGIDeviceManager* dxgiMgr_ = nullptr;
    bool mfStarted_ = false;
    bool comInitialized_ = false;
    bool mftProvidesSamples_ = false;
    bool dxvaActive_ = false;

    uint32_t codedW_ = 1920;
    uint32_t codedH_ = 1080;
    uint32_t cropX_ = 0;
    uint32_t cropY_ = 0;
    uint32_t cropW_ = 1920;
    uint32_t cropH_ = 1080;
    uint32_t defaultStride_ = 1920;
    uint32_t lastSpsW_ = 0;
    uint32_t lastSpsH_ = 0;

    std::vector<uint8_t> spsPps_;
    bool pendingConfigPrepend_ = false;
    bool sawFirstKeyFrame_ = false;

    DecodedNV12Frame workFrame_;
    DecodedFrameFn frameCb_ = nullptr;
    void* frameCbCtx_ = nullptr;
    DecodedGpuFrameFn gpuFrameCb_ = nullptr;
    void* gpuFrameCbCtx_ = nullptr;
    DecodedCpuViewFn cpuViewCb_ = nullptr;
    void* cpuViewCbCtx_ = nullptr;
};
