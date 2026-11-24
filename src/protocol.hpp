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

#include <cstdint>
#include <string>

enum TouchAction : uint8_t {
    TOUCH_DOWN = 0,
    TOUCH_UP   = 1,
    TOUCH_MOVE = 2
};

struct ToolPaths {
    std::string adbExe;
    std::string scrcpyExe;
    std::string scrcpyServer;
    std::string scrcpyVersion;
};

class ScrcpyBridge {
public:
    ScrcpyBridge();
    ~ScrcpyBridge();

    // Searches ./scrcpy/, exe dir, and PATH for adb, scrcpy, and scrcpy-server
    bool locateTools(ToolPaths& outPaths, std::string& outError);

    // Launches the display-only scrcpy mirror window (--no-control)
    bool launchMirrorWindow(const ToolPaths& tools, const std::string& extraArgs,
                            const std::string& windowTitle, std::string& outError);

    // Pushes scrcpy-server, forwards TCP port, connects control + low-fps metadata stream
    bool connectControl(const ToolPaths& tools, int controlMaxSize, std::string& outError);

    void disconnect();

    bool isConnected() const { return connected_ != 0; }
    bool isMirrorRunning() const;

    std::string deviceName() const;
    void getScreenSize(uint32_t& outW, uint32_t& outH) const;

    // Sends a 32-byte SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT packet
    // When clampToScreen is false (e.g. unbounded MOVE/UP in Free Fire aim), signed coords pass through un-clamped.
    bool sendTouchPixels(TouchAction action, uint64_t pointerId, int32_t px, int32_t py, bool clampToScreen = true);
    bool sendTouchNorm(TouchAction action, uint64_t pointerId, float nx, float ny);

    // Sends Android Back/Home/etc. keycode if needed
    bool sendKeycode(uint8_t action, uint32_t androidKeycode);

private:
    static DWORD WINAPI videoReaderProc(LPVOID param);
    static DWORD WINAPI controlReaderProc(LPVOID param);
    void runVideoReader();
    void runControlReader();

    bool sendAll(SOCKET s, const uint8_t* data, int len);
    bool recvAll(SOCKET s, uint8_t* data, int len);

    bool runCommandCapture(const std::string& cmdLine, std::string& outText, DWORD& outExitCode);

    bool wsaInit_ = false;
    volatile LONG connected_ = 0;
    volatile LONG stopThreads_ = 0;

    SOCKET videoSock_ = INVALID_SOCKET;
    SOCKET controlSock_ = INVALID_SOCKET;

    HANDLE videoThread_ = NULL;
    HANDLE controlThread_ = NULL;
    HANDLE serverProc_ = NULL;
    HANDLE mirrorProc_ = NULL;

    mutable CRITICAL_SECTION sendLock_;
    mutable CRITICAL_SECTION metaLock_;

    std::string adbExe_;
    std::string scid_;
    int forwardPort_ = 27199;

    std::string deviceName_;
    uint32_t screenW_ = 1920;
    uint32_t screenH_ = 1080;
    bool isV4Proto_ = false;
};
