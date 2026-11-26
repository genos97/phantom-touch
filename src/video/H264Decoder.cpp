#include "video/H264Decoder.hpp"
#include "bridge/SpsParser.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <icodecapi.h>

#include <cstdio>
#include <cstring>

namespace {

// Official immutable Microsoft COM GUIDs (defined inline so linking never breaks across SDK variants)
const GUID kCLSID_CMSH264DecoderMFT = {
    0x62CE7E72, 0x4C71, 0x4D20, { 0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D }
};
const GUID kCODECAPI_AVLowLatencyMode = {
    0x9C27891A, 0xED7A, 0x40E1, { 0x88, 0xE8, 0xB2, 0x27, 0x27, 0xA0, 0x24, 0xEE }
};
const GUID kMF_LOW_LATENCY = {
    0x9C27891A, 0xED7A, 0x40E1, { 0x88, 0xE8, 0xB2, 0x27, 0x27, 0xA0, 0x24, 0xEE }
};
const GUID kIID_ICodecAPI = {
    0x901DB4C7, 0x31CE, 0x41A2, { 0x85, 0xDC, 0x8F, 0xA0, 0xBF, 0x41, 0xB8, 0xDA }
};

template <typename T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace

H264Decoder::H264Decoder() {}

H264Decoder::~H264Decoder() {
    shutdown();
}

void H264Decoder::setFrameCallback(DecodedFrameFn fn, void* ctx) {
    frameCb_ = fn;
    frameCbCtx_ = ctx;
}

bool H264Decoder::init(uint32_t initialW, uint32_t initialH, std::string& outError) {
    shutdown();

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        comInitialized_ = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        outError = "COM initialization failed for H264Decoder.";
        return false;
    }

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        outError = "Windows Media Foundation MFStartup failed.";
        shutdown();
        return false;
    }
    mfStarted_ = true;

    hr = CoCreateInstance(kCLSID_CMSH264DecoderMFT, NULL, CLSCTX_INPROC_SERVER,
                          IID_IMFTransform, reinterpret_cast<void**>(&mft_));
    if (FAILED(hr) || !mft_) {
        outError = "Windows built-in H.264 Media Foundation decoder (CMSH264DecoderMFT) unavailable.";
        shutdown();
        return false;
    }

    // Request ultra-low-latency zero-reorder decoding for real-time screen mirroring
    IMFAttributes* attrs = nullptr;
    if (SUCCEEDED(mft_->GetAttributes(&attrs)) && attrs) {
        attrs->SetUINT32(kMF_LOW_LATENCY, 1);
        attrs->Release();
    }
    ICodecAPI* codecApi = nullptr;
    if (SUCCEEDED(mft_->QueryInterface(kIID_ICodecAPI, reinterpret_cast<void**>(&codecApi))) && codecApi) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&kCODECAPI_AVLowLatencyMode, &var);
        codecApi->Release();
    }

    codedW_ = (initialW >= 64) ? initialW : 1920;
    codedH_ = (initialH >= 64) ? initialH : 1080;
    cropX_ = 0;
    cropY_ = 0;
    cropW_ = codedW_;
    cropH_ = codedH_;
    defaultStride_ = codedW_;

    IMFMediaType* inType = nullptr;
    hr = MFCreateMediaType(&inType);
    if (FAILED(hr) || !inType) {
        outError = "Failed creating H.264 input media type.";
        shutdown();
        return false;
    }
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, codedW_, codedH_);

    hr = mft_->SetInputType(0, inType, 0);
    safeRelease(inType);
    if (FAILED(hr)) {
        outError = "H.264 MFT rejected input media type.";
        shutdown();
        return false;
    }

    if (!configureOutputNV12()) {
        outError = "H.264 MFT did not expose NV12 output format.";
        shutdown();
        return false;
    }

    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    spsPps_.clear();
    pendingConfigPrepend_ = false;
    sawFirstKeyFrame_ = false;
    lastSpsW_ = codedW_;
    lastSpsH_ = codedH_;
    return true;
}

void H264Decoder::shutdown() {
    safeRelease(reusableOutSample_);
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        safeRelease(mft_);
    }
    if (mfStarted_) {
        MFShutdown();
        mfStarted_ = false;
    }
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
    spsPps_.clear();
    pendingConfigPrepend_ = false;
    sawFirstKeyFrame_ = false;
}

bool H264Decoder::configureOutputNV12() {
    if (!mft_) return false;
    for (DWORD idx = 0; idx < 32; ++idx) {
        IMFMediaType* outType = nullptr;
        HRESULT hr = mft_->GetOutputAvailableType(0, idx, &outType);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr) || !outType) break;

        GUID subtype = GUID_NULL;
        if (SUCCEEDED(outType->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) {
            hr = mft_->SetOutputType(0, outType, 0);
            safeRelease(outType);
            if (SUCCEEDED(hr)) {
                updateOutputFormatMetadata();
                return true;
            }
        } else {
            safeRelease(outType);
        }
    }
    return false;
}

void H264Decoder::updateOutputFormatMetadata() {
    if (!mft_) return;
    IMFMediaType* curType = nullptr;
    if (SUCCEEDED(mft_->GetOutputCurrentType(0, &curType)) && curType) {
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(curType, MF_MT_FRAME_SIZE, &w, &h)) && w >= 16 && h >= 16) {
            codedW_ = w;
            codedH_ = h;
        }
        UINT32 strideVal = 0;
        if (SUCCEEDED(curType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideVal)) && strideVal >= codedW_) {
            defaultStride_ = strideVal;
        } else {
            defaultStride_ = codedW_;
        }

        MFVideoArea area;
        ZeroMemory(&area, sizeof(area));
        UINT32 blobSize = 0;
        if (SUCCEEDED(curType->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                       reinterpret_cast<UINT8*>(&area), sizeof(area), &blobSize)) &&
            blobSize == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
            cropX_ = static_cast<uint32_t>(area.OffsetX.value >= 0 ? area.OffsetX.value : 0);
            cropY_ = static_cast<uint32_t>(area.OffsetY.value >= 0 ? area.OffsetY.value : 0);
            cropW_ = static_cast<uint32_t>(area.Area.cx);
            cropH_ = static_cast<uint32_t>(area.Area.cy);
        } else {
            cropX_ = 0;
            cropY_ = 0;
            cropW_ = codedW_;
            cropH_ = codedH_;
        }
        if (cropX_ + cropW_ > codedW_) cropW_ = codedW_ - cropX_;
        if (cropY_ + cropH_ > codedH_) cropH_ = codedH_ - cropY_;
        safeRelease(curType);
    }

    MFT_OUTPUT_STREAM_INFO info;
    ZeroMemory(&info, sizeof(info));
    if (SUCCEEDED(mft_->GetOutputStreamInfo(0, &info))) {
        mftProvidesSamples_ = ((info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                                MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0);
        safeRelease(reusableOutSample_);
        if (!mftProvidesSamples_) {
            DWORD bufSize = info.cbSize;
            DWORD minSize = defaultStride_ * codedH_ * 3 / 2;
            if (bufSize < minSize) bufSize = minSize;
            IMFMediaBuffer* buf = nullptr;
            if (SUCCEEDED(MFCreateSample(&reusableOutSample_)) &&
                SUCCEEDED(MFCreateMemoryBuffer(bufSize, &buf))) {
                reusableOutSample_->AddBuffer(buf);
                safeRelease(buf);
            } else {
                safeRelease(reusableOutSample_);
                safeRelease(buf);
            }
        }
    }
}

void H264Decoder::flush() {
    if (!mft_) return;
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    sawFirstKeyFrame_ = false;
}

bool H264Decoder::decodePacket(const uint8_t* data, size_t size, uint64_t ptsUs,
                               bool isConfig, bool isKeyFrame) {
    if (!mft_ || !data || size == 0) return false;

    if (isConfig) {
        uint32_t newW = 0, newH = 0;
        if (SpsParser::parseH264Resolution(data, size, newW, newH) &&
            newW >= 64 && newH >= 64) {
            if (sawFirstKeyFrame_ && (newW != lastSpsW_ || newH != lastSpsH_)) {
                // Screen rotated or virtual display resized: flush stale frames so MFT adapts immediately
                mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
                sawFirstKeyFrame_ = false;
            }
            lastSpsW_ = newW;
            lastSpsH_ = newH;
        }
        spsPps_.assign(data, data + size);
        pendingConfigPrepend_ = true;
        return false;
    }

    // Wait for first keyframe after start/flush so MFT never decodes garbage P-frames
    if (!sawFirstKeyFrame_) {
        if (!isKeyFrame && !pendingConfigPrepend_) {
            return false;
        }
        sawFirstKeyFrame_ = true;
    }

    const bool prepend = (pendingConfigPrepend_ || isKeyFrame) && !spsPps_.empty();
    const size_t totalSize = (prepend ? spsPps_.size() : 0) + size;

    IMFSample* inSample = nullptr;
    IMFMediaBuffer* inBuf = nullptr;
    if (FAILED(MFCreateSample(&inSample)) ||
        FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(totalSize), &inBuf))) {
        safeRelease(inBuf);
        safeRelease(inSample);
        return false;
    }

    BYTE* dst = nullptr;
    if (SUCCEEDED(inBuf->Lock(&dst, NULL, NULL)) && dst) {
        size_t off = 0;
        if (prepend) {
            std::memcpy(dst, spsPps_.data(), spsPps_.size());
            off = spsPps_.size();
        }
        std::memcpy(dst + off, data, size);
        inBuf->Unlock();
        inBuf->SetCurrentLength(static_cast<DWORD>(totalSize));
    }
    inSample->AddBuffer(inBuf);
    safeRelease(inBuf);

    inSample->SetSampleTime(static_cast<LONGLONG>(ptsUs) * 10LL);
    if (isKeyFrame) {
        inSample->SetUINT32(MFSampleExtension_CleanPoint, 1);
    }

    bool decodedAny = false;
    HRESULT hr = mft_->ProcessInput(0, inSample, 0);
    if (hr == MF_E_NOTACCEPTING) {
        drainOutputs(decodedAny);
        hr = mft_->ProcessInput(0, inSample, 0);
    }
    safeRelease(inSample);

    if (FAILED(hr)) {
        return decodedAny;
    }

    pendingConfigPrepend_ = false;
    drainOutputs(decodedAny);
    return decodedAny;
}

bool H264Decoder::drainOutputs(bool& decodedAny) {
    for (int iter = 0; iter < 16; ++iter) {
        MFT_OUTPUT_DATA_BUFFER outData;
        ZeroMemory(&outData, sizeof(outData));
        outData.dwStreamID = 0;
        outData.pSample = mftProvidesSamples_ ? nullptr : reusableOutSample_;

        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &outData, &status);
        if (outData.pEvents) {
            outData.pEvents->Release();
            outData.pEvents = nullptr;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!configureOutputNV12()) return false;
            continue;
        }
        if (FAILED(hr)) {
            break;
        }

        IMFSample* outSample = outData.pSample;
        if (outSample) {
            IMFMediaBuffer* buf = nullptr;
            if (SUCCEEDED(outSample->ConvertToContiguousBuffer(&buf)) && buf) {
                const uint8_t* srcPtr = nullptr;
                DWORD srcStride = defaultStride_;
                DWORD maxLen = 0, curLen = 0;
                bool locked2D = false;

                IMF2DBuffer* buf2D = nullptr;
                if (SUCCEEDED(buf->QueryInterface(IID_IMF2DBuffer, reinterpret_cast<void**>(&buf2D))) && buf2D) {
                    BYTE* scan0 = nullptr;
                    LONG pitch = 0;
                    if (SUCCEEDED(buf2D->Lock2D(&scan0, &pitch)) && scan0 && pitch > 0) {
                        srcPtr = scan0;
                        srcStride = static_cast<DWORD>(pitch);
                        locked2D = true;
                    } else {
                        safeRelease(buf2D);
                    }
                }
                if (!locked2D) {
                    BYTE* raw = nullptr;
                    if (SUCCEEDED(buf->Lock(&raw, &maxLen, &curLen)) && raw) {
                        srcPtr = raw;
                    }
                }

                if (srcPtr && cropW_ >= 16 && cropH_ >= 16 && codedH_ >= cropH_) {
                    // Copy clean cropped NV12 frame into contiguous output buffer (stride == aligned cropW)
                    const uint32_t outW = cropW_ & ~1u;
                    const uint32_t outH = cropH_ & ~1u;
                    const uint32_t uvOffY = codedH_;
                    const uint32_t cropXEven = cropX_ & ~1u;
                    const uint32_t cropYEven = cropY_ & ~1u;

                    workFrame_.width = outW;
                    workFrame_.height = outH;
                    workFrame_.stride = outW;
                    workFrame_.surfaceH = outH;
                    workFrame_.data.resize(static_cast<size_t>(outW) * outH * 3 / 2);

                    // Y plane
                    uint8_t* dstY = workFrame_.data.data();
                    for (uint32_t row = 0; row < outH; ++row) {
                        std::memcpy(dstY + static_cast<size_t>(row) * outW,
                                    srcPtr + static_cast<size_t>(cropYEven + row) * srcStride + cropXEven,
                                    outW);
                    }
                    // UV interleaved plane
                    uint8_t* dstUV = dstY + static_cast<size_t>(outW) * outH;
                    const uint8_t* srcUVBase = srcPtr + static_cast<size_t>(uvOffY) * srcStride;
                    for (uint32_t row = 0; row < outH / 2; ++row) {
                        std::memcpy(dstUV + static_cast<size_t>(row) * outW,
                                    srcUVBase + static_cast<size_t>(cropYEven / 2 + row) * srcStride + cropXEven,
                                    outW);
                    }

                    decodedAny = true;
                    if (frameCb_) {
                        frameCb_(frameCbCtx_, workFrame_);
                    }
                }

                if (locked2D && buf2D) {
                    buf2D->Unlock2D();
                    safeRelease(buf2D);
                } else if (srcPtr) {
                    buf->Unlock();
                }
                safeRelease(buf);
            }
            if (mftProvidesSamples_) {
                outSample->Release();
            }
        }
    }
    return true;
}
