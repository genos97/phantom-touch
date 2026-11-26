#include "audio/AudioPlayer.hpp"
#include "engine/ThreadScheduling.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mferror.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Official COM & Media Foundation GUIDs defined inline so linking stays independent of SDK variants.
const GUID kCLSID_MMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E }
};
const GUID kIID_IMMDeviceEnumerator = {
    0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 }
};
const GUID kIID_IAudioClient = {
    0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 }
};
const GUID kIID_IAudioRenderClient = {
    0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 }
};
const GUID kCLSID_CMSAACDecMFT = {
    0x32D186A7, 0x218F, 0x4C75, { 0x88, 0x76, 0xDD, 0x77, 0x27, 0x3A, 0x89, 0x99 }
};
const GUID kSubTypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};
const GUID kSubTypePcm = {
    0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};
const GUID kMFMediaType_Audio_Local = {
    0x73647561, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};
const GUID kMFAudioFormat_AAC_Local = {
    0x00001610, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};
const GUID kMFAudioFormat_Float_Local = {
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};
const GUID kMFAudioFormat_PCM_Local = {
    0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};
const GUID kMF_MT_USER_DATA_Local = {
    0xB6BC765F, 0x4C3B, 0x40A4, { 0xBD, 0x51, 0x25, 0x35, 0xB6, 0x6F, 0xE0, 0x9D }
};
const GUID kMF_MT_AAC_PAYLOAD_TYPE_Local = {
    0xBFBABE79, 0x7434, 0x4D1C, { 0x94, 0xF0, 0x72, 0xA3, 0xB9, 0xE1, 0x71, 0x88 }
};
const GUID kMF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION_Local = {
    0x7632F0E6, 0x9538, 0x4D61, { 0xAC, 0xDA, 0xEA, 0x29, 0xC8, 0xC1, 0x44, 0x56 }
};

template <typename T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

class BitReader {
public:
    BitReader(const uint8_t* data, size_t bytes)
        : data_(data), totalBits_(bytes * 8u), bitPos_(0) {}

    bool readBits(size_t count, uint32_t& out) {
        if (bitPos_ + count > totalBits_ || count > 32u) return false;
        uint32_t val = 0;
        for (size_t i = 0; i < count; ++i) {
            size_t b = bitPos_++;
            uint8_t bit = (data_[b >> 3] >> (7u - (b & 7u))) & 1u;
            val = (val << 1u) | bit;
        }
        out = val;
        return true;
    }

private:
    const uint8_t* data_;
    size_t totalBits_;
    size_t bitPos_;
};

class MfAacDecoder {
public:
    MfAacDecoder() {}
    ~MfAacDecoder() { shutdown(); }

    bool isInitialized() const { return mft_ != nullptr; }

    bool init(const uint8_t* asc, size_t ascLen, std::string& outError) {
        shutdown();

        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            comInit_ = true;
        } else if (hr != RPC_E_CHANGED_MODE) {
            outError = "COM initialization failed for MF AAC decoder.";
            return false;
        }

        hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) {
            outError = "Windows Media Foundation MFStartup failed for AAC.";
            shutdown();
            return false;
        }
        mfStarted_ = true;

        hr = CoCreateInstance(kCLSID_CMSAACDecMFT, NULL, CLSCTX_INPROC_SERVER,
                              IID_IMFTransform, reinterpret_cast<void**>(&mft_));
        if (FAILED(hr) || !mft_) {
            outError = "Windows built-in MF AAC decoder (CMSAACDecMFT) unavailable.";
            shutdown();
            return false;
        }

        uint32_t sr = 48000;
        uint32_t ch = 2;
        if (asc && ascLen >= 2) {
            AudioPlayer::parseAacAudioSpecificConfig(asc, ascLen, sr, ch);
        }
        inSampleRate_ = sr;
        inChannels_ = ch;

        std::vector<uint8_t> userData = AudioPlayer::buildMfAacUserData(asc, ascLen);

        IMFMediaType* inType = nullptr;
        hr = MFCreateMediaType(&inType);
        if (FAILED(hr) || !inType) {
            outError = "Failed creating AAC input media type.";
            shutdown();
            return false;
        }
        inType->SetGUID(MF_MT_MAJOR_TYPE, kMFMediaType_Audio_Local);
        inType->SetGUID(MF_MT_SUBTYPE, kMFAudioFormat_AAC_Local);
        inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sr);
        inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
        inType->SetUINT32(kMF_MT_AAC_PAYLOAD_TYPE_Local, 0);
        inType->SetUINT32(kMF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION_Local, 0xFE);
        inType->SetBlob(kMF_MT_USER_DATA_Local, userData.data(), static_cast<UINT32>(userData.size()));

        hr = mft_->SetInputType(0, inType, 0);
        safeRelease(inType);
        if (FAILED(hr)) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "CMSAACDecMFT SetInputType failed (0x%08lX).",
                          static_cast<unsigned long>(hr));
            outError = msg;
            shutdown();
            return false;
        }

        if (!configureOutputFormat()) {
            outError = "CMSAACDecMFT did not expose a usable PCM output type.";
            shutdown();
            return false;
        }

        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    void shutdown() {
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
        if (comInit_) {
            CoUninitialize();
            comInit_ = false;
        }
        mftProvidesSamples_ = false;
        outSampleRate_ = 48000;
        outChannels_ = 2;
        outBitsPerSample_ = 32;
        outIsFloat_ = true;
    }

    bool decodePacket(const uint8_t* data, size_t size, uint64_t ptsUs,
                      std::vector<float>& outStereo48k) {
        outStereo48k.clear();
        if (!mft_ || !data || size == 0) return false;

        IMFSample* inSample = nullptr;
        IMFMediaBuffer* inBuf = nullptr;
        if (FAILED(MFCreateSample(&inSample)) ||
            FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &inBuf))) {
            safeRelease(inBuf);
            safeRelease(inSample);
            return false;
        }

        BYTE* dst = nullptr;
        if (SUCCEEDED(inBuf->Lock(&dst, NULL, NULL)) && dst) {
            std::memcpy(dst, data, size);
            inBuf->Unlock();
            inBuf->SetCurrentLength(static_cast<DWORD>(size));
        } else {
            safeRelease(inBuf);
            safeRelease(inSample);
            return false;
        }
        inSample->AddBuffer(inBuf);
        safeRelease(inBuf);
        inSample->SetSampleTime(static_cast<LONGLONG>(ptsUs) * 10LL);

        HRESULT hr = mft_->ProcessInput(0, inSample, 0);
        if (hr == MF_E_NOTACCEPTING) {
            drainOutputs(outStereo48k);
            hr = mft_->ProcessInput(0, inSample, 0);
        }
        safeRelease(inSample);
        if (FAILED(hr)) {
            return false;
        }
        drainOutputs(outStereo48k);
        return !outStereo48k.empty();
    }

private:
    bool configureOutputFormat() {
        if (!mft_) return false;
        // Pass 0 prefers IEEE Float output; Pass 1 falls back to 16-bit integer PCM.
        for (int pass = 0; pass < 2; ++pass) {
            const GUID targetSub = (pass == 0) ? kMFAudioFormat_Float_Local : kMFAudioFormat_PCM_Local;
            for (DWORD idx = 0; idx < 64; ++idx) {
                IMFMediaType* outType = nullptr;
                HRESULT hr = mft_->GetOutputAvailableType(0, idx, &outType);
                if (hr == MF_E_NO_MORE_TYPES || FAILED(hr) || !outType) break;

                GUID subtype = GUID_NULL;
                UINT32 ch = 0, sr = 0, bps = 0;
                outType->GetGUID(MF_MT_SUBTYPE, &subtype);
                outType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                outType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
                outType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);

                bool match = (subtype == targetSub) &&
                             (pass == 0 ? (bps == 32) : (bps == 16)) &&
                             (ch >= 1 && ch <= 8);
                if (match) {
                    hr = mft_->SetOutputType(0, outType, 0);
                    safeRelease(outType);
                    if (SUCCEEDED(hr)) {
                        updateOutputMetadata();
                        return true;
                    }
                } else {
                    safeRelease(outType);
                }
            }
        }
        return false;
    }

    void updateOutputMetadata() {
        IMFMediaType* cur = nullptr;
        if (SUCCEEDED(mft_->GetOutputCurrentType(0, &cur)) && cur) {
            GUID sub = GUID_NULL;
            UINT32 sr = 48000, ch = 2, bps = 32;
            cur->GetGUID(MF_MT_SUBTYPE, &sub);
            cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
            cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
            cur->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);
            outSampleRate_ = (sr >= 8000 && sr <= 192000) ? sr : 48000;
            outChannels_ = (ch >= 1 && ch <= 8) ? ch : 2;
            outBitsPerSample_ = (bps == 16 || bps == 32) ? bps : 32;
            outIsFloat_ = (sub == kMFAudioFormat_Float_Local);
            safeRelease(cur);
        }

        MFT_OUTPUT_STREAM_INFO info;
        ZeroMemory(&info, sizeof(info));
        if (SUCCEEDED(mft_->GetOutputStreamInfo(0, &info))) {
            mftProvidesSamples_ = ((info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                                    MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0);
            safeRelease(reusableOutSample_);
            if (!mftProvidesSamples_) {
                DWORD bufSize = info.cbSize;
                if (bufSize < 32768u) bufSize = 32768u;
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

    void drainOutputs(std::vector<float>& outStereo48k) {
        for (int iter = 0; iter < 16; ++iter) {
            if (!mftProvidesSamples_ && reusableOutSample_) {
                IMFMediaBuffer* b = nullptr;
                if (SUCCEEDED(reusableOutSample_->GetBufferByIndex(0, &b)) && b) {
                    b->SetCurrentLength(0);
                    b->Release();
                }
            }
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
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!configureOutputFormat()) break;
                continue;
            }
            if (FAILED(hr)) break;

            IMFSample* outSample = outData.pSample;
            if (outSample) {
                IMFMediaBuffer* buf = nullptr;
                if (SUCCEEDED(outSample->ConvertToContiguousBuffer(&buf)) && buf) {
                    BYTE* ptr = nullptr;
                    DWORD maxLen = 0, curLen = 0;
                    if (SUCCEEDED(buf->Lock(&ptr, &maxLen, &curLen)) && ptr && curLen > 0) {
                        appendConvertedStereo48k(ptr, curLen, outStereo48k);
                        buf->Unlock();
                    }
                    safeRelease(buf);
                }
                if (mftProvidesSamples_) {
                    outSample->Release();
                }
            }
        }
    }

    void appendConvertedStereo48k(const uint8_t* raw, DWORD bytes, std::vector<float>& outStereo48k) {
        const uint32_t bytesPerSample = outBitsPerSample_ / 8u;
        const uint32_t frameStrideBytes = outChannels_ * bytesPerSample;
        if (frameStrideBytes == 0) return;
        const uint32_t inFrames = bytes / frameStrideBytes;
        if (inFrames == 0) return;

        nativeScratch_.resize(static_cast<size_t>(inFrames) * 2u);
        if (outIsFloat_ && outBitsPerSample_ == 32u) {
            const float* src = reinterpret_cast<const float*>(raw);
            for (uint32_t i = 0; i < inFrames; ++i) {
                const float* fr = src + static_cast<size_t>(i) * outChannels_;
                float l = fr[0];
                float r = (outChannels_ >= 2u) ? fr[1] : fr[0];
                nativeScratch_[static_cast<size_t>(i) * 2u] = l;
                nativeScratch_[static_cast<size_t>(i) * 2u + 1u] = r;
            }
        } else if (!outIsFloat_ && outBitsPerSample_ == 16u) {
            const int16_t* src = reinterpret_cast<const int16_t*>(raw);
            const float scale = 1.0f / 32768.0f;
            for (uint32_t i = 0; i < inFrames; ++i) {
                const int16_t* fr = src + static_cast<size_t>(i) * outChannels_;
                float l = static_cast<float>(fr[0]) * scale;
                float r = static_cast<float>((outChannels_ >= 2u) ? fr[1] : fr[0]) * scale;
                nativeScratch_[static_cast<size_t>(i) * 2u] = l;
                nativeScratch_[static_cast<size_t>(i) * 2u + 1u] = r;
            }
        } else {
            return;
        }

        if (outSampleRate_ == 48000u) {
            outStereo48k.insert(outStereo48k.end(), nativeScratch_.begin(), nativeScratch_.end());
        } else {
            const double ratio = static_cast<double>(outSampleRate_) / 48000.0;
            const uint32_t outFrames = static_cast<uint32_t>(
                static_cast<double>(inFrames) * 48000.0 / static_cast<double>(outSampleRate_));
            const size_t base = outStereo48k.size();
            outStereo48k.resize(base + static_cast<size_t>(outFrames) * 2u);
            for (uint32_t o = 0; o < outFrames; ++o) {
                double pos = static_cast<double>(o) * ratio;
                uint32_t idx0 = static_cast<uint32_t>(pos);
                if (idx0 >= inFrames) idx0 = inFrames - 1u;
                uint32_t idx1 = (idx0 + 1u < inFrames) ? (idx0 + 1u) : idx0;
                float f = static_cast<float>(pos - static_cast<double>(idx0));
                float l0 = nativeScratch_[static_cast<size_t>(idx0) * 2u];
                float r0 = nativeScratch_[static_cast<size_t>(idx0) * 2u + 1u];
                float l1 = nativeScratch_[static_cast<size_t>(idx1) * 2u];
                float r1 = nativeScratch_[static_cast<size_t>(idx1) * 2u + 1u];
                outStereo48k[base + static_cast<size_t>(o) * 2u] = l0 + (l1 - l0) * f;
                outStereo48k[base + static_cast<size_t>(o) * 2u + 1u] = r0 + (r1 - r0) * f;
            }
        }
    }

    IMFTransform* mft_ = nullptr;
    IMFSample* reusableOutSample_ = nullptr;
    bool mfStarted_ = false;
    bool comInit_ = false;
    bool mftProvidesSamples_ = false;
    uint32_t inSampleRate_ = 48000;
    uint32_t inChannels_ = 2;
    uint32_t outSampleRate_ = 48000;
    uint32_t outChannels_ = 2;
    uint32_t outBitsPerSample_ = 32;
    bool outIsFloat_ = true;
    std::vector<float> nativeScratch_;
};

} // namespace

// ---------------------------------------------------------------------------
// AudioDriftRegulator
// ---------------------------------------------------------------------------

AudioDriftRegulator::AudioDriftRegulator() {
    configure(48000u, 25u);
}

void AudioDriftRegulator::configure(uint32_t sampleRate, uint32_t targetMs) {
    sampleRate_ = (sampleRate >= 8000u && sampleRate <= 192000u) ? sampleRate : 48000u;
    if (targetMs < 15u) targetMs = 15u;
    if (targetMs > 100u) targetMs = 100u;
    targetFrames_ = (sampleRate_ * targetMs) / 1000u;
    if (targetFrames_ < 128u) targetFrames_ = 128u;
    capFrames_ = sampleRate_;
    ring_.assign(static_cast<size_t>(capFrames_) * 2u, 0.0f);
    reset();
}

void AudioDriftRegulator::reset() {
    writePos_ = 0;
    readPos_ = 0;
    buffered_ = 0;
    primed_ = false;
    avgFrames_ = static_cast<double>(targetFrames_);
    driftRate_ = 1.0;
    fracPhase_ = 0.0;
    driftActive_ = false;
    nextExpectedPtsUs_ = 0;
    underflowFrames_ = 0;
    droppedFrames_ = 0;
    fadeInRemain_ = 0;
    prevL_ = 0.0f;
    prevR_ = 0.0f;
}

void AudioDriftRegulator::writeRing(const float* src, uint32_t frames) {
    uint32_t first = capFrames_ - writePos_;
    if (first > frames) first = frames;
    if (first > 0) {
        std::memcpy(&ring_[static_cast<size_t>(writePos_) * 2u], src,
                    static_cast<size_t>(first) * 2u * sizeof(float));
    }
    uint32_t second = frames - first;
    if (second > 0) {
        std::memcpy(&ring_[0], src + static_cast<size_t>(first) * 2u,
                    static_cast<size_t>(second) * 2u * sizeof(float));
    }
    writePos_ = (writePos_ + frames) % capFrames_;
    buffered_ += frames;
}

void AudioDriftRegulator::writeRingSilence(uint32_t frames) {
    if (frames == 0) return;
    if (frames > capFrames_ - buffered_) frames = capFrames_ - buffered_;
    uint32_t first = capFrames_ - writePos_;
    if (first > frames) first = frames;
    if (first > 0) {
        std::memset(&ring_[static_cast<size_t>(writePos_) * 2u], 0,
                    static_cast<size_t>(first) * 2u * sizeof(float));
    }
    uint32_t second = frames - first;
    if (second > 0) {
        std::memset(&ring_[0], 0, static_cast<size_t>(second) * 2u * sizeof(float));
    }
    writePos_ = (writePos_ + frames) % capFrames_;
    buffered_ += frames;
}

void AudioDriftRegulator::discardOldest(uint32_t frames) {
    if (frames == 0) return;
    if (frames > buffered_) frames = buffered_;
    readPos_ = (readPos_ + frames) % capFrames_;
    buffered_ -= frames;
    droppedFrames_ += frames;
    avgFrames_ -= static_cast<double>(frames);
    if (avgFrames_ < 0.0) avgFrames_ = 0.0;
}

void AudioDriftRegulator::pushInterleavedFloat(const float* samples, uint32_t frames, uint64_t ptsUs) {
    if (!samples || frames == 0 || capFrames_ == 0) return;

    if (frames > capFrames_) {
        uint32_t trim = frames - capFrames_;
        samples += static_cast<size_t>(trim) * 2u;
        droppedFrames_ += trim;
        frames = capFrames_;
    }

    if (nextExpectedPtsUs_ > 0 && ptsUs > nextExpectedPtsUs_ + 100000ULL) {
        if (buffered_ + frames < targetFrames_) {
            writeRingSilence(targetFrames_ - buffered_ - frames);
        }
        avgFrames_ = static_cast<double>(targetFrames_);
        driftActive_ = false;
        driftRate_ = 1.0;
        fracPhase_ = 0.0;
    }
    nextExpectedPtsUs_ = ptsUs + (static_cast<uint64_t>(frames) * 1000000ULL) / sampleRate_;

    if (buffered_ + frames > capFrames_) {
        discardOldest(buffered_ + frames - capFrames_);
    }

    writeRing(samples, frames);

    if (!primed_) {
        // Allow at least one full 1024-frame (~21.3ms) Android read block past targetFrames_
        // during initial pre-roll so 2x1024 startup bursts never drop frames before playback starts.
        const uint32_t minHeadroom = 1536u;
        const uint32_t head = (sampleRate_ / 32u > minHeadroom) ? (sampleRate_ / 32u) : minHeadroom;
        const uint32_t preCap = targetFrames_ + head;
        if (buffered_ > preCap) {
            discardOldest(buffered_ - preCap);
        }
    } else {
        const uint32_t margin = (sampleRate_ * 60u) / 1000u;
        const uint32_t halfTgt = targetFrames_ / 2u;
        const uint32_t maxCap = targetFrames_ + ((halfTgt > margin) ? halfTgt : margin);
        if (buffered_ > maxCap) {
            const uint32_t keep = targetFrames_ + sampleRate_ / 100u;
            if (buffered_ > keep) {
                discardOldest(buffered_ - keep);
                fadeInRemain_ = 16u;
            }
        }
    }
}

void AudioDriftRegulator::pushInterleavedS16(const int16_t* samples, uint32_t frames, uint64_t ptsUs) {
    if (!samples || frames == 0) return;
    const size_t n = static_cast<size_t>(frames) * 2u;
    s16Scratch_.resize(n);
    const float scale = 1.0f / 32768.0f;
    for (size_t i = 0; i < n; ++i) {
        s16Scratch_[i] = static_cast<float>(samples[i]) * scale;
    }
    pushInterleavedFloat(s16Scratch_.data(), frames, ptsUs);
}

bool AudioDriftRegulator::pullInterleavedFloat(float* out, uint32_t frames) {
    if (!out || frames == 0) return false;

    if (!primed_) {
        if (buffered_ < targetFrames_) {
            std::memset(out, 0, static_cast<size_t>(frames) * 2u * sizeof(float));
            return false;
        }
        primed_ = true;
        avgFrames_ = static_cast<double>(buffered_);
        fadeInRemain_ = 16u;
    }

    avgFrames_ = avgFrames_ * 0.96 + static_cast<double>(buffered_) * 0.04;
    const double err = avgFrames_ - static_cast<double>(targetFrames_);
    const double onThresh = static_cast<double>(sampleRate_) * 0.004;
    const double offThresh = static_cast<double>(sampleRate_) * 0.001;

    if (!driftActive_) {
        if (std::fabs(err) > onThresh) driftActive_ = true;
    } else if (std::fabs(err) < offThresh) {
        driftActive_ = false;
    }

    if (!driftActive_ || (err > 0.0 && buffered_ < targetFrames_)) {
        driftRate_ = 1.0;
    } else {
        double comp = err / (4.0 * static_cast<double>(sampleRate_));
        if (comp > 0.015) comp = 0.015;
        if (comp < -0.015) comp = -0.015;
        driftRate_ = 1.0 + comp;
    }

    uint32_t outWritten = 0;
    if (driftRate_ == 1.0) {
        fracPhase_ = 0.0;
        const uint32_t avail = (buffered_ < frames) ? buffered_ : frames;
        for (uint32_t i = 0; i < avail; ++i) {
            const size_t idx = static_cast<size_t>(readPos_) * 2u;
            float l = ring_[idx];
            float r = ring_[idx + 1u];
            if (fadeInRemain_ > 0) {
                const float g = 1.0f - static_cast<float>(fadeInRemain_) / 32.0f;
                l *= g;
                r *= g;
                --fadeInRemain_;
            }
            out[static_cast<size_t>(i) * 2u] = l;
            out[static_cast<size_t>(i) * 2u + 1u] = r;
            prevL_ = l;
            prevR_ = r;
            readPos_ = (readPos_ + 1u) % capFrames_;
            --buffered_;
        }
        outWritten = avail;
    } else {
        while (outWritten < frames && buffered_ > 0) {
            const size_t i0 = static_cast<size_t>(readPos_) * 2u;
            const float l0 = ring_[i0];
            const float r0 = ring_[i0 + 1u];
            float l1 = l0;
            float r1 = r0;
            if (buffered_ > 1u) {
                const size_t i1 = static_cast<size_t>((readPos_ + 1u) % capFrames_) * 2u;
                l1 = ring_[i1];
                r1 = ring_[i1 + 1u];
            }
            const float f = static_cast<float>(fracPhase_);
            float l = l0 + (l1 - l0) * f;
            float r = r0 + (r1 - r0) * f;
            if (fadeInRemain_ > 0) {
                const float g = 1.0f - static_cast<float>(fadeInRemain_) / 32.0f;
                l *= g;
                r *= g;
                --fadeInRemain_;
            }
            out[static_cast<size_t>(outWritten) * 2u] = l;
            out[static_cast<size_t>(outWritten) * 2u + 1u] = r;
            prevL_ = l;
            prevR_ = r;
            ++outWritten;

            fracPhase_ += driftRate_;
            uint32_t adv = static_cast<uint32_t>(fracPhase_);
            fracPhase_ -= static_cast<double>(adv);
            if (adv > buffered_) adv = buffered_;
            if (adv > 0) {
                readPos_ = (readPos_ + adv) % capFrames_;
                buffered_ -= adv;
            }
        }
    }

    if (outWritten < frames) {
        const uint32_t ramp = (outWritten < 32u) ? outWritten : 32u;
        for (uint32_t k = 0; k < ramp; ++k) {
            const uint32_t pos = outWritten - ramp + k;
            const float g = static_cast<float>(ramp - 1u - k) / static_cast<float>(ramp);
            out[static_cast<size_t>(pos) * 2u] *= g;
            out[static_cast<size_t>(pos) * 2u + 1u] *= g;
        }
        std::memset(out + static_cast<size_t>(outWritten) * 2u, 0,
                    static_cast<size_t>(frames - outWritten) * 2u * sizeof(float));
        underflowFrames_ += static_cast<uint64_t>(frames - outWritten);
        fadeInRemain_ = 32u;
    }

    return outWritten > 0;
}

// ---------------------------------------------------------------------------
// AudioPlayer::Impl
// ---------------------------------------------------------------------------

struct AudioPlayer::Impl {
    Impl() {
        InitializeCriticalSection(&regLock);
        stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        wasapiEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    }

    ~Impl() {
        if (stopEvent) {
            CloseHandle(stopEvent);
            stopEvent = NULL;
        }
        if (wasapiEvent) {
            CloseHandle(wasapiEvent);
            wasapiEvent = NULL;
        }
        DeleteCriticalSection(&regLock);
    }

    static DWORD WINAPI renderThreadProc(LPVOID param) {
        static_cast<Impl*>(param)->runRenderThread();
        return 0;
    }

    bool openDefaultWasapiDevice(IMMDeviceEnumerator* enumerator,
                                 IAudioClient*& outClient,
                                 IAudioRenderClient*& outRender,
                                 uint32_t& outBufFrames,
                                 uint32_t& outDevRate,
                                 uint32_t& outDevCh,
                                 uint32_t& outDevBits,
                                 bool& outDevFloat,
                                 std::string& outErr) {
        outClient = nullptr;
        outRender = nullptr;
        outBufFrames = 0;
        outDevRate = 48000;
        outDevCh = 2;
        outDevBits = 32;
        outDevFloat = true;

        IMMDevice* dev = nullptr;
        HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (FAILED(hr) || !dev) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev);
        }
        if (FAILED(hr) || !dev) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "No active Windows audio output endpoint (0x%08lX)",
                          static_cast<unsigned long>(hr));
            outErr = buf;
            return false;
        }

        hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, NULL, reinterpret_cast<void**>(&outClient));
        safeRelease(dev);
        if (FAILED(hr) || !outClient) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "IAudioClient activation failed (0x%08lX)",
                          static_cast<unsigned long>(hr));
            outErr = buf;
            return false;
        }

        WAVEFORMATEXTENSIBLE wfxe;
        ZeroMemory(&wfxe, sizeof(wfxe));
        wfxe.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfxe.Format.nChannels = 2;
        wfxe.Format.nSamplesPerSec = 48000;
        wfxe.Format.wBitsPerSample = 32;
        wfxe.Format.nBlockAlign = 8;
        wfxe.Format.nAvgBytesPerSec = 48000u * 8u;
        wfxe.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfxe.Samples.wValidBitsPerSample = 32;
        wfxe.dwChannelMask = 0x3u; // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
        wfxe.SubFormat = kSubTypeIeeeFloat;

        const DWORD primaryFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = outClient->Initialize(AUDCLNT_SHAREMODE_SHARED, primaryFlags, 0, 0,
                                   &wfxe.Format, NULL);
        if (FAILED(hr)) {
            WAVEFORMATEX* mixFmt = nullptr;
            HRESULT hrMix = outClient->GetMixFormat(&mixFmt);
            if (SUCCEEDED(hrMix) && mixFmt) {
                hr = outClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                           0, 0, mixFmt, NULL);
                if (SUCCEEDED(hr)) {
                    outDevRate = (mixFmt->nSamplesPerSec >= 8000 && mixFmt->nSamplesPerSec <= 192000)
                        ? mixFmt->nSamplesPerSec : 48000;
                    outDevCh = (mixFmt->nChannels >= 1 && mixFmt->nChannels <= 8) ? mixFmt->nChannels : 2;
                    outDevBits = mixFmt->wBitsPerSample;
                    if (mixFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                        outDevFloat = true;
                    } else if (mixFmt->wFormatTag == WAVE_FORMAT_PCM) {
                        outDevFloat = false;
                    } else if (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                               mixFmt->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
                        const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFmt);
                        outDevFloat = (ext->SubFormat == kSubTypeIeeeFloat) != 0;
                    } else {
                        outDevFloat = false;
                    }
                }
                CoTaskMemFree(mixFmt);
            }
        }
        if (FAILED(hr)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "WASAPI IAudioClient::Initialize failed (0x%08lX)",
                          static_cast<unsigned long>(hr));
            outErr = buf;
            safeRelease(outClient);
            return false;
        }

        hr = outClient->GetBufferSize(&outBufFrames);
        if (FAILED(hr) || outBufFrames == 0) {
            safeRelease(outClient);
            outErr = "WASAPI GetBufferSize failed.";
            return false;
        }
        hr = outClient->SetEventHandle(wasapiEvent);
        if (FAILED(hr)) {
            safeRelease(outClient);
            outErr = "WASAPI SetEventHandle failed.";
            return false;
        }
        hr = outClient->GetService(kIID_IAudioRenderClient, reinterpret_cast<void**>(&outRender));
        if (FAILED(hr) || !outRender) {
            safeRelease(outClient);
            outErr = "WASAPI GetService(IAudioRenderClient) failed.";
            return false;
        }
        return true;
    }

    void writeDeviceFrames(BYTE* dst, const float* src48k, uint32_t srcFrames,
                           uint32_t devFrames, uint32_t devCh, uint32_t devBits, bool devFloat) {
        if (srcFrames == 0 || devFrames == 0) return;
        const bool direct48k = (srcFrames == devFrames);
        const double ratio = direct48k ? 1.0 : (static_cast<double>(srcFrames) / static_cast<double>(devFrames));

        if (devFloat && devBits == 32u && devCh == 2u && direct48k) {
            std::memcpy(dst, src48k, static_cast<size_t>(devFrames) * 2u * sizeof(float));
            return;
        }

        if (devFloat && devBits == 32u) {
            float* outF = reinterpret_cast<float*>(dst);
            for (uint32_t i = 0; i < devFrames; ++i) {
                uint32_t sIdx = direct48k ? i : static_cast<uint32_t>(static_cast<double>(i) * ratio);
                if (sIdx >= srcFrames) sIdx = srcFrames - 1u;
                float l = src48k[static_cast<size_t>(sIdx) * 2u];
                float r = src48k[static_cast<size_t>(sIdx) * 2u + 1u];
                float* chPtr = outF + static_cast<size_t>(i) * devCh;
                for (uint32_t c = 0; c < devCh; ++c) {
                    if (c == 0) chPtr[c] = (devCh == 1u) ? (0.5f * (l + r)) : l;
                    else if (c == 1u) chPtr[c] = r;
                    else chPtr[c] = 0.0f;
                }
            }
        } else if (!devFloat && devBits == 16u) {
            int16_t* outI = reinterpret_cast<int16_t*>(dst);
            for (uint32_t i = 0; i < devFrames; ++i) {
                uint32_t sIdx = direct48k ? i : static_cast<uint32_t>(static_cast<double>(i) * ratio);
                if (sIdx >= srcFrames) sIdx = srcFrames - 1u;
                float l = src48k[static_cast<size_t>(sIdx) * 2u];
                float r = src48k[static_cast<size_t>(sIdx) * 2u + 1u];
                int16_t* chPtr = outI + static_cast<size_t>(i) * devCh;
                for (uint32_t c = 0; c < devCh; ++c) {
                    float v = (c == 0) ? ((devCh == 1u) ? (0.5f * (l + r)) : l) : ((c == 1u) ? r : 0.0f);
                    if (v > 1.0f) v = 1.0f;
                    if (v < -1.0f) v = -1.0f;
                    long iv = static_cast<long>(v * 32767.0f);
                    if (iv > 32767L) iv = 32767L;
                    if (iv < -32768L) iv = -32768L;
                    chPtr[c] = static_cast<int16_t>(iv);
                }
            }
        } else {
            std::memset(dst, 0, static_cast<size_t>(devFrames) * devCh * (devBits / 8u));
        }
    }

    void runRenderThread() {
        HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        const bool comInit = SUCCEEDED(hrCom);
        DWORD mmcssIdx = 0;
        HANDLE avrt = ThreadScheduling::enableMmcssAudio(mmcssIdx);

        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hrEnum = CoCreateInstance(kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                          kIID_IMMDeviceEnumerator,
                                          reinterpret_cast<void**>(&enumerator));
        if (FAILED(hrEnum) || !enumerator) {
            InterlockedExchange(&wasapiActive, 0);
            InterlockedExchange(&running, 0);
            ThreadScheduling::disableMmcss(avrt);
            if (comInit) CoUninitialize();
            return;
        }

        std::vector<float> pullBuf;
        HANDLE waitHandles[2] = { stopEvent, wasapiEvent };

        while (!stopRequested) {
            IAudioClient* client = nullptr;
            IAudioRenderClient* render = nullptr;
            uint32_t bufFrames = 0, devRate = 48000, devCh = 2, devBits = 32;
            bool devFloat = true;
            std::string devErr;

            if (!openDefaultWasapiDevice(enumerator, client, render, bufFrames,
                                         devRate, devCh, devBits, devFloat, devErr)) {
                InterlockedExchange(&wasapiActive, 0);
                // Exit cleanly if no audio output device exists on a headless host.
                break;
            }

            BYTE* initDst = nullptr;
            if (SUCCEEDED(render->GetBuffer(bufFrames, &initDst))) {
                render->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            }

            ResetEvent(wasapiEvent);
            if (FAILED(client->Start())) {
                safeRelease(render);
                safeRelease(client);
                InterlockedExchange(&wasapiActive, 0);
                break;
            }
            InterlockedExchange(&wasapiActive, 1);

            bool deviceLost = false;
            while (!stopRequested) {
                DWORD w = WaitForMultipleObjects(2, waitHandles, FALSE, 200);
                if (w == WAIT_OBJECT_0 || stopRequested) {
                    break;
                }

                UINT32 padding = 0;
                HRESULT hr = client->GetCurrentPadding(&padding);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING || FAILED(hr)) {
                    deviceLost = true;
                    break;
                }
                if (padding >= bufFrames) continue;
                const uint32_t availDev = bufFrames - padding;
                const uint32_t pull48k = (devRate == 48000u)
                    ? availDev
                    : static_cast<uint32_t>((static_cast<uint64_t>(availDev) * 48000ULL + devRate - 1u) / devRate);
                if (pull48k == 0) continue;

                pullBuf.resize(static_cast<size_t>(pull48k) * 2u);
                bool hasAudio = false;
                EnterCriticalSection(&regLock);
                hasAudio = regulator.pullInterleavedFloat(pullBuf.data(), pull48k);
                framesPlayed += pull48k;
                LeaveCriticalSection(&regLock);

                const bool silent = (!hasAudio || muted != 0);
                BYTE* dst = nullptr;
                hr = render->GetBuffer(availDev, &dst);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
                    deviceLost = true;
                    break;
                }
                if (SUCCEEDED(hr) && dst) {
                    if (silent) {
                        render->ReleaseBuffer(availDev, AUDCLNT_BUFFERFLAGS_SILENT);
                    } else {
                        writeDeviceFrames(dst, pullBuf.data(), pull48k, availDev, devCh, devBits, devFloat);
                        render->ReleaseBuffer(availDev, 0);
                    }
                }
            }

            client->Stop();
            safeRelease(render);
            safeRelease(client);
            InterlockedExchange(&wasapiActive, 0);

            if (!deviceLost || stopRequested) {
                break;
            }
            if (WaitForSingleObject(stopEvent, 150) == WAIT_OBJECT_0) {
                break;
            }
        }

        safeRelease(enumerator);
        InterlockedExchange(&wasapiActive, 0);
        InterlockedExchange(&running, 0);
        ThreadScheduling::disableMmcss(avrt);
        if (comInit) CoUninitialize();
    }

    CRITICAL_SECTION regLock;
    AudioDriftRegulator regulator;
    MfAacDecoder aacDecoder;
    std::vector<float> aacDecodeBuf;

    HANDLE renderThread = NULL;
    HANDLE stopEvent = NULL;
    HANDLE wasapiEvent = NULL;

    volatile LONG running = 0;
    volatile LONG startAttempted = 0;
    volatile LONG stopRequested = 0;
    volatile LONG muted = 0;
    volatile LONG wasapiActive = 0;

    AudioCodecId activeCodec = AUDIO_CODEC_NONE;
    uint64_t packetsReceived = 0;
    uint64_t framesPushed = 0;
    uint64_t framesPlayed = 0;
};

// ---------------------------------------------------------------------------
// AudioPlayer public API
// ---------------------------------------------------------------------------

AudioPlayer::AudioPlayer()
    : impl_(new Impl()) {}

AudioPlayer::~AudioPlayer() {
    stop();
    delete impl_;
    impl_ = nullptr;
}

void AudioPlayer::prepareForSession() {
    InterlockedExchange(&impl_->stopRequested, 0);
    InterlockedExchange(&impl_->startAttempted, 0);
    if (impl_->stopEvent) ResetEvent(impl_->stopEvent);
}

bool AudioPlayer::start(AudioCodecId codec, int targetBufferMs, std::string& outError) {
    outError.clear();
    if (InterlockedCompareExchange(&impl_->startAttempted, 1, 0) != 0) {
        return impl_->running != 0;
    }
    if (impl_->running) return true;
    if (targetBufferMs < 15) targetBufferMs = 15;
    if (targetBufferMs > 100) targetBufferMs = 100;

    InterlockedExchange(&impl_->stopRequested, 0);
    if (impl_->stopEvent) ResetEvent(impl_->stopEvent);

    EnterCriticalSection(&impl_->regLock);
    impl_->regulator.configure(48000u, static_cast<uint32_t>(targetBufferMs));
    impl_->activeCodec = codec;
    impl_->packetsReceived = 0;
    impl_->framesPushed = 0;
    impl_->framesPlayed = 0;
    LeaveCriticalSection(&impl_->regLock);

    InterlockedExchange(&impl_->running, 1);
    impl_->renderThread = CreateThread(NULL, 0, Impl::renderThreadProc, impl_, 0, NULL);
    if (!impl_->renderThread) {
        InterlockedExchange(&impl_->running, 0);
        outError = "Failed creating WASAPI audio render thread.";
        return false;
    }
    return true;
}

void AudioPlayer::requestStop() {
    InterlockedExchange(&impl_->stopRequested, 1);
    if (impl_->stopEvent) SetEvent(impl_->stopEvent);
}

void AudioPlayer::stop() {
    requestStop();
    if (impl_->renderThread) {
        WaitForSingleObject(impl_->renderThread, 2000);
        CloseHandle(impl_->renderThread);
        impl_->renderThread = NULL;
    }
    InterlockedExchange(&impl_->running, 0);
    InterlockedExchange(&impl_->wasapiActive, 0);
    impl_->aacDecoder.shutdown();
}

bool AudioPlayer::isRunning() const {
    return impl_->running != 0;
}

bool AudioPlayer::isStopRequested() const {
    return impl_->stopRequested != 0;
}

void AudioPlayer::setMuted(bool muted) {
    InterlockedExchange(&impl_->muted, muted ? 1 : 0);
}

bool AudioPlayer::isMuted() const {
    return impl_->muted != 0;
}

void AudioPlayer::onAudioPacket(AudioCodecId codec, const uint8_t* data, size_t size,
                                uint64_t ptsUs, bool isConfig) {
    if (!data || size == 0 || impl_->stopRequested) return;

    EnterCriticalSection(&impl_->regLock);
    ++impl_->packetsReceived;
    impl_->activeCodec = codec;
    LeaveCriticalSection(&impl_->regLock);

    if (codec == AUDIO_CODEC_RAW) {
        if (isConfig || size < 4) return;
        const uint32_t frames = static_cast<uint32_t>(size / 4u);
        const int16_t* pcm = reinterpret_cast<const int16_t*>(data);
        EnterCriticalSection(&impl_->regLock);
        impl_->regulator.pushInterleavedS16(pcm, frames, ptsUs);
        impl_->framesPushed += frames;
        LeaveCriticalSection(&impl_->regLock);
    } else if (codec == AUDIO_CODEC_AAC) {
        if (isConfig) {
            std::string err;
            if (!impl_->aacDecoder.init(data, size, err)) {
                std::printf("[Audio] MF AAC init failed: %s\n", err.c_str());
            }
            return;
        }
        if (!impl_->aacDecoder.isInitialized()) {
            const uint8_t defaultAsc[2] = { 0x11, 0x90 };
            std::string err;
            if (!impl_->aacDecoder.init(defaultAsc, sizeof(defaultAsc), err)) {
                std::printf("[Audio] MF AAC fallback init failed: %s\n", err.c_str());
                return;
            }
        }
        if (impl_->aacDecoder.decodePacket(data, size, ptsUs, impl_->aacDecodeBuf) &&
            !impl_->aacDecodeBuf.empty()) {
            const uint32_t frames = static_cast<uint32_t>(impl_->aacDecodeBuf.size() / 2u);
            EnterCriticalSection(&impl_->regLock);
            impl_->regulator.pushInterleavedFloat(impl_->aacDecodeBuf.data(), frames, ptsUs);
            impl_->framesPushed += frames;
            LeaveCriticalSection(&impl_->regLock);
        }
    }
}

AudioStats AudioPlayer::getStats() const {
    AudioStats s;
    EnterCriticalSection(&impl_->regLock);
    s.packetsReceived = impl_->packetsReceived;
    s.framesPushed = impl_->framesPushed;
    s.framesPlayed = impl_->framesPlayed;
    s.underflowFrames = impl_->regulator.underflowFrames();
    s.droppedFrames = impl_->regulator.droppedFrames();
    s.bufferedMs = static_cast<float>(impl_->regulator.bufferedFrames()) * 1000.0f / 48000.0f;
    s.driftRate = impl_->regulator.currentRate();
    s.codec = impl_->activeCodec;
    LeaveCriticalSection(&impl_->regLock);
    s.wasapiActive = (impl_->wasapiActive != 0);
    return s;
}

std::vector<uint8_t> AudioPlayer::buildMfAacUserData(const uint8_t* asc, size_t ascLen) {
    const uint8_t defaultAsc[2] = { 0x11, 0x90 };
    if (!asc || ascLen == 0) {
        asc = defaultAsc;
        ascLen = sizeof(defaultAsc);
    }
    // 12-byte HEAACWAVEINFO fields before AudioSpecificConfig:
    //   wPayloadType=0 (Raw), wAudioProfileLevelIndication=0x00FE,
    //   wStructType=0, wReserved1=0, dwReserved2=0 (all little-endian).
    std::vector<uint8_t> out(12u + ascLen, 0u);
    out[2] = 0xFEu;
    out[3] = 0x00u;
    std::memcpy(out.data() + 12u, asc, ascLen);
    return out;
}

bool AudioPlayer::parseAacAudioSpecificConfig(const uint8_t* asc, size_t ascLen,
                                              uint32_t& outSampleRate, uint32_t& outChannels) {
    if (!asc || ascLen < 2) return false;

    static const uint32_t kRates[13] = {
        96000u, 88200u, 64000u, 48000u, 44100u, 32000u,
        24000u, 22050u, 16000u, 12000u, 11025u, 8000u, 7350u
    };

    BitReader br(asc, ascLen);
    uint32_t aot = 0;
    if (!br.readBits(5u, aot)) return false;
    if (aot == 31u) {
        uint32_t ext = 0;
        if (!br.readBits(6u, ext)) return false;
        aot = 32u + ext;
    }

    uint32_t freqIdx = 0;
    if (!br.readBits(4u, freqIdx)) return false;
    uint32_t sr = 48000u;
    if (freqIdx == 15u) {
        if (!br.readBits(24u, sr)) return false;
    } else if (freqIdx < 13u) {
        sr = kRates[freqIdx];
    } else {
        return false;
    }

    uint32_t chanCfg = 0;
    if (!br.readBits(4u, chanCfg)) return false;
    uint32_t ch = 2u;
    if (chanCfg >= 1u && chanCfg <= 6u) {
        ch = chanCfg;
    } else if (chanCfg == 7u) {
        ch = 8u;
    } else if (chanCfg != 0u) {
        return false;
    }

    (void)aot;
    outSampleRate = sr;
    outChannels = ch;
    return true;
}

bool AudioPlayer::probeMfAacDecoderInit(const uint8_t* asc, size_t ascLen, std::string& outError) {
    MfAacDecoder dec;
    bool ok = dec.init(asc, ascLen, outError);
    dec.shutdown();
    return ok;
}
