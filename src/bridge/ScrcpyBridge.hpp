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
#include <string>
#include <vector>

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

class ScrcpyBridge {
public:
    ScrcpyBridge();
    ~ScrcpyBridge();

    bool locateTools(ToolPaths& outPaths, std::string& outError);
    bool listDevices(const ToolPaths& tools, std::vector<AdbDeviceInfo>& outDevices);
    bool connectWireless(const ToolPaths& tools, const std::string& ipPort, std::string& outMsg);

    static std::vector<AdbDeviceInfo> parseAdbDevicesOutput(const std::string& raw);

    bool launchMirrorWindow(const ToolPaths& tools, const std::string& extraArgs,
                            const std::string& windowTitle, std::string& outError,
                            bool borderless = false, const std::string& deviceSerial = "");
    void stopMirrorWindow();

    bool connectControl(const ToolPaths& tools, int controlMaxSize, std::string& outError,
                        const std::string& deviceSerial = "", bool stayAwake = true);
    void disconnect();

    bool isConnected() const { return connected_ != 0; }
    bool isMirrorRunning() const;
    DWORD mirrorProcessId() const { return mirrorPid_; }
    std::string activeSerial() const;

    std::string deviceName() const;
    void getScreenSize(uint32_t& outW, uint32_t& outH) const;

    // Sends a 32-byte SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT packet
    bool sendTouchPixels(TouchAction action, uint64_t pointerId, int32_t px, int32_t py, bool clampToScreen = true);
    bool sendTouchNorm(TouchAction action, uint64_t pointerId, float nx, float ny);
    bool sendKeycode(uint8_t action, uint32_t androidKeycode);
    void sendAndroidNav(uint32_t androidKeycode);
    bool setDisplayPower(bool on);

    // Captures a native device PNG screenshot via adb exec-out screencap -p
    bool captureScreenshot(const std::string& savePath, std::string& outError);

    // Thread-safe activity log ring buffer for the in-app Log Viewer
    void appendLog(const std::string& line);
    std::vector<std::string> getRecentLogs() const;

private:
    static DWORD WINAPI videoReaderProc(LPVOID param);
    static DWORD WINAPI controlReaderProc(LPVOID param);
    void runVideoReader();
    void runControlReader();

    std::string adbPrefix() const;
    bool sendAll(SOCKET s, const uint8_t* data, int len);
    bool recvAll(SOCKET s, uint8_t* data, int len);
    bool runCommandCapture(const std::string& cmdLine, std::string& outText, DWORD& outExitCode);
    bool runCommandCaptureBinary(const std::string& cmdLine, std::vector<uint8_t>& outBytes, DWORD& outExitCode);

    bool wsaInit_ = false;
    volatile LONG connected_ = 0;
    volatile LONG stopThreads_ = 0;

    SOCKET videoSock_ = INVALID_SOCKET;
    SOCKET controlSock_ = INVALID_SOCKET;

    HANDLE videoThread_ = NULL;
    HANDLE controlThread_ = NULL;
    HANDLE serverProc_ = NULL;
    HANDLE mirrorProc_ = NULL;
    DWORD mirrorPid_ = 0;

    mutable CRITICAL_SECTION sendLock_;
    mutable CRITICAL_SECTION metaLock_;

    std::string adbExe_;
    std::string activeSerial_;
    std::string scid_;
    int forwardPort_ = 27199;
    bool isV4Proto_ = false;

    std::string deviceName_;
    uint32_t screenW_ = 1920;
    uint32_t screenH_ = 1080;
    std::vector<std::string> logs_;
};
