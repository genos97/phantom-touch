#include "video/Recorder.hpp"

#include "bridge/SpsParser.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace {

// Inline GUIDs so MinGW links cleanly across SDK revisions.
const GUID kLocalMF_SINK_WRITER_DISABLE_THROTTLING = {
    0x08b845d8, 0x2b74, 0x4afe, { 0x9d, 0x53, 0xbe, 0x16, 0xd2, 0xd5, 0xae, 0x4f }
};
const GUID kLocalMF_READWRITE_ENABLE_HARDWARE_TRANSFORMS = {
    0xa634a91c, 0x822b, 0x41b9, { 0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0 }
};

template <typename T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), NULL, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &out[0], len);
    return out;
}

void ensureParentDir(const std::string& filePathUtf8) {
    size_t pos = filePathUtf8.find_last_of("\\/");
    if (pos == std::string::npos) return;
    std::wstring wdir = utf8ToWide(filePathUtf8.substr(0, pos));
    if (!wdir.empty()) {
        CreateDirectoryW(wdir.c_str(), NULL);
    }
}

std::string replaceWithH264Ext(const std::string& pathUtf8) {
    size_t slash = pathUtf8.find_last_of("\\/");
    size_t dot = pathUtf8.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return pathUtf8.substr(0, dot) + ".h264";
    }
    return pathUtf8 + ".h264";
}

bool containsSpsNal(const uint8_t* data, size_t size) {
    if (!data || size < 5) return false;
    for (size_t i = 0; i + 4 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0) {
            size_t scLen = 0;
            if (data[i + 2] == 1) scLen = 3;
            else if (i + 5 < size && data[i + 2] == 0 && data[i + 3] == 1) scLen = 4;
            if (scLen > 0) {
                uint8_t nalType = data[i + scLen] & 0x1Fu;
                if (nalType == 7) return true;
                i += scLen;
            }
        }
    }
    return false;
}

std::vector<uint8_t> extractSpsPpsNals(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    if (!data || size < 5) return out;
    struct NalSpan { size_t start; size_t end; uint8_t type; };
    std::vector<NalSpan> spans;
    for (size_t i = 0; i + 3 < size; ++i) {
        size_t scLen = 0;
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) scLen = 3;
            else if (i + 4 < size && data[i + 2] == 0 && data[i + 3] == 1) scLen = 4;
        }
        if (scLen > 0) {
            if (!spans.empty()) spans.back().end = i;
            uint8_t nalType = (i + scLen < size) ? (data[i + scLen] & 0x1Fu) : 0;
            spans.push_back(NalSpan{ i, size, nalType });
            i += scLen - 1;
        }
    }
    for (size_t k = 0; k < spans.size(); ++k) {
        if ((spans[k].type == 7 || spans[k].type == 8) && spans[k].end > spans[k].start) {
            out.insert(out.end(), data + spans[k].start, data + spans[k].end);
        }
    }
    return out;
}

struct ComMfScope {
    bool comOwned = false;
    bool mfOwned = false;
    ComMfScope() {
        HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (hrCom == S_OK || hrCom == S_FALSE) {
            comOwned = true;
        }
        if (SUCCEEDED(MFStartup(MF_VERSION))) {
            mfOwned = true;
        }
    }
    ~ComMfScope() {
        if (mfOwned) MFShutdown();
        if (comOwned) CoUninitialize();
    }
};

class MfMp4Writer {
public:
    ~MfMp4Writer() { close(); }

    bool open(const std::string& mp4PathUtf8,
              uint32_t width, uint32_t height,
              const std::vector<uint8_t>& videoConfig,
              AudioCodecId audioCodec,
              const std::vector<uint8_t>& audioConfig,
              bool& outHasAudio,
              std::string& outError) {
        close();
        outHasAudio = false;
        outError.clear();

        if (width < 16 || height < 16) {
            outError = "Invalid video dimensions for MP4 muxer.";
            return false;
        }

        ensureParentDir(mp4PathUtf8);
        std::wstring wPath = utf8ToWide(mp4PathUtf8);
        if (wPath.empty()) {
            outError = "Invalid MP4 output path.";
            return false;
        }

        IMFAttributes* attrs = nullptr;
        if (SUCCEEDED(MFCreateAttributes(&attrs, 1)) && attrs) {
            attrs->SetUINT32(kLocalMF_SINK_WRITER_DISABLE_THROTTLING, 1);
        }

        HRESULT hr = MFCreateSinkWriterFromURL(wPath.c_str(), NULL, attrs, &writer_);
        safeRelease(attrs);
        if (FAILED(hr) || !writer_) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "MFCreateSinkWriterFromURL failed (hr=0x%08lX)",
                          static_cast<unsigned long>(hr));
            outError = buf;
            DeleteFileW(wPath.c_str());
            return false;
        }

        auto buildH264Type = [&](IMFMediaType** ppType) -> HRESULT {
            *ppType = nullptr;
            IMFMediaType* mt = nullptr;
            HRESULT mhr = MFCreateMediaType(&mt);
            if (SUCCEEDED(mhr) && mt) {
                mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
                mt->SetUINT32(MF_MT_AVG_BITRATE, 16000000);
                mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                MFSetAttributeSize(mt, MF_MT_FRAME_SIZE, width, height);
                MFSetAttributeRatio(mt, MF_MT_FRAME_RATE, 60, 1);
                MFSetAttributeRatio(mt, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                if (!videoConfig.empty()) {
                    mt->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, videoConfig.data(),
                                static_cast<UINT32>(videoConfig.size()));
                }
                *ppType = mt;
            }
            return mhr;
        };

        // Video passthrough (zero re-encode!): exact same IMFMediaType pointer on AddStream + SetInputMediaType
        IMFMediaType* vidType = nullptr;
        const char* vidFailStep = "MFCreateMediaType";
        hr = buildH264Type(&vidType);
        if (SUCCEEDED(hr) && vidType) {
            vidFailStep = "AddStream(vid)";
            hr = writer_->AddStream(vidType, &vidStream_);
        }
        if (SUCCEEDED(hr) && vidType) {
            vidFailStep = "SetInputMediaType(vid)";
            hr = writer_->SetInputMediaType(vidStream_, vidType, NULL);
        }
        safeRelease(vidType);

        if (FAILED(hr)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s failed (hr=0x%08lX, %ux%u, cfg=%zuB)",
                          vidFailStep, static_cast<unsigned long>(hr),
                          width, height, videoConfig.size());
            outError = buf;
            close();
            DeleteFileW(wPath.c_str());
            return false;
        }

        // Optional audio stream (RAW PCM -> AAC encode via MF, or AAC passthrough)
        if (audioCodec == AUDIO_CODEC_RAW || (audioCodec == AUDIO_CODEC_AAC && !audioConfig.empty())) {
            IMFMediaType* audOut = nullptr;
            IMFMediaType* audIn = nullptr;
            HRESULT ahr = MFCreateMediaType(&audOut);
            if (SUCCEEDED(ahr) && audOut) {
                audOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                audOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                audOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                audOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                audOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                audOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
                audOut->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
                audOut->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
                if (audioCodec == AUDIO_CODEC_AAC && !audioConfig.empty()) {
                    std::vector<uint8_t> userData(12 + audioConfig.size(), 0);
                    std::memcpy(userData.data() + 12, audioConfig.data(), audioConfig.size());
                    audOut->SetBlob(MF_MT_USER_DATA, userData.data(), static_cast<UINT32>(userData.size()));
                }
                ahr = writer_->AddStream(audOut, &audStream_);
            }
            if (SUCCEEDED(ahr)) {
                if (audioCodec == AUDIO_CODEC_AAC) {
                    ahr = writer_->SetInputMediaType(audStream_, audOut, NULL);
                } else {
                    ahr = MFCreateMediaType(&audIn);
                    if (SUCCEEDED(ahr) && audIn) {
                        audIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                        audIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                        audIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                        audIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                        audIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                        audIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
                        audIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
                        ahr = writer_->SetInputMediaType(audStream_, audIn, NULL);
                    }
                }
            }
            safeRelease(audIn);
            safeRelease(audOut);
            if (SUCCEEDED(ahr)) {
                hasAudioStream_ = true;
                outHasAudio = true;
            } else {
                // Re-open clean video-only writer if rejected audio stream was already added to SinkWriter
                close();
                DeleteFileW(wPath.c_str());
                IMFAttributes* vattrs = nullptr;
                if (SUCCEEDED(MFCreateAttributes(&vattrs, 1)) && vattrs) {
                    vattrs->SetUINT32(kLocalMF_SINK_WRITER_DISABLE_THROTTLING, 1);
                }
                HRESULT hr2 = MFCreateSinkWriterFromURL(wPath.c_str(), NULL, vattrs, &writer_);
                safeRelease(vattrs);
                if (FAILED(hr2) || !writer_) {
                    outError = "MFCreateSinkWriterFromURL failed after audio fallback.";
                    DeleteFileW(wPath.c_str());
                    return false;
                }
                IMFMediaType* vt2 = nullptr;
                HRESULT vr = buildH264Type(&vt2);
                if (SUCCEEDED(vr) && vt2) {
                    vr = writer_->AddStream(vt2, &vidStream_);
                }
                if (SUCCEEDED(vr) && vt2) {
                    vr = writer_->SetInputMediaType(vidStream_, vt2, NULL);
                }
                safeRelease(vt2);
                if (FAILED(vr)) {
                    outError = "MF video-only retry failed.";
                    close();
                    DeleteFileW(wPath.c_str());
                    return false;
                }
            }
        }

        hr = writer_->BeginWriting();
        if (FAILED(hr)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "BeginWriting failed (hr=0x%08lX)",
                          static_cast<unsigned long>(hr));
            outError = buf;
            close();
            DeleteFileW(wPath.c_str());
            return false;
        }
        writing_ = true;
        return true;
    }

    bool writeVideoSample(const uint8_t* data, size_t size,
                          LONGLONG sampleTime100ns, LONGLONG dur100ns,
                          bool isKeyFrame, HRESULT* outHr = nullptr) {
        if (!writer_ || !writing_ || !data || size == 0) return false;
        return writeStreamSample(vidStream_, data, size, sampleTime100ns, dur100ns, true, isKeyFrame, outHr);
    }

    bool writeAudioSample(const uint8_t* data, size_t size,
                          LONGLONG sampleTime100ns, LONGLONG dur100ns) {
        if (!writer_ || !writing_ || !hasAudioStream_ || !data || size == 0) return false;
        return writeStreamSample(audStream_, data, size, sampleTime100ns, dur100ns, false, false, nullptr);
    }

    bool finalize() {
        if (!writer_) return false;
        HRESULT hr = S_OK;
        if (writing_) {
            hr = writer_->Finalize();
            writing_ = false;
        }
        safeRelease(writer_);
        hasAudioStream_ = false;
        return SUCCEEDED(hr);
    }

    void close() {
        writing_ = false;
        hasAudioStream_ = false;
        safeRelease(writer_);
    }

    bool hasAudioStream() const { return hasAudioStream_; }

private:
    bool writeStreamSample(DWORD streamIdx, const uint8_t* data, size_t size,
                           LONGLONG sampleTime100ns, LONGLONG dur100ns,
                           bool isVideo, bool isKeyFrame, HRESULT* outHr) {
        IMFMediaBuffer* buf = nullptr;
        HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buf);
        if (FAILED(hr) || !buf) {
            if (outHr) *outHr = hr;
            return false;
        }
        BYTE* dst = nullptr;
        DWORD maxLen = 0;
        hr = buf->Lock(&dst, &maxLen, NULL);
        if (SUCCEEDED(hr) && dst && maxLen >= size) {
            std::memcpy(dst, data, size);
            buf->Unlock();
            buf->SetCurrentLength(static_cast<DWORD>(size));
        } else {
            if (dst) buf->Unlock();
            safeRelease(buf);
            if (outHr) *outHr = hr;
            return false;
        }

        IMFSample* sample = nullptr;
        hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr) && sample) {
            sample->AddBuffer(buf);
            sample->SetSampleTime(sampleTime100ns);
            sample->SetSampleDuration(dur100ns > 0 ? dur100ns : 166667);
            if (isVideo && isKeyFrame) {
                sample->SetUINT32(MFSampleExtension_CleanPoint, 1);
            }
            hr = writer_->WriteSample(streamIdx, sample);
        }
        safeRelease(sample);
        safeRelease(buf);
        if (outHr) *outHr = hr;
        return SUCCEEDED(hr);
    }

    IMFSinkWriter* writer_ = nullptr;
    DWORD vidStream_ = 0;
    DWORD audStream_ = 0;
    bool hasAudioStream_ = false;
    bool writing_ = false;
};

RecordingResult writeRawH264Fallback(const std::vector<uint8_t>& videoConfig,
                                     const std::vector<RecordedPacket>& packets,
                                     const std::string& mp4PathUtf8,
                                     bool isReplay,
                                     const std::string& reason) {
    RecordingResult res;
    res.isReplay = isReplay;
    res.usedH264Fallback = true;
    std::string h264Path = replaceWithH264Ext(mp4PathUtf8);
    ensureParentDir(h264Path);
    std::wstring wPath = utf8ToWide(h264Path);
    std::ofstream out;
#ifdef _WIN32
    out.open(h264Path.c_str(), std::ios::binary);
    (void)wPath;
#endif
    if (!out.is_open()) {
        res.ok = false;
        res.error = "MP4 mux failed (" + reason + ") and H.264 fallback open failed.";
        return res;
    }

    bool wroteConfig = false;
    bool started = false;
    uint64_t firstPts = 0;
    uint64_t lastPts = 0;

    for (size_t i = 0; i < packets.size(); ++i) {
        const RecordedPacket& p = packets[i];
        if (!p.isVideo || p.data.empty()) continue;
        if (!started) {
            if (!p.isKeyFrame) continue;
            started = true;
            firstPts = p.ptsUs;
            if (!videoConfig.empty() && !containsSpsNal(p.data.data(), p.data.size())) {
                out.write(reinterpret_cast<const char*>(videoConfig.data()),
                          static_cast<std::streamsize>(videoConfig.size()));
                wroteConfig = true;
            }
        }
        out.write(reinterpret_cast<const char*>(p.data.data()),
                  static_cast<std::streamsize>(p.data.size()));
        lastPts = p.ptsUs;
        ++res.videoFramesWritten;
    }
    if (!wroteConfig && res.videoFramesWritten == 0 && !videoConfig.empty()) {
        out.write(reinterpret_cast<const char*>(videoConfig.data()),
                  static_cast<std::streamsize>(videoConfig.size()));
    }
    out.close();

    res.ok = (res.videoFramesWritten > 0);
    res.outputPath = h264Path;
    if (lastPts > firstPts) {
        res.durationSec = static_cast<double>(lastPts - firstPts) / 1000000.0;
    }
    res.error = res.ok ? reason : ("No H.264 keyframes received (" + reason + ")");
    return res;
}

RecordingResult muxPacketsToFile(const ReplaySnapshot& snap,
                                 const std::string& mp4PathUtf8,
                                 bool isReplay) {
    RecordingResult res;
    res.isReplay = isReplay;
    res.outputPath = mp4PathUtf8;

    size_t firstIdrIdx = snap.packets.size();
    for (size_t i = 0; i < snap.packets.size(); ++i) {
        if (snap.packets[i].isVideo && snap.packets[i].isKeyFrame && !snap.packets[i].data.empty()) {
            firstIdrIdx = i;
            break;
        }
    }
    if (firstIdrIdx >= snap.packets.size()) {
        res.ok = false;
        res.error = "No H.264 keyframe available to write.";
        return res;
    }

    ComMfScope mfScope;
    MfMp4Writer writer;
    bool hasAudio = false;
    std::string openErr;
    std::vector<uint8_t> effectiveVidCfg = snap.videoConfig;
    if (effectiveVidCfg.empty()) {
        effectiveVidCfg = extractSpsPpsNals(snap.packets[firstIdrIdx].data.data(),
                                            snap.packets[firstIdrIdx].data.size());
    }
    uint32_t w = snap.width >= 16 ? snap.width : 1280;
    uint32_t h = snap.height >= 16 ? snap.height : 720;

    if (!writer.open(mp4PathUtf8, w, h, effectiveVidCfg, snap.audioCodec, snap.audioConfig, hasAudio, openErr)) {
        return writeRawH264Fallback(effectiveVidCfg, snap.packets, mp4PathUtf8, isReplay, openErr);
    }

    const uint64_t vidBaseUs = snap.packets[firstIdrIdx].ptsUs;
    uint64_t audBaseUs = 0;
    bool hasAudBase = false;
    LONGLONG lastVidTime = -1;
    LONGLONG lastAudTime = -1;
    uint64_t lastVidPtsUs = vidBaseUs;
    bool needInlineConfig = !snap.videoConfig.empty();

    for (size_t i = firstIdrIdx; i < snap.packets.size(); ++i) {
        const RecordedPacket& p = snap.packets[i];
        if (p.data.empty()) continue;

        if (p.isVideo) {
            LONGLONG relUs = (p.ptsUs >= vidBaseUs) ? static_cast<LONGLONG>(p.ptsUs - vidBaseUs) : 0;
            LONGLONG t100 = relUs * 10LL;
            if (t100 <= lastVidTime) t100 = lastVidTime + 10000LL;
            lastVidTime = t100;
            lastVidPtsUs = p.ptsUs;

            const uint8_t* ptr = p.data.data();
            size_t len = p.data.size();
            std::vector<uint8_t> combined;
            if (needInlineConfig) {
                needInlineConfig = false;
                if (!containsSpsNal(ptr, len)) {
                    combined.reserve(snap.videoConfig.size() + len);
                    combined.insert(combined.end(), snap.videoConfig.begin(), snap.videoConfig.end());
                    combined.insert(combined.end(), ptr, ptr + len);
                    ptr = combined.data();
                    len = combined.size();
                }
            }

            HRESULT whr = S_OK;
            if (!writer.writeVideoSample(ptr, len, t100, 166667LL, p.isKeyFrame, &whr)) {
                if (res.videoFramesWritten == 0) {
                    writer.close();
                    DeleteFileW(utf8ToWide(mp4PathUtf8).c_str());
                    char ebuf[96];
                    std::snprintf(ebuf, sizeof(ebuf), "First H.264 WriteSample failed (hr=0x%08lX)",
                                  static_cast<unsigned long>(whr));
                    return writeRawH264Fallback(snap.videoConfig, snap.packets, mp4PathUtf8, isReplay, ebuf);
                }
            } else {
                ++res.videoFramesWritten;
            }
        } else if (hasAudio) {
            if (p.ptsUs < vidBaseUs && (vidBaseUs - p.ptsUs) > 500000ULL && !hasAudBase) {
                continue;
            }
            if (!hasAudBase) {
                audBaseUs = (p.ptsUs >= vidBaseUs) ? vidBaseUs : p.ptsUs;
                hasAudBase = true;
            }
            LONGLONG relUs = (p.ptsUs >= audBaseUs) ? static_cast<LONGLONG>(p.ptsUs - audBaseUs) : 0;
            LONGLONG t100 = relUs * 10LL;
            if (t100 <= lastAudTime) t100 = lastAudTime + 10000LL;
            lastAudTime = t100;
            LONGLONG dur100 = 200000LL;
            if (snap.audioCodec == AUDIO_CODEC_RAW && p.data.size() >= 4) {
                uint64_t frames = static_cast<uint64_t>(p.data.size()) / 4ULL;
                dur100 = static_cast<LONGLONG>((frames * 10000000ULL) / 48000ULL);
            }
            if (writer.writeAudioSample(p.data.data(), p.data.size(), t100, dur100)) {
                ++res.audioPacketsWritten;
            }
        }
    }

    bool finOk = writer.finalize();
    res.hasAudio = hasAudio && (res.audioPacketsWritten > 0);
    if (lastVidPtsUs > vidBaseUs) {
        res.durationSec = static_cast<double>(lastVidPtsUs - vidBaseUs) / 1000000.0;
    } else if (res.videoFramesWritten > 0) {
        res.durationSec = static_cast<double>(res.videoFramesWritten) / 60.0;
    }
    res.ok = finOk && (res.videoFramesWritten > 0);
    if (!res.ok) {
        DeleteFileW(utf8ToWide(mp4PathUtf8).c_str());
        return writeRawH264Fallback(snap.videoConfig, snap.packets, mp4PathUtf8, isReplay, "MF Finalize failed");
    }
    return res;
}

} // namespace

// ============================================================================
// ReplayRingBuffer (pure, unit-testable)
// ============================================================================

void ReplayRingBuffer::configure(uint32_t windowSec, size_t maxBytes) {
    windowSec_ = (windowSec < 5) ? 5 : (windowSec > 120 ? 120 : windowSec);
    maxBytes_ = (maxBytes < 1024u * 1024u) ? (1024u * 1024u) : maxBytes;
    trimOldGops();
}

void ReplayRingBuffer::reset() {
    ring_.clear();
    videoConfig_.clear();
    audioConfig_.clear();
    audioCodec_ = AUDIO_CODEC_NONE;
    width_ = 0;
    height_ = 0;
    totalBytes_ = 0;
    earliestVideoPtsUs_ = 0;
    latestVideoPtsUs_ = 0;
    hasVideoPts_ = false;
}

void ReplayRingBuffer::pushVideo(const uint8_t* data, size_t size, uint64_t ptsUs,
                                 bool isConfig, bool isKeyFrame,
                                 uint32_t fallbackW, uint32_t fallbackH) {
    if (!data || size == 0) return;

    if (fallbackW >= 16 && fallbackH >= 16) {
        if (width_ == 0 || height_ == 0) {
            width_ = fallbackW;
            height_ = fallbackH;
        }
    }

    if (isConfig) {
        videoConfig_.assign(data, data + size);
        uint32_t sw = 0, sh = 0;
        if (SpsParser::parseH264Resolution(data, size, sw, sh) && sw >= 16 && sh >= 16) {
            width_ = sw;
            height_ = sh;
        }
        return;
    }

    RecordedPacket pkt;
    pkt.isVideo = true;
    pkt.ptsUs = ptsUs;
    pkt.isConfig = false;
    pkt.isKeyFrame = isKeyFrame;
    pkt.data.assign(data, data + size);

    totalBytes_ += pkt.data.size();
    ring_.push_back(std::move(pkt));

    if (!hasVideoPts_) {
        hasVideoPts_ = true;
        earliestVideoPtsUs_ = ptsUs;
        latestVideoPtsUs_ = ptsUs;
    } else {
        if (ptsUs > latestVideoPtsUs_) latestVideoPtsUs_ = ptsUs;
    }

    trimOldGops();
}

void ReplayRingBuffer::pushAudio(AudioCodecId codec, const uint8_t* data, size_t size,
                                 uint64_t ptsUs, bool isConfig) {
    if (!data || size == 0 || codec == AUDIO_CODEC_NONE) return;
    audioCodec_ = codec;
    if (isConfig) {
        audioConfig_.assign(data, data + size);
        return;
    }

    RecordedPacket pkt;
    pkt.isVideo = false;
    pkt.audioCodec = codec;
    pkt.ptsUs = ptsUs;
    pkt.isConfig = false;
    pkt.isKeyFrame = false;
    pkt.data.assign(data, data + size);

    totalBytes_ += pkt.data.size();
    ring_.push_back(std::move(pkt));
    trimOldGops();
}

void ReplayRingBuffer::trimOldGops() {
    if (ring_.empty()) {
        totalBytes_ = 0;
        earliestVideoPtsUs_ = 0;
        latestVideoPtsUs_ = 0;
        hasVideoPts_ = false;
        return;
    }

    const uint64_t windowUs = static_cast<uint64_t>(windowSec_) * 1000000ULL;
    const uint64_t cutoffUs = (hasVideoPts_ && latestVideoPtsUs_ > windowUs)
        ? (latestVideoPtsUs_ - windowUs)
        : 0;

    // Collect keyframe indices so we always start on a clean IDR keyframe.
    size_t trimToIdx = 0;
    size_t bytesBeforeTrim = 0;
    size_t runningBytes = 0;
    size_t lastSafeKeyframe = 0;
    bool haveSafeKeyframe = false;

    for (size_t i = 0; i < ring_.size(); ++i) {
        const RecordedPacket& p = ring_[i];
        if (p.isVideo && p.isKeyFrame) {
            bool tooOld = (cutoffUs > 0 && p.ptsUs <= cutoffUs);
            bool needMemSpace = (totalBytes_ - runningBytes > maxBytes_);
            if (tooOld || needMemSpace) {
                lastSafeKeyframe = i;
                bytesBeforeTrim = runningBytes;
                haveSafeKeyframe = true;
            } else {
                break;
            }
        }
        runningBytes += p.data.size();
    }

    if (haveSafeKeyframe && lastSafeKeyframe > 0) {
        trimToIdx = lastSafeKeyframe;
        for (size_t i = 0; i < trimToIdx; ++i) {
            ring_.pop_front();
        }
        totalBytes_ = (totalBytes_ >= bytesBeforeTrim) ? (totalBytes_ - bytesBeforeTrim) : 0;
    }

    // Hard safety cap if stream ran without keyframes for an extended period
    while (totalBytes_ > maxBytes_ && ring_.size() > 1) {
        totalBytes_ -= ring_.front().data.size();
        ring_.pop_front();
    }

    earliestVideoPtsUs_ = 0;
    hasVideoPts_ = false;
    for (size_t i = 0; i < ring_.size(); ++i) {
        if (ring_[i].isVideo) {
            if (!hasVideoPts_) {
                earliestVideoPtsUs_ = ring_[i].ptsUs;
                latestVideoPtsUs_ = ring_[i].ptsUs;
                hasVideoPts_ = true;
            } else if (ring_[i].ptsUs > latestVideoPtsUs_) {
                latestVideoPtsUs_ = ring_[i].ptsUs;
            }
        }
    }
}

bool ReplayRingBuffer::buildSnapshot(ReplaySnapshot& out) const {
    out = ReplaySnapshot();
    out.width = width_;
    out.height = height_;
    out.videoConfig = videoConfig_;
    out.audioCodec = audioCodec_;
    out.audioConfig = audioConfig_;

    if (ring_.empty()) return false;

    size_t firstIdr = ring_.size();
    for (size_t i = 0; i < ring_.size(); ++i) {
        if (ring_[i].isVideo && ring_[i].isKeyFrame && !ring_[i].data.empty()) {
            firstIdr = i;
            break;
        }
    }
    if (firstIdr >= ring_.size()) return false;

    const uint64_t startPtsUs = ring_[firstIdr].ptsUs;
    out.packets.reserve(ring_.size() - firstIdr);
    for (size_t i = firstIdr; i < ring_.size(); ++i) {
        const RecordedPacket& p = ring_[i];
        if (!p.isVideo && p.ptsUs + 100000ULL < startPtsUs) continue;
        out.packets.push_back(p);
    }
    return !out.packets.empty();
}

double ReplayRingBuffer::bufferedDurationSec() const {
    if (!hasVideoPts_ || latestVideoPtsUs_ <= earliestVideoPtsUs_) return 0.0;
    return static_cast<double>(latestVideoPtsUs_ - earliestVideoPtsUs_) / 1000000.0;
}

// ============================================================================
// Recorder (runtime MP4 passthrough writer + async replay saver)
// ============================================================================

struct Recorder::Impl {
    CRITICAL_SECTION lock;
    RecordingDoneFn doneCb = nullptr;
    void* doneCtx = nullptr;

    ReplayRingBuffer replayRing;
    bool replayEnabled = true;

    std::vector<uint8_t> latestVideoConfig;
    AudioCodecId latestAudioCodec = AUDIO_CODEC_NONE;
    std::vector<uint8_t> latestAudioConfig;
    uint32_t latestW = 1920;
    uint32_t latestH = 1080;

    // Live recording queue + thread
    bool recording = false;
    bool stopRequested = false;
    std::string liveOutPath;
    uint32_t liveW = 1920;
    uint32_t liveH = 1080;
    std::deque<RecordedPacket> liveQueue;
    size_t liveQueuedBytes = 0;
    ULONGLONG recStartTick = 0;
    HANDLE recWakeEvent = NULL;
    HANDLE recThread = NULL;
    std::vector<HANDLE> replayThreads;

    Impl() {
        InitializeCriticalSection(&lock);
        recWakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    }

    ~Impl() {
        if (recWakeEvent) CloseHandle(recWakeEvent);
        DeleteCriticalSection(&lock);
    }
};

struct ReplayWriteJob {
    Recorder* owner;
    ReplaySnapshot snap;
    std::string outPath;
    RecordingDoneFn cb;
    void* cbCtx;
};

static DWORD WINAPI replayWriterThreadProc(LPVOID param) {
    ReplayWriteJob* job = static_cast<ReplayWriteJob*>(param);
    if (!job) return 0;
    RecordingResult res = muxPacketsToFile(job->snap, job->outPath, true);
    if (job->cb) {
        job->cb(job->cbCtx, res);
    }
    delete job;
    return 0;
}

static DWORD WINAPI liveRecorderThreadProc(LPVOID param) {
    Recorder* rec = static_cast<Recorder*>(param);
    (void)rec;
    return 0;
}

namespace {

struct LiveWriterContext {
    CRITICAL_SECTION* lock = nullptr;
    HANDLE wakeEvent = NULL;
    bool* stopRequested = nullptr;
    bool* recordingFlag = nullptr;
    std::deque<RecordedPacket>* queue = nullptr;
    size_t* queuedBytes = nullptr;
    std::vector<uint8_t> videoConfig;
    AudioCodecId audioCodec = AUDIO_CODEC_NONE;
    std::vector<uint8_t> audioConfig;
    std::string outPath;
    uint32_t width = 1280;
    uint32_t height = 720;
    RecordingDoneFn doneCb = nullptr;
    void* doneCtx = nullptr;
};

static DWORD WINAPI liveWriterWorkerProc(LPVOID param) {
    LiveWriterContext* lctx = static_cast<LiveWriterContext*>(param);
    if (!lctx) return 0;

    ComMfScope mfScope;
    MfMp4Writer writer;
    bool writerOpened = false;
    bool hasAudio = false;
    bool fallbackMode = false;
    std::ofstream rawOut;
    std::string rawPath;
    std::string errMsg;

    bool waitingForIdr = true;
    bool needInlineConfig = true;
    uint64_t vidBaseUs = 0;
    uint64_t audBaseUs = 0;
    bool hasAudBase = false;
    LONGLONG lastVidTime = -1;
    LONGLONG lastAudTime = -1;
    uint64_t lastVidPtsUs = 0;
    uint32_t videoFrames = 0;
    uint32_t audioPkts = 0;

    auto switchToRawFallback = [&](const std::string& reason) {
        writer.close();
        DeleteFileW(utf8ToWide(lctx->outPath).c_str());
        rawPath = replaceWithH264Ext(lctx->outPath);
        ensureParentDir(rawPath);
        rawOut.open(rawPath.c_str(), std::ios::binary);
        fallbackMode = true;
        errMsg = reason;
        if (rawOut.is_open() && !lctx->videoConfig.empty()) {
            rawOut.write(reinterpret_cast<const char*>(lctx->videoConfig.data()),
                         static_cast<std::streamsize>(lctx->videoConfig.size()));
        }
    };

    for (;;) {
        std::vector<RecordedPacket> batch;
        bool stopping = false;
        EnterCriticalSection(lctx->lock);
        while (!lctx->queue->empty()) {
            batch.push_back(std::move(lctx->queue->front()));
            lctx->queue->pop_front();
        }
        *lctx->queuedBytes = 0;
        stopping = *lctx->stopRequested;
        LeaveCriticalSection(lctx->lock);

        for (size_t i = 0; i < batch.size(); ++i) {
            const RecordedPacket& p = batch[i];
            if (p.data.empty()) continue;

            if (waitingForIdr) {
                if (!p.isVideo) {
                    if (p.isConfig && lctx->audioCodec != AUDIO_CODEC_NONE) {
                        lctx->audioConfig = p.data;
                    }
                    continue;
                }
                if (p.isConfig) {
                    lctx->videoConfig = p.data;
                    uint32_t sw = 0, sh = 0;
                    if (SpsParser::parseH264Resolution(p.data.data(), p.data.size(), sw, sh) && sw >= 16 && sh >= 16) {
                        lctx->width = sw;
                        lctx->height = sh;
                    }
                    continue;
                }
                if (!p.isKeyFrame) continue;

                waitingForIdr = false;
                vidBaseUs = p.ptsUs;
                lastVidPtsUs = p.ptsUs;

                if (lctx->videoConfig.empty()) {
                    lctx->videoConfig = extractSpsPpsNals(p.data.data(), p.data.size());
                }
                uint32_t w = (lctx->width >= 16) ? lctx->width : 1280;
                uint32_t h = (lctx->height >= 16) ? lctx->height : 720;
                if (writer.open(lctx->outPath, w, h, lctx->videoConfig, lctx->audioCodec,
                                lctx->audioConfig, hasAudio, errMsg)) {
                    writerOpened = true;
                } else {
                    switchToRawFallback(errMsg);
                }
            }

            if (p.isVideo) {
                if (p.isConfig) {
                    lctx->videoConfig = p.data;
                    continue;
                }
                if (fallbackMode) {
                    if (rawOut.is_open()) {
                        rawOut.write(reinterpret_cast<const char*>(p.data.data()),
                                     static_cast<std::streamsize>(p.data.size()));
                        lastVidPtsUs = p.ptsUs;
                        ++videoFrames;
                    }
                    continue;
                }
                if (!writerOpened) continue;

                LONGLONG relUs = (p.ptsUs >= vidBaseUs) ? static_cast<LONGLONG>(p.ptsUs - vidBaseUs) : 0;
                LONGLONG t100 = relUs * 10LL;
                if (t100 <= lastVidTime) t100 = lastVidTime + 10000LL;
                lastVidTime = t100;
                lastVidPtsUs = p.ptsUs;

                const uint8_t* ptr = p.data.data();
                size_t len = p.data.size();
                std::vector<uint8_t> combined;
                if (needInlineConfig) {
                    needInlineConfig = false;
                    if (!lctx->videoConfig.empty() && !containsSpsNal(ptr, len)) {
                        combined.reserve(lctx->videoConfig.size() + len);
                        combined.insert(combined.end(), lctx->videoConfig.begin(), lctx->videoConfig.end());
                        combined.insert(combined.end(), ptr, ptr + len);
                        ptr = combined.data();
                        len = combined.size();
                    }
                }

                HRESULT whr = S_OK;
                if (!writer.writeVideoSample(ptr, len, t100, 166667LL, p.isKeyFrame, &whr)) {
                    if (videoFrames == 0) {
                        char ebuf[96];
                        std::snprintf(ebuf, sizeof(ebuf), "Live WriteSample failed (hr=0x%08lX)",
                                      static_cast<unsigned long>(whr));
                        switchToRawFallback(ebuf);
                        if (rawOut.is_open()) {
                            rawOut.write(reinterpret_cast<const char*>(ptr), static_cast<std::streamsize>(len));
                            ++videoFrames;
                        }
                    }
                } else {
                    ++videoFrames;
                }
            } else if (!fallbackMode && writerOpened && hasAudio) {
                if (p.isConfig) continue;
                if (!hasAudBase) {
                    audBaseUs = (p.ptsUs >= vidBaseUs) ? vidBaseUs : p.ptsUs;
                    hasAudBase = true;
                }
                LONGLONG relUs = (p.ptsUs >= audBaseUs) ? static_cast<LONGLONG>(p.ptsUs - audBaseUs) : 0;
                LONGLONG t100 = relUs * 10LL;
                if (t100 <= lastAudTime) t100 = lastAudTime + 10000LL;
                lastAudTime = t100;
                LONGLONG dur100 = 200000LL;
                if (lctx->audioCodec == AUDIO_CODEC_RAW && p.data.size() >= 4) {
                    uint64_t frames = static_cast<uint64_t>(p.data.size()) / 4ULL;
                    dur100 = static_cast<LONGLONG>((frames * 10000000ULL) / 48000ULL);
                }
                if (writer.writeAudioSample(p.data.data(), p.data.size(), t100, dur100)) {
                    ++audioPkts;
                }
            }
        }

        if (stopping) break;
        WaitForSingleObject(lctx->wakeEvent, 25);
    }

    RecordingResult res;
    res.isReplay = false;
    res.usedH264Fallback = fallbackMode;
    res.videoFramesWritten = videoFrames;
    res.audioPacketsWritten = audioPkts;
    res.hasAudio = (!fallbackMode && hasAudio && audioPkts > 0);
    if (lastVidPtsUs > vidBaseUs) {
        res.durationSec = static_cast<double>(lastVidPtsUs - vidBaseUs) / 1000000.0;
    } else if (videoFrames > 0) {
        res.durationSec = static_cast<double>(videoFrames) / 60.0;
    }

    if (fallbackMode) {
        if (rawOut.is_open()) rawOut.close();
        res.outputPath = rawPath;
        res.ok = (videoFrames > 0);
        res.error = res.ok ? errMsg : (errMsg.empty() ? "No video frames recorded." : errMsg);
    } else if (writerOpened) {
        bool finOk = writer.finalize();
        res.outputPath = lctx->outPath;
        res.ok = finOk && (videoFrames > 0);
        if (!res.ok) {
            DeleteFileW(utf8ToWide(lctx->outPath).c_str());
            res.error = "MP4 finalize produced 0 video frames.";
        }
    } else {
        res.outputPath = lctx->outPath;
        res.ok = false;
        res.error = "Recording stopped before receiving a video keyframe.";
    }

    EnterCriticalSection(lctx->lock);
    *lctx->recordingFlag = false;
    *lctx->stopRequested = false;
    LeaveCriticalSection(lctx->lock);

    if (lctx->doneCb) {
        lctx->doneCb(lctx->doneCtx, res);
    }
    delete lctx;
    return 0;
}

} // namespace

Recorder::Recorder() : impl_(new Impl()) {
    (void)liveRecorderThreadProc;
    impl_->replayRing.configure(30, 96u * 1024u * 1024u);
}

Recorder::~Recorder() {
    resetSession();
    delete impl_;
    impl_ = nullptr;
}

void Recorder::setDoneCallback(RecordingDoneFn fn, void* ctx) {
    EnterCriticalSection(&impl_->lock);
    impl_->doneCb = fn;
    impl_->doneCtx = ctx;
    LeaveCriticalSection(&impl_->lock);
}

void Recorder::setReplayEnabled(bool enabled, uint32_t windowSec) {
    EnterCriticalSection(&impl_->lock);
    impl_->replayEnabled = enabled;
    impl_->replayRing.configure(windowSec, 96u * 1024u * 1024u);
    if (!enabled) {
        impl_->replayRing.reset();
    }
    LeaveCriticalSection(&impl_->lock);
}

bool Recorder::isReplayEnabled() const {
    EnterCriticalSection(&impl_->lock);
    bool en = impl_->replayEnabled;
    LeaveCriticalSection(&impl_->lock);
    return en;
}

uint32_t Recorder::replayWindowSec() const {
    EnterCriticalSection(&impl_->lock);
    uint32_t w = impl_->replayRing.windowSec();
    LeaveCriticalSection(&impl_->lock);
    return w;
}

double Recorder::bufferedReplaySec() const {
    EnterCriticalSection(&impl_->lock);
    double d = impl_->replayRing.bufferedDurationSec();
    LeaveCriticalSection(&impl_->lock);
    return d;
}

void Recorder::resetSession() {
    stopRecording();

    std::vector<HANDLE> rthreads;
    EnterCriticalSection(&impl_->lock);
    rthreads.swap(impl_->replayThreads);
    impl_->replayRing.reset();
    impl_->latestVideoConfig.clear();
    impl_->latestAudioConfig.clear();
    impl_->latestAudioCodec = AUDIO_CODEC_NONE;
    impl_->latestW = 1920;
    impl_->latestH = 1080;
    LeaveCriticalSection(&impl_->lock);

    for (size_t i = 0; i < rthreads.size(); ++i) {
        if (rthreads[i]) {
            WaitForSingleObject(rthreads[i], 5000);
            CloseHandle(rthreads[i]);
        }
    }
}

void Recorder::onVideoPacket(const uint8_t* data, size_t size, uint64_t ptsUs,
                             bool isConfig, bool isKeyFrame,
                             uint32_t streamW, uint32_t streamH) {
    if (!data || size == 0) return;

    EnterCriticalSection(&impl_->lock);
    if (streamW >= 16 && streamH >= 16) {
        impl_->latestW = streamW;
        impl_->latestH = streamH;
    }
    if (isConfig) {
        impl_->latestVideoConfig.assign(data, data + size);
        uint32_t sw = 0, sh = 0;
        if (SpsParser::parseH264Resolution(data, size, sw, sh) && sw >= 16 && sh >= 16) {
            impl_->latestW = sw;
            impl_->latestH = sh;
        }
    }

    if (impl_->replayEnabled) {
        impl_->replayRing.pushVideo(data, size, ptsUs, isConfig, isKeyFrame, impl_->latestW, impl_->latestH);
    }

    if (impl_->recording && !impl_->stopRequested) {
        if (impl_->liveQueuedBytes + size <= 96u * 1024u * 1024u) {
            RecordedPacket pkt;
            pkt.isVideo = true;
            pkt.ptsUs = ptsUs;
            pkt.isConfig = isConfig;
            pkt.isKeyFrame = isKeyFrame;
            pkt.data.assign(data, data + size);
            impl_->liveQueuedBytes += pkt.data.size();
            impl_->liveQueue.push_back(std::move(pkt));
            if (impl_->recWakeEvent) SetEvent(impl_->recWakeEvent);
        }
    }
    LeaveCriticalSection(&impl_->lock);
}

void Recorder::onAudioPacket(AudioCodecId codec, const uint8_t* data, size_t size,
                             uint64_t ptsUs, bool isConfig) {
    if (!data || size == 0 || codec == AUDIO_CODEC_NONE) return;

    EnterCriticalSection(&impl_->lock);
    impl_->latestAudioCodec = codec;
    if (isConfig) {
        impl_->latestAudioConfig.assign(data, data + size);
    }

    if (impl_->replayEnabled) {
        impl_->replayRing.pushAudio(codec, data, size, ptsUs, isConfig);
    }

    if (impl_->recording && !impl_->stopRequested) {
        if (impl_->liveQueuedBytes + size <= 96u * 1024u * 1024u) {
            RecordedPacket pkt;
            pkt.isVideo = false;
            pkt.audioCodec = codec;
            pkt.ptsUs = ptsUs;
            pkt.isConfig = isConfig;
            pkt.isKeyFrame = false;
            pkt.data.assign(data, data + size);
            impl_->liveQueuedBytes += pkt.data.size();
            impl_->liveQueue.push_back(std::move(pkt));
            if (impl_->recWakeEvent) SetEvent(impl_->recWakeEvent);
        }
    }
    LeaveCriticalSection(&impl_->lock);
}

bool Recorder::startRecording(const std::string& mp4PathUtf8,
                              uint32_t streamW, uint32_t streamH,
                              std::string& outErr) {
    outErr.clear();
    if (mp4PathUtf8.empty()) {
        outErr = "Empty recording file path.";
        return false;
    }

    EnterCriticalSection(&impl_->lock);
    if (impl_->recording) {
        LeaveCriticalSection(&impl_->lock);
        outErr = "Recording is already active.";
        return false;
    }
    if (impl_->recThread) {
        HANDLE prev = impl_->recThread;
        impl_->recThread = NULL;
        LeaveCriticalSection(&impl_->lock);
        WaitForSingleObject(prev, 5000);
        CloseHandle(prev);
        EnterCriticalSection(&impl_->lock);
    }

    if (streamW >= 16 && streamH >= 16) {
        impl_->latestW = streamW;
        impl_->latestH = streamH;
    }
    impl_->liveQueue.clear();
    impl_->liveQueuedBytes = 0;
    impl_->liveOutPath = mp4PathUtf8;
    impl_->liveW = impl_->latestW;
    impl_->liveH = impl_->latestH;
    impl_->stopRequested = false;
    impl_->recording = true;
    impl_->recStartTick = GetTickCount64();

    LiveWriterContext* lctx = new LiveWriterContext();
    lctx->lock = &impl_->lock;
    lctx->wakeEvent = impl_->recWakeEvent;
    lctx->stopRequested = &impl_->stopRequested;
    lctx->recordingFlag = &impl_->recording;
    lctx->queue = &impl_->liveQueue;
    lctx->queuedBytes = &impl_->liveQueuedBytes;
    lctx->videoConfig = impl_->latestVideoConfig;
    lctx->audioCodec = impl_->latestAudioCodec;
    lctx->audioConfig = impl_->latestAudioConfig;
    lctx->outPath = impl_->liveOutPath;
    lctx->width = impl_->liveW;
    lctx->height = impl_->liveH;
    lctx->doneCb = impl_->doneCb;
    lctx->doneCtx = impl_->doneCtx;

    if (impl_->recWakeEvent) ResetEvent(impl_->recWakeEvent);
    HANDLE hThread = CreateThread(NULL, 0, liveWriterWorkerProc, lctx, 0, NULL);
    if (!hThread) {
        impl_->recording = false;
        delete lctx;
        LeaveCriticalSection(&impl_->lock);
        outErr = "Failed to spawn MP4 writer thread.";
        return false;
    }
    SetThreadPriority(hThread, THREAD_PRIORITY_BELOW_NORMAL);
    impl_->recThread = hThread;
    LeaveCriticalSection(&impl_->lock);
    return true;
}

void Recorder::stopRecording() {
    HANDLE th = NULL;
    EnterCriticalSection(&impl_->lock);
    if (impl_->recording) {
        impl_->stopRequested = true;
        if (impl_->recWakeEvent) SetEvent(impl_->recWakeEvent);
    }
    th = impl_->recThread;
    impl_->recThread = NULL;
    LeaveCriticalSection(&impl_->lock);

    if (th) {
        WaitForSingleObject(th, 8000);
        CloseHandle(th);
    }
}

bool Recorder::isRecording() const {
    EnterCriticalSection(&impl_->lock);
    bool r = impl_->recording;
    LeaveCriticalSection(&impl_->lock);
    return r;
}

double Recorder::recordingElapsedSec() const {
    EnterCriticalSection(&impl_->lock);
    if (!impl_->recording) {
        LeaveCriticalSection(&impl_->lock);
        return 0.0;
    }
    double s = static_cast<double>(GetTickCount64() - impl_->recStartTick) / 1000.0;
    LeaveCriticalSection(&impl_->lock);
    return s;
}

bool Recorder::saveReplayAsync(const std::string& mp4PathUtf8, std::string& outErr) {
    outErr.clear();
    if (mp4PathUtf8.empty()) {
        outErr = "Empty replay file path.";
        return false;
    }

    ReplaySnapshot snap;
    RecordingDoneFn cb = nullptr;
    void* cbCtx = nullptr;
    bool built = false;

    EnterCriticalSection(&impl_->lock);
    // Clean finished replay handles opportunistically
    for (size_t i = 0; i < impl_->replayThreads.size();) {
        if (WaitForSingleObject(impl_->replayThreads[i], 0) == WAIT_OBJECT_0) {
            CloseHandle(impl_->replayThreads[i]);
            impl_->replayThreads.erase(impl_->replayThreads.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    built = impl_->replayRing.buildSnapshot(snap);
    cb = impl_->doneCb;
    cbCtx = impl_->doneCtx;
    LeaveCriticalSection(&impl_->lock);

    if (!built) {
        outErr = "Replay buffer has no keyframe yet (wait ~2s after connecting).";
        return false;
    }

    ReplayWriteJob* job = new ReplayWriteJob{ this, std::move(snap), mp4PathUtf8, cb, cbCtx };
    HANDLE h = CreateThread(NULL, 0, replayWriterThreadProc, job, 0, NULL);
    if (!h) {
        delete job;
        outErr = "Failed to spawn replay writer thread.";
        return false;
    }
    SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
    EnterCriticalSection(&impl_->lock);
    impl_->replayThreads.push_back(h);
    LeaveCriticalSection(&impl_->lock);
    return true;
}

RecordingResult Recorder::writeSnapshotToFile(const ReplaySnapshot& snap,
                                                const std::string& mp4PathUtf8) {
    return muxPacketsToFile(snap, mp4PathUtf8, true);
}

bool Recorder::extractAnnexBSpsPps(const uint8_t* data, size_t size,
                                   uint32_t& outW, uint32_t& outH) {
    return SpsParser::parseH264Resolution(data, size, outW, outH);
}

bool Recorder::verifyMp4Playable(const std::string& mp4PathUtf8,
                                 bool& outHasVideo, bool& outHasAudio,
                                 uint64_t& outDuration100ns,
                                 uint32_t* outVideoSamples) {
    outHasVideo = false;
    outHasAudio = false;
    outDuration100ns = 0;
    if (outVideoSamples) *outVideoSamples = 0;

    std::wstring wPath = utf8ToWide(mp4PathUtf8);
    if (wPath.empty()) return false;

    ComMfScope mfScope;
    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(wPath.c_str(), NULL, &reader);
    if (FAILED(hr) || !reader) return false;

    IMFMediaType* vType = nullptr;
    if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &vType)) && vType) {
        outHasVideo = true;
        safeRelease(vType);
    }
    IMFMediaType* aType = nullptr;
    if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &aType)) && aType) {
        outHasAudio = true;
        safeRelease(aType);
    }

    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var))) {
        if (var.vt == VT_UI8) outDuration100ns = var.uhVal.QuadPart;
        else if (var.vt == VT_I8 && var.hVal.QuadPart > 0) outDuration100ns = static_cast<uint64_t>(var.hVal.QuadPart);
        PropVariantClear(&var);
    }

    uint32_t vidSamples = 0;
    if (outHasVideo) {
        for (int tries = 0; tries < 256; ++tries) {
            DWORD actualIdx = 0;
            DWORD flags = 0;
            LONGLONG ts = 0;
            IMFSample* samp = nullptr;
            HRESULT rhr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                             &actualIdx, &flags, &ts, &samp);
            bool gotSamp = (samp != nullptr);
            safeRelease(samp);
            if (FAILED(rhr)) break;
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) break;
            if (gotSamp && (flags & MF_SOURCE_READERF_ERROR) == 0) {
                ++vidSamples;
            }
        }
    }
    if (outVideoSamples) *outVideoSamples = vidSamples;

    safeRelease(reader);
    return outHasVideo && (vidSamples > 0);
}
