#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "bridge/TouchTypes.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class ControlPacketQueue {
public:
    static const size_t kMaxClipboardBytes = 32768;
    static std::string truncateUtf8Cap(const std::string& utf8, size_t maxBytes);

    void push(const std::vector<uint8_t>& pkt, bool highPrio);
    void push(std::vector<uint8_t>&& pkt, bool highPrio);
    bool popNext(std::vector<uint8_t>& outPkt);
    void clear();
    size_t highSize() const { return hi_.size(); }
    size_t lowSize() const { return lo_.size(); }

private:
    std::deque<std::vector<uint8_t>> hi_;
    std::deque<std::vector<uint8_t>> lo_;
};

struct ToolPaths {
    std::string adbExe;
    std::string scrcpyExe;
    std::string scrcpyServer;
    std::string scrcpyVersion;
};

struct AdbDeviceInfo {
    std::string serial;
    std::string state;   // "device", "unauthorized", "offline"
    std::string model;   // Friendly model name (e.g. "SM_G990E")
    bool wireless = false;
};

struct MdnsServiceInfo {
    std::string name;    // e.g. "studio-PhantomTouch" or "adb-RZCW401ABCD-xyz"
    std::string type;    // "_adb-tls-pairing._tcp" or "_adb-tls-connect._tcp"
    std::string addr;    // e.g. "192.168.1.42:37891"
};

typedef void (*DeviceClipboardFn)(void* ctx, const std::string& utf8Text);
typedef void (*VideoPacketFn)(void* ctx, const uint8_t* data, size_t size,
                              uint64_t ptsUs, bool isConfig, bool isKeyFrame,
                              bool present, int64_t qpcPktRecv);

struct BridgeStreamConfig {
    int maxSize = 0;            // 0 = native phone resolution
    int maxFps = 0;             // 0 = uncapped (V10), >0 = cap FPS; metadataOnly forces 1 FPS
    int bitrateMbps = 16;       // Video bitrate in Mbps (metadataOnly forces 100kbps)
    bool metadataOnly = false;  // True for legacy 1 FPS control-only mode
    bool stayAwake = true;
    bool virtualDisplay = false;
    std::string virtualDisplaySpec; // e.g. "1920x1080/320", "1280x720/240", or "auto"
};

class ScrcpyBridge {
public:
    ScrcpyBridge();
    ~ScrcpyBridge();

    bool locateTools(ToolPaths& outPaths, std::string& outError);
    bool listDevices(const ToolPaths& tools, std::vector<AdbDeviceInfo>& outDevices);
    bool connectWireless(const ToolPaths& tools, const std::string& ipPort, std::string& outMsg);
    bool pairWireless(const ToolPaths& tools, const std::string& ipPort, const std::string& pairCode, std::string& outMsg);
    bool listMdnsServices(const ToolPaths& tools, std::vector<MdnsServiceInfo>& outServices);

    static std::vector<AdbDeviceInfo> parseAdbDevicesOutput(const std::string& raw);
    static std::vector<MdnsServiceInfo> parseMdnsServicesOutput(const std::string& raw);
    static std::string parseForegroundPackage(const std::string& dumpsysRaw);
    static int parseMajorVersion(const std::string& versionStr);
    static bool parseV4SessionHeader(const uint8_t hdr[12], uint32_t& outW, uint32_t& outH, bool& outClientResized);
    static size_t serializeResizeDisplay(uint8_t* buf, uint16_t width, uint16_t height);
    static bool isPureAscii(const std::string& utf8);
    static std::vector<std::string> splitUtf8Chunks(const std::string& utf8, size_t maxBytes);

    bool launchMirrorWindow(const ToolPaths& tools, const std::string& extraArgs,
                            const std::string& windowTitle, std::string& outError,
                            bool borderless = false, const std::string& deviceSerial = "");
    bool launchAudioOnlyRelay(const ToolPaths& tools, const std::string& deviceSerial,
                              int audioBufferMs, bool rawCodec, std::string& outError);
    void stopMirrorWindow();
    void triggerVideoStallTest() { InterlockedExchange(&injectVideoStall_, 1); }
    bool runAdbShell(const std::string& shellArgs, std::string& outText, DWORD timeoutMs = 5000);

    bool connectControl(const ToolPaths& tools, int controlMaxSize, std::string& outError,
                        const std::string& deviceSerial = "", bool stayAwake = true);
    bool connectSession(const ToolPaths& tools, const BridgeStreamConfig& cfg,
                        std::string& outError, const std::string& deviceSerial = "");
    void disconnect();

    bool isConnected() const { return connected_ != 0; }
    bool isMirrorRunning() const;
    bool isVirtualDisplayActive() const { return virtualDisplayActive_; }
    DWORD mirrorProcessId() const { return mirrorPid_; }
    std::string activeSerial() const;

    std::string deviceName() const;
    void getScreenSize(uint32_t& outW, uint32_t& outH) const;

    // Multi-touch, keyboard, text, clipboard, app launcher, and display control over scrcpy control socket
    bool sendTouchPixels(TouchAction action, uint64_t pointerId, int32_t px, int32_t py, bool clampToScreen = true);
    bool sendTouchNorm(TouchAction action, uint64_t pointerId, float nx, float ny);
    bool sendKeycode(uint8_t action, uint32_t androidKeycode, uint32_t metastate = 0);
    bool sendText(const std::string& utf8Text);
    bool sendSetClipboard(const std::string& utf8Text, bool paste);
    bool sendGetClipboard(uint8_t copyKey); // 0=NONE, 1=COPY, 2=CUT
    bool sendStartApp(const std::string& appName);
    bool sendResetVideo();
    bool sendResizeDisplay(uint16_t width, uint16_t height);
    void sendAndroidNav(uint32_t androidKeycode);
    bool setDisplayPower(bool on);

    void setClipboardCallback(DeviceClipboardFn fn, void* ctx);
    void setVideoPacketCallback(VideoPacketFn fn, void* ctx);

    // Foreground app inspection & Drag-and-Drop file/APK helpers
    std::string queryForegroundPackage();
    bool installApk(const std::string& localApkPath, std::string& outMsg);
    bool pushFileToDownloads(const std::string& localFilePath, std::string& outMsg);

    // Captures a native device PNG screenshot via adb exec-out screencap -p
    bool captureScreenshot(const std::string& savePath, std::string& outError);

    // Thread-safe activity log ring buffer for the in-app Log Viewer
    void appendLog(const std::string& line);
    std::vector<std::string> getRecentLogs() const;

private:
    static DWORD WINAPI videoReaderProc(LPVOID param);
    static DWORD WINAPI controlReaderProc(LPVOID param);
    static DWORD WINAPI controlSenderProc(LPVOID param);
    void runVideoReader();
    void runControlReader();
    void runControlSender();
    bool enqueueControlPacket(std::vector<uint8_t>&& pkt, bool highPrio);

    std::string adbPrefix() const;
    bool sendAll(SOCKET s, const uint8_t* data, int len);
    bool recvAll(SOCKET s, uint8_t* data, int len);
    bool runCommandCapture(const std::string& cmdLine, std::string& outText, DWORD& outExitCode, DWORD timeoutMs = 15000);
    bool runCommandCaptureBinary(const std::string& cmdLine, std::vector<uint8_t>& outBytes, DWORD& outExitCode, DWORD timeoutMs = 15000);

    bool wsaInit_ = false;
    volatile LONG connected_ = 0;
    volatile LONG stopThreads_ = 0;
    volatile LONG injectVideoStall_ = 0;

    SOCKET videoSock_ = INVALID_SOCKET;
    SOCKET controlSock_ = INVALID_SOCKET;

    HANDLE videoThread_ = NULL;
    HANDLE controlThread_ = NULL;
    HANDLE controlSenderThread_ = NULL;
    HANDLE controlSendEvent_ = NULL;
    HANDLE serverProc_ = NULL;
    HANDLE mirrorProc_ = NULL;
    DWORD mirrorPid_ = 0;

    mutable CRITICAL_SECTION sendLock_;
    mutable CRITICAL_SECTION metaLock_;
    mutable CRITICAL_SECTION procSpawnLock_;
    ControlPacketQueue controlQueue_;

    std::string adbExe_;
    std::string activeSerial_;
    std::string scid_;
    int forwardPort_ = 27199;
    bool isV4Proto_ = false;

    std::string deviceName_;
    uint32_t screenW_ = 1920;
    uint32_t screenH_ = 1080;
    std::vector<std::string> logs_;

    uint64_t clipSeq_ = 1;
    DeviceClipboardFn clipCb_ = nullptr;
    void* clipCbCtx_ = nullptr;
    VideoPacketFn videoCb_ = nullptr;
    void* videoCbCtx_ = nullptr;
    bool virtualDisplayActive_ = false;
};
