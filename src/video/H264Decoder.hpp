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

typedef void (*DecodedFrameFn)(void* ctx, const DecodedNV12Frame& frame);

struct IMFTransform;
struct IMFSample;

// Hardware-accelerated (DXVA / Media Foundation) Annex-B H.264 decoder -> NV12.
// Uses Windows built-in CMSH264DecoderMFT with zero external codec DLLs.
// Handles scrcpy SPS/PPS config packets and live portrait <-> landscape stream changes.
class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    bool init(uint32_t initialW, uint32_t initialH, std::string& outError);
    void shutdown();
    bool isInitialized() const { return mft_ != nullptr; }

    void setFrameCallback(DecodedFrameFn fn, void* ctx);

    // Feeds one framed scrcpy H.264 packet (config SPS/PPS or encoded frame).
    // Invokes the frame callback synchronously for each decoded NV12 frame produced.
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
    bool mfStarted_ = false;
    bool comInitialized_ = false;
    bool mftProvidesSamples_ = false;

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
};
