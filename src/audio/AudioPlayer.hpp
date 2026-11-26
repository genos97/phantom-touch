#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Must stay free of <windows.h> so ScrcpyBridge.hpp can include it before <winsock2.h>.

enum AudioCodecId {
    AUDIO_CODEC_NONE = 0,
    AUDIO_CODEC_RAW  = 1, // 0x00726177: 48 kHz stereo signed 16-bit LE PCM
    AUDIO_CODEC_AAC  = 2  // 0x00616163: 48 kHz stereo raw AAC-LC access units
};

struct AudioStats {
    uint64_t packetsReceived = 0;
    uint64_t framesPushed = 0;
    uint64_t framesPlayed = 0;
    uint64_t underflowFrames = 0;
    uint64_t droppedFrames = 0;
    float bufferedMs = 0.0f;
    double driftRate = 1.0;
    AudioCodecId codec = AUDIO_CODEC_NONE;
    bool wasapiActive = false;
};

// Bounded stereo float32 ring buffer with pre-roll priming, hysteresis clock-drift
// resampling, PTS discontinuity tracking, and click-free underrun/overflow fading.
class AudioDriftRegulator {
public:
    AudioDriftRegulator();

    void configure(uint32_t sampleRate, uint32_t targetMs);
    void reset();

    void pushInterleavedFloat(const float* samples, uint32_t frames, uint64_t ptsUs);
    void pushInterleavedS16(const int16_t* samples, uint32_t frames, uint64_t ptsUs);
    bool pullInterleavedFloat(float* out, uint32_t frames);

    uint32_t bufferedFrames() const { return buffered_; }
    uint32_t targetFrames() const { return targetFrames_; }
    bool isPrimed() const { return primed_; }
    uint64_t underflowFrames() const { return underflowFrames_; }
    uint64_t droppedFrames() const { return droppedFrames_; }
    double currentRate() const { return driftRate_; }

private:
    void writeRing(const float* src, uint32_t frames);
    void writeRingSilence(uint32_t frames);
    void discardOldest(uint32_t frames);

    uint32_t sampleRate_ = 48000;
    uint32_t targetFrames_ = 1200;
    uint32_t capFrames_ = 48000;
    uint32_t writePos_ = 0;
    uint32_t readPos_ = 0;
    uint32_t buffered_ = 0;
    bool primed_ = false;
    double avgFrames_ = 0.0;
    double driftRate_ = 1.0;
    double fracPhase_ = 0.0;
    bool driftActive_ = false;
    uint64_t nextExpectedPtsUs_ = 0;
    uint64_t underflowFrames_ = 0;
    uint64_t droppedFrames_ = 0;
    uint32_t fadeInRemain_ = 0;
    float prevL_ = 0.0f;
    float prevR_ = 0.0f;
    std::vector<float> ring_;
    std::vector<float> s16Scratch_;
};

// In-app WASAPI shared event-driven audio player + Windows Media Foundation AAC decoder.
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    void prepareForSession();
    bool start(AudioCodecId codec, int targetBufferMs, std::string& outError);
    void requestStop();
    void stop();

    bool isRunning() const;
    bool isStopRequested() const;
    void setMuted(bool muted);
    bool isMuted() const;

    void onAudioPacket(AudioCodecId codec, const uint8_t* data, size_t size, uint64_t ptsUs, bool isConfig);
    AudioStats getStats() const;

    static std::vector<uint8_t> buildMfAacUserData(const uint8_t* asc, size_t ascLen);
    static bool parseAacAudioSpecificConfig(const uint8_t* asc, size_t ascLen,
                                            uint32_t& outSampleRate, uint32_t& outChannels);
    static bool probeMfAacDecoderInit(const uint8_t* asc, size_t ascLen, std::string& outError);

private:
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    struct Impl;
    Impl* impl_;
};
