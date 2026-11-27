#pragma once

#include "audio/AudioPlayer.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// Must stay free of <windows.h> so headers including winsock2.h remain clean.

struct RecordedPacket {
    bool isVideo = true;
    AudioCodecId audioCodec = AUDIO_CODEC_NONE;
    uint64_t ptsUs = 0;
    bool isConfig = false;
    bool isKeyFrame = false;
    std::vector<uint8_t> data;
};

struct RecordingResult {
    bool ok = false;
    bool isReplay = false;
    bool usedH264Fallback = false;
    bool hasAudio = false;
    uint32_t videoFramesWritten = 0;
    uint32_t audioPacketsWritten = 0;
    double durationSec = 0.0;
    std::string outputPath;
    std::string error;
};

struct ReplaySnapshot {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> videoConfig;
    AudioCodecId audioCodec = AUDIO_CODEC_NONE;
    std::vector<uint8_t> audioConfig;
    std::vector<RecordedPacket> packets;
};

// Pure unit-testable bounded keyframe-aligned circular replay buffer (F12).
class ReplayRingBuffer {
public:
    ReplayRingBuffer() = default;

    void configure(uint32_t windowSec, size_t maxBytes = 96u * 1024u * 1024u);
    void reset();

    void pushVideo(const uint8_t* data, size_t size, uint64_t ptsUs,
                   bool isConfig, bool isKeyFrame,
                   uint32_t fallbackW, uint32_t fallbackH);
    void pushAudio(AudioCodecId codec, const uint8_t* data, size_t size,
                   uint64_t ptsUs, bool isConfig);

    bool buildSnapshot(ReplaySnapshot& out) const;

    size_t packetCount() const { return ring_.size(); }
    size_t totalBytes() const { return totalBytes_; }
    uint32_t windowSec() const { return windowSec_; }
    double bufferedDurationSec() const;

private:
    void trimOldGops();

    uint32_t windowSec_ = 30;
    size_t maxBytes_ = 96u * 1024u * 1024u;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::vector<uint8_t> videoConfig_;
    AudioCodecId audioCodec_ = AUDIO_CODEC_NONE;
    std::vector<uint8_t> audioConfig_;
    std::deque<RecordedPacket> ring_;
    size_t totalBytes_ = 0;
    uint64_t earliestVideoPtsUs_ = 0;
    uint64_t latestVideoPtsUs_ = 0;
    bool hasVideoPts_ = false;
};

typedef void (*RecordingDoneFn)(void* ctx, const RecordingResult& result);

// Zero-reencode Media Foundation H.264 + audio MP4 recorder & Instant Replay manager (F12).
// Packet taps return in microseconds under a tiny lock; all MF muxing runs on background threads.
class Recorder {
public:
    Recorder();
    ~Recorder();

    void setDoneCallback(RecordingDoneFn fn, void* ctx);
    void setReplayEnabled(bool enabled, uint32_t windowSec = 30);
    bool isReplayEnabled() const;
    uint32_t replayWindowSec() const;
    double bufferedReplaySec() const;

    void resetSession();

    void onVideoPacket(const uint8_t* data, size_t size, uint64_t ptsUs,
                       bool isConfig, bool isKeyFrame,
                       uint32_t streamW, uint32_t streamH);
    void onAudioPacket(AudioCodecId codec, const uint8_t* data, size_t size,
                       uint64_t ptsUs, bool isConfig);

    bool startRecording(const std::string& mp4PathUtf8,
                        uint32_t streamW, uint32_t streamH,
                        std::string& outErr);
    void stopRecording();
    bool isRecording() const;
    double recordingElapsedSec() const;

    bool saveReplayAsync(const std::string& mp4PathUtf8, std::string& outErr);

    static RecordingResult writeSnapshotToFile(const ReplaySnapshot& snap,
                                               const std::string& mp4PathUtf8);
    static bool verifyMp4Playable(const std::string& mp4PathUtf8,
                                  bool& outHasVideo, bool& outHasAudio,
                                  uint64_t& outDuration100ns,
                                  uint32_t* outVideoSamples = nullptr);
    static bool extractAnnexBSpsPps(const uint8_t* data, size_t size,
                                    uint32_t& outW, uint32_t& outH);

private:
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    struct Impl;
    Impl* impl_;
};
