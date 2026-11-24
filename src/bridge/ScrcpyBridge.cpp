#include "bridge/ScrcpyBridge.hpp"
#include "bridge/SpsParser.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

bool fileExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

std::string getExeDir() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string s(buf);
    size_t pos = s.find_last_of("\\/");
    return (pos == std::string::npos) ? "." : s.substr(0, pos);
}

std::string findInPath(const std::string& filename) {
    char buf[MAX_PATH] = {0};
    DWORD n = SearchPathA(NULL, filename.c_str(), NULL, MAX_PATH, buf, NULL);
    if (n > 0 && n < MAX_PATH) {
        return std::string(buf);
    }
    return "";
}

std::string findToolFile(const std::vector<std::string>& searchDirs, const std::string& name) {
    for (size_t i = 0; i < searchDirs.size(); ++i) {
        std::string full = searchDirs[i] + "\\" + name;
        if (fileExists(full)) return full;
    }
    return findInPath(name);
}

void writeU16BE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

void writeU32BE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

void writeU64BE(uint8_t* p, uint64_t v) {
    writeU32BE(p, static_cast<uint32_t>(v >> 32));
    writeU32BE(p + 4, static_cast<uint32_t>(v & 0xFFFFFFFFu));
}

uint32_t readU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           static_cast<uint32_t>(p[3]);
}

uint64_t readU64BE(const uint8_t* p) {
    return (static_cast<uint64_t>(readU32BE(p)) << 32) |
           static_cast<uint64_t>(readU32BE(p + 4));
}

std::string parseScrcpyVersion(const std::string& output) {
    // Expected first line: "scrcpy 3.1 <https://...>"
    size_t pos = output.find("scrcpy ");
    if (pos == std::string::npos) return "";
    pos += 7;
    size_t end = pos;
    while (end < output.size() &&
           (output[end] == '.' || (output[end] >= '0' && output[end] <= '9'))) {
        ++end;
    }
    return output.substr(pos, end - pos);
}

} // namespace

ScrcpyBridge::ScrcpyBridge() {
    InitializeCriticalSection(&sendLock_);
    InitializeCriticalSection(&metaLock_);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        wsaInit_ = true;
    }
}

ScrcpyBridge::~ScrcpyBridge() {
    disconnect();
    if (wsaInit_) {
        WSACleanup();
    }
    DeleteCriticalSection(&sendLock_);
    DeleteCriticalSection(&metaLock_);
}

bool ScrcpyBridge::runCommandCapture(const std::string& cmdLine, std::string& outText, DWORD& outExitCode) {
    outText.clear();
    outExitCode = 1;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = NULL, writePipe = NULL;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError  = writePipe;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<char> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back('\0');

    BOOL ok = CreateProcessA(NULL, mutableCmd.data(), NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(writePipe);

    if (!ok) {
        CloseHandle(readPipe);
        return false;
    }

    char buf[1024];
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        outText.append(buf, bytesRead);
    }

    WaitForSingleObject(pi.hProcess, 15000);
    GetExitCodeProcess(pi.hProcess, &outExitCode);

    CloseHandle(readPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

bool ScrcpyBridge::runCommandCaptureBinary(const std::string& cmdLine, std::vector<uint8_t>& outBytes, DWORD& outExitCode) {
    outBytes.clear();
    outExitCode = 1;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = NULL, writePipe = NULL;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError  = NULL;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<char> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back('\0');

    BOOL ok = CreateProcessA(NULL, mutableCmd.data(), NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return false;
    }

    uint8_t buf[4096];
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &bytesRead, NULL) && bytesRead > 0) {
        outBytes.insert(outBytes.end(), buf, buf + bytesRead);
    }

    WaitForSingleObject(pi.hProcess, 15000);
    GetExitCodeProcess(pi.hProcess, &outExitCode);

    CloseHandle(readPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

void ScrcpyBridge::appendLog(const std::string& line) {
    if (line.empty()) return;
    std::printf("%s\n", line.c_str());
    EnterCriticalSection(&metaLock_);
    logs_.push_back(line);
    if (logs_.size() > 80) {
        logs_.erase(logs_.begin(), logs_.begin() + (logs_.size() - 80));
    }
    LeaveCriticalSection(&metaLock_);
}

std::vector<std::string> ScrcpyBridge::getRecentLogs() const {
    EnterCriticalSection(&metaLock_);
    std::vector<std::string> copy = logs_;
    LeaveCriticalSection(&metaLock_);
    return copy;
}

std::string ScrcpyBridge::adbPrefix() const {
    if (activeSerial_.empty()) {
        return "\"" + adbExe_ + "\"";
    }
    return "\"" + adbExe_ + "\" -s \"" + activeSerial_ + "\"";
}

std::vector<AdbDeviceInfo> ScrcpyBridge::parseAdbDevicesOutput(const std::string& raw) {
    std::vector<AdbDeviceInfo> result;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) continue;
        if (line.find("List of devices") == 0 || line[0] == '*') continue;

        std::istringstream ls(line);
        std::string serial, state;
        if (!(ls >> serial >> state)) continue;

        AdbDeviceInfo info;
        info.serial = serial;
        info.state = state;
        info.wireless = (serial.find(':') != std::string::npos);

        std::string tok;
        while (ls >> tok) {
            if (tok.find("model:") == 0) {
                info.model = tok.substr(6);
                for (size_t i = 0; i < info.model.size(); ++i) {
                    if (info.model[i] == '_') info.model[i] = ' ';
                }
            }
        }
        if (info.model.empty()) {
            info.model = info.serial;
        }
        result.push_back(info);
    }
    return result;
}

bool ScrcpyBridge::listDevices(const ToolPaths& tools, std::vector<AdbDeviceInfo>& outDevices) {
    outDevices.clear();
    if (tools.adbExe.empty()) return false;
    std::string raw;
    DWORD ec = 0;
    if (!runCommandCapture("\"" + tools.adbExe + "\" devices -l", raw, ec) || ec != 0) {
        return false;
    }
    outDevices = parseAdbDevicesOutput(raw);
    return true;
}

bool ScrcpyBridge::connectWireless(const ToolPaths& tools, const std::string& ipPort, std::string& outMsg) {
    outMsg.clear();
    if (tools.adbExe.empty() || ipPort.empty()) {
        outMsg = "Enter an IP:Port (e.g. 192.168.1.50:5555)";
        return false;
    }
    DWORD ec = 0;
    std::string raw;
    if (!runCommandCapture("\"" + tools.adbExe + "\" connect \"" + ipPort + "\"", raw, ec)) {
        outMsg = "Failed to execute adb connect.";
        return false;
    }
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
    outMsg = raw.empty() ? "adb connect completed" : raw;
    appendLog("[ADB] " + outMsg);
    return (raw.find("connected to") != std::string::npos || raw.find("already connected") != std::string::npos);
}

bool ScrcpyBridge::locateTools(ToolPaths& outPaths, std::string& outError) {
    std::string exeDir = getExeDir();
    std::vector<std::string> dirs;
    dirs.push_back(exeDir + "\\scrcpy");
    dirs.push_back(exeDir);
    dirs.push_back(".\\scrcpy");
    dirs.push_back(".");

    outPaths.scrcpyExe = findToolFile(dirs, "scrcpy.exe");
    if (!outPaths.scrcpyExe.empty()) {
        // Also prioritize the directory where scrcpy.exe lives
        size_t pos = outPaths.scrcpyExe.find_last_of("\\/");
        if (pos != std::string::npos) {
            dirs.insert(dirs.begin(), outPaths.scrcpyExe.substr(0, pos));
        }
    }

    outPaths.adbExe = findToolFile(dirs, "adb.exe");
    outPaths.scrcpyServer = findToolFile(dirs, "scrcpy-server");
    if (outPaths.scrcpyServer.empty()) {
        outPaths.scrcpyServer = findToolFile(dirs, "scrcpy-server.jar");
    }

    if (outPaths.scrcpyExe.empty() || outPaths.adbExe.empty() || outPaths.scrcpyServer.empty()) {
        outError = "Could not find scrcpy.exe / adb.exe / scrcpy-server.\n"
                   "Run setup_scrcpy.ps1 (or unzip official scrcpy into .\\scrcpy\\).";
        return false;
    }

    std::string verOut;
    DWORD exitCode = 0;
    if (runCommandCapture("\"" + outPaths.scrcpyExe + "\" --version", verOut, exitCode)) {
        outPaths.scrcpyVersion = parseScrcpyVersion(verOut);
    }
    if (outPaths.scrcpyVersion.empty()) {
        outPaths.scrcpyVersion = "3.1";
    }
    return true;
}

bool ScrcpyBridge::launchMirrorWindow(const ToolPaths& tools, const std::string& extraArgs,
                                      const std::string& windowTitle, std::string& outError,
                                      bool borderless, const std::string& deviceSerial) {
    if (mirrorProc_ != NULL) {
        return true;
    }

    std::string cmd = "\"" + tools.scrcpyExe + "\" --no-control --window-title=\"" + windowTitle + "\"";
    if (!deviceSerial.empty()) {
        cmd += " -s \"" + deviceSerial + "\"";
    }
    if (borderless) {
        // Start borderless and offscreen until ViewportHost embeds it via SetParent (zero stray window flash!)
        cmd += " --window-borderless --window-x=-32000 --window-y=-32000";
    }
    if (!extraArgs.empty()) {
        cmd += " " + extraArgs;
    }

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = borderless ? SW_SHOWNOACTIVATE : SW_SHOW;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');

    if (!CreateProcessA(NULL, mutableCmd.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        outError = "Failed to launch scrcpy mirror window.";
        return false;
    }

    mirrorProc_ = pi.hProcess;
    mirrorPid_ = pi.dwProcessId;
    CloseHandle(pi.hThread);
    appendLog("[Mirror] Launched scrcpy (" + (borderless ? std::string("embedded") : std::string("windowed")) + ")");
    return true;
}

void ScrcpyBridge::stopMirrorWindow() {
    if (mirrorProc_) {
        TerminateProcess(mirrorProc_, 0);
        WaitForSingleObject(mirrorProc_, 500);
        CloseHandle(mirrorProc_);
        mirrorProc_ = NULL;
        mirrorPid_ = 0;
    }
}

bool ScrcpyBridge::isMirrorRunning() const {
    if (!mirrorProc_) return false;
    return WaitForSingleObject(mirrorProc_, 0) == WAIT_TIMEOUT;
}

std::string ScrcpyBridge::activeSerial() const {
    EnterCriticalSection(&metaLock_);
    std::string copy = activeSerial_;
    LeaveCriticalSection(&metaLock_);
    return copy;
}

bool ScrcpyBridge::connectControl(const ToolPaths& tools, int controlMaxSize, std::string& outError,
                                  const std::string& deviceSerial) {
    if (!wsaInit_) {
        outError = "Winsock initialization failed.";
        return false;
    }

    adbExe_ = tools.adbExe;
    EnterCriticalSection(&metaLock_);
    activeSerial_ = deviceSerial;
    LeaveCriticalSection(&metaLock_);

    // 1. Verify an authorized Android device is connected
    std::vector<AdbDeviceInfo> devs;
    if (!listDevices(tools, devs)) {
        outError = "Failed to execute adb devices.";
        return false;
    }
    std::string chosenSerial = deviceSerial;
    bool foundReady = false;
    bool foundUnauth = false;
    for (size_t i = 0; i < devs.size(); ++i) {
        if (devs[i].state == "unauthorized") foundUnauth = true;
        if (devs[i].state == "device") {
            if (chosenSerial.empty() || chosenSerial == devs[i].serial) {
                chosenSerial = devs[i].serial;
                foundReady = true;
                break;
            }
        }
    }
    if (!foundReady) {
        if (foundUnauth) {
            outError = "Phone is unauthorized! Tap 'Allow USB debugging' on your phone screen.";
        } else {
            outError = "No authorized Android device detected. Plug in USB or connect Wireless ADB.";
        }
        return false;
    }
    EnterCriticalSection(&metaLock_);
    activeSerial_ = chosenSerial;
    LeaveCriticalSection(&metaLock_);

    std::string adb = adbPrefix();

    // 2. Push scrcpy-server binary to a unique path so cleanup=true never races with scrcpy.exe!
    std::string pushOut;
    DWORD exitCode = 0;
    if (!runCommandCapture(adb + " push \"" + tools.scrcpyServer +
                           "\" /data/local/tmp/km-scrcpy-server.jar", pushOut, exitCode) || exitCode != 0) {
        outError = "Failed to push scrcpy-server to device:\n" + pushOut;
        return false;
    }

    // 3. Create unique 31-bit scid and adb forward tunnel
    std::srand(static_cast<unsigned int>(std::time(NULL) ^ GetTickCount()));
    uint32_t scidVal = (static_cast<uint32_t>(std::rand()) << 16) ^
                       static_cast<uint32_t>(std::rand());
    scidVal &= 0x7FFFFFFFu;
    if (scidVal == 0) scidVal = 0x12345678u;

    char scidBuf[16];
    std::snprintf(scidBuf, sizeof(scidBuf), "%08x", scidVal);
    scid_ = scidBuf;
    forwardPort_ = 27199;

    char portBuf[16];
    std::snprintf(portBuf, sizeof(portBuf), "%d", forwardPort_);

    std::string fwdOut;
    std::string fwdCmd = adb + " forward tcp:" + portBuf + " localabstract:scrcpy_" + scid_;
    if (!runCommandCapture(fwdCmd, fwdOut, exitCode) || exitCode != 0) {
        outError = "Failed to set up adb forward tunnel:\n" + fwdOut;
        return false;
    }

    // 4. Launch scrcpy-server in low-FPS metadata + full multi-touch control mode
    std::string srvCmd = adb + " shell CLASSPATH=/data/local/tmp/km-scrcpy-server.jar "
                         "app_process / com.genymobile.scrcpy.Server " + tools.scrcpyVersion +
                         " scid=" + scid_ +
                         " log_level=info"
                         " video=true"
                         " audio=false"
                         " control=true"
                         " video_codec=h264"
                         " max_fps=1"
                         " video_bit_rate=100000"
                         " tunnel_forward=true"
                         " send_dummy_byte=true"
                         " send_device_meta=true"
                         " send_codec_meta=true"
                         " send_frame_meta=true"
                         " clipboard_autosync=false"
                         " power_on=false"
                         " cleanup=false";
    if (controlMaxSize > 0) {
        char szBuf[32];
        std::snprintf(szBuf, sizeof(szBuf), " max_size=%d", controlMaxSize);
        srvCmd += szBuf;
    }

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    std::vector<char> mutableSrv(srvCmd.begin(), srvCmd.end());
    mutableSrv.push_back('\0');

    if (!CreateProcessA(NULL, mutableSrv.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        outError = "Failed to start scrcpy-server process.";
        return false;
    }
    serverProc_ = pi.hProcess;
    CloseHandle(pi.hThread);

    // 5. Connect Video socket #1 and wait for the 1-byte dummy handshake
    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(forwardPort_));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    bool gotDummy = false;
    for (int attempt = 0; attempt < 60; ++attempt) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            Sleep(100);
            continue;
        }
        if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            uint8_t dummy = 0;
            int r = recv(s, reinterpret_cast<char*>(&dummy), 1, 0);
            if (r == 1) {
                videoSock_ = s;
                gotDummy = true;
                break;
            }
        }
        closesocket(s);
        Sleep(100);
    }

    if (!gotDummy) {
        outError = "Timed out waiting for scrcpy-server control bridge.\n"
                   "Check USB debugging and scrcpy version compatibility (" + tools.scrcpyVersion + ").";
        disconnect();
        return false;
    }

    // 6. Connect Control socket #2 (TCP_NODELAY for ultra-low input latency!)
    controlSock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (controlSock_ == INVALID_SOCKET ||
        connect(controlSock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        outError = "Failed to connect to scrcpy control socket.";
        disconnect();
        return false;
    }
    BOOL nodelay = TRUE;
    setsockopt(controlSock_, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    // 7. Read 64-byte device name + codec/session metadata on Video socket #1
    //    scrcpy v2/v3: [4B codec_id][4B width][4B height]
    //    scrcpy v4/v5: [4B codec_id][4B 0x80000000 session marker][4B width][4B height]
    uint8_t devNameBuf[65] = {0};
    if (!recvAll(videoSock_, devNameBuf, 64)) {
        outError = "Failed reading device name from scrcpy-server.";
        disconnect();
        return false;
    }
    uint8_t codecMeta[12] = {0};
    if (!recvAll(videoSock_, codecMeta, 12)) {
        outError = "Failed reading codec metadata from scrcpy-server.";
        disconnect();
        return false;
    }

    uint32_t f1 = readU32BE(codecMeta + 4);
    uint32_t f2 = readU32BE(codecMeta + 8);
    uint32_t initW = f1;
    uint32_t initH = f2;
    isV4Proto_ = false;

    if (f1 == 0x80000000u) {
        // scrcpy v4/v5+ 12-byte session header: [0x80000000][width][height]
        uint8_t extraH[4] = {0};
        if (!recvAll(videoSock_, extraH, 4)) {
            outError = "Failed reading v4/v5 session height from scrcpy-server.";
            disconnect();
            return false;
        }
        initW = f2;
        initH = readU32BE(extraH);
        isV4Proto_ = true;
    }

    if (initW == 0 || initH == 0 || initW > 8192 || initH > 8192) {
        initW = 1920;
        initH = 1080;
    }

    EnterCriticalSection(&metaLock_);
    deviceName_ = reinterpret_cast<const char*>(devNameBuf);
    screenW_ = initW;
    screenH_ = initH;
    LeaveCriticalSection(&metaLock_);

    // 8. Spawn background reader threads (rotation tracking + drain control acks)
    InterlockedExchange(&stopThreads_, 0);
    InterlockedExchange(&connected_, 1);

    videoThread_ = CreateThread(NULL, 0, videoReaderProc, this, 0, NULL);
    controlThread_ = CreateThread(NULL, 0, controlReaderProc, this, 0, NULL);

    char logBuf[160];
    std::snprintf(logBuf, sizeof(logBuf), "[Bridge] Connected to '%s' (%ux%u native touch)",
                  deviceName_.c_str(), initW, initH);
    appendLog(logBuf);
    return true;
}

void ScrcpyBridge::disconnect() {
    InterlockedExchange(&connected_, 0);
    InterlockedExchange(&stopThreads_, 1);

    if (videoSock_ != INVALID_SOCKET) {
        shutdown(videoSock_, SD_BOTH);
        closesocket(videoSock_);
        videoSock_ = INVALID_SOCKET;
    }
    if (controlSock_ != INVALID_SOCKET) {
        shutdown(controlSock_, SD_BOTH);
        closesocket(controlSock_);
        controlSock_ = INVALID_SOCKET;
    }

    if (videoThread_) {
        WaitForSingleObject(videoThread_, 1000);
        CloseHandle(videoThread_);
        videoThread_ = NULL;
    }
    if (controlThread_) {
        WaitForSingleObject(controlThread_, 1000);
        CloseHandle(controlThread_);
        controlThread_ = NULL;
    }

    if (serverProc_) {
        TerminateProcess(serverProc_, 0);
        CloseHandle(serverProc_);
        serverProc_ = NULL;
    }
    stopMirrorWindow();

    if (!adbExe_.empty() && !scid_.empty()) {
        std::string adb = adbPrefix();
        char portBuf[16];
        std::snprintf(portBuf, sizeof(portBuf), "%d", forwardPort_);
        std::string dummyOut;
        DWORD ec = 0;
        runCommandCapture(adb + " forward --remove tcp:" + portBuf, dummyOut, ec);
        runCommandCapture(adb + " shell rm -f /data/local/tmp/km-scrcpy-server.jar", dummyOut, ec);
        scid_.clear();
    }
}

std::string ScrcpyBridge::deviceName() const {
    EnterCriticalSection(&metaLock_);
    std::string copy = deviceName_;
    LeaveCriticalSection(&metaLock_);
    return copy;
}

void ScrcpyBridge::getScreenSize(uint32_t& outW, uint32_t& outH) const {
    EnterCriticalSection(&metaLock_);
    outW = screenW_;
    outH = screenH_;
    LeaveCriticalSection(&metaLock_);
}

bool ScrcpyBridge::sendAll(SOCKET s, const uint8_t* data, int len) {
    int total = 0;
    while (total < len) {
        int n = send(s, reinterpret_cast<const char*>(data + total), len - total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

bool ScrcpyBridge::recvAll(SOCKET s, uint8_t* data, int len) {
    int total = 0;
    while (total < len) {
        int n = recv(s, reinterpret_cast<char*>(data + total), len - total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

bool ScrcpyBridge::sendTouchPixels(TouchAction action, uint64_t pointerId, int32_t px, int32_t py, bool clampToScreen) {
    if (!connected_) return false;

    uint32_t w = 0, h = 0;
    getScreenSize(w, h);
    if (w == 0 || h == 0) return false;

    if (clampToScreen) {
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        if (px >= static_cast<int32_t>(w)) px = static_cast<int32_t>(w) - 1;
        if (py >= static_cast<int32_t>(h)) py = static_cast<int32_t>(h) - 1;
    } else {
        // Keep within reasonable signed int32 bounds while allowing unbounded virtual aim travel
        if (px < -1000000) px = -1000000;
        if (py < -1000000) py = -1000000;
        if (px >  1000000) px =  1000000;
        if (py >  1000000) py =  1000000;
    }

    uint8_t msg[32];
    msg[0] = 2; // SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT
    msg[1] = static_cast<uint8_t>(action);
    writeU64BE(msg + 2, pointerId);
    writeU32BE(msg + 10, static_cast<uint32_t>(px));
    writeU32BE(msg + 14, static_cast<uint32_t>(py));
    writeU16BE(msg + 18, static_cast<uint16_t>(w));
    writeU16BE(msg + 20, static_cast<uint16_t>(h));
    uint16_t pressure = (action == TOUCH_UP) ? 0x0000u : 0xFFFFu;
    writeU16BE(msg + 22, pressure);
    writeU32BE(msg + 24, 0u); // action_button = 0 (multi-touch finger)
    writeU32BE(msg + 28, 0u); // buttons = 0

    EnterCriticalSection(&sendLock_);
    bool ok = (controlSock_ != INVALID_SOCKET) && sendAll(controlSock_, msg, sizeof(msg));
    LeaveCriticalSection(&sendLock_);

    if (!ok) {
        InterlockedExchange(&connected_, 0);
    }
    return ok;
}

bool ScrcpyBridge::sendTouchNorm(TouchAction action, uint64_t pointerId, float nx, float ny) {
    uint32_t w = 0, h = 0;
    getScreenSize(w, h);
    int32_t px = static_cast<int32_t>(nx * static_cast<float>(w) + 0.5f);
    int32_t py = static_cast<int32_t>(ny * static_cast<float>(h) + 0.5f);
    return sendTouchPixels(action, pointerId, px, py);
}

bool ScrcpyBridge::sendKeycode(uint8_t action, uint32_t androidKeycode) {
    if (!connected_) return false;
    // SC_CONTROL_MSG_TYPE_INJECT_KEYCODE (14 bytes):
    // [0] type=0, [1] action, [2..5] keycode, [6..9] repeat, [10..13] metastate
    uint8_t msg[14] = {0};
    msg[0] = 0;
    msg[1] = action;
    writeU32BE(msg + 2, androidKeycode);
    writeU32BE(msg + 6, 0);
    writeU32BE(msg + 10, 0);

    EnterCriticalSection(&sendLock_);
    bool ok = (controlSock_ != INVALID_SOCKET) && sendAll(controlSock_, msg, sizeof(msg));
    LeaveCriticalSection(&sendLock_);
    return ok;
}

void ScrcpyBridge::sendAndroidNav(uint32_t androidKeycode) {
    if (!connected_) return;
    sendKeycode(0 /* ACTION_DOWN */, androidKeycode);
    sendKeycode(1 /* ACTION_UP */, androidKeycode);
}

bool ScrcpyBridge::captureScreenshot(const std::string& savePath, std::string& outError) {
    if (adbExe_.empty()) {
        outError = "ADB not initialized.";
        return false;
    }
    std::vector<uint8_t> pngBytes;
    DWORD ec = 0;
    if (!runCommandCaptureBinary(adbPrefix() + " exec-out screencap -p", pngBytes, ec) ||
        ec != 0 || pngBytes.size() < 32) {
        outError = "Screenshot capture failed.";
        return false;
    }
    std::ofstream out(savePath.c_str(), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        outError = "Could not write " + savePath;
        return false;
    }
    out.write(reinterpret_cast<const char*>(pngBytes.data()), static_cast<std::streamsize>(pngBytes.size()));
    out.flush();
    if (!out.good()) {
        outError = "Write error saving screenshot.";
        return false;
    }
    appendLog("[Screenshot] Saved -> " + savePath);
    return true;
}

DWORD WINAPI ScrcpyBridge::videoReaderProc(LPVOID param) {
    static_cast<ScrcpyBridge*>(param)->runVideoReader();
    return 0;
}

DWORD WINAPI ScrcpyBridge::controlReaderProc(LPVOID param) {
    static_cast<ScrcpyBridge*>(param)->runControlReader();
    return 0;
}

void ScrcpyBridge::runVideoReader() {
    std::vector<uint8_t> payload;
    while (!stopThreads_) {
        uint8_t hdr[12];
        if (!recvAll(videoSock_, hdr, 12)) break;

        // scrcpy v4/v5+ emits a 12-byte session header on every screen rotation/reconfiguration
        if (isV4Proto_ && readU32BE(hdr) == 0x80000000u) {
            uint32_t newW = readU32BE(hdr + 4);
            uint32_t newH = readU32BE(hdr + 8);
            if (newW >= 64 && newH >= 64 && newW <= 8192 && newH <= 8192) {
                EnterCriticalSection(&metaLock_);
                bool changed = (newW != screenW_ || newH != screenH_);
                screenW_ = newW;
                screenH_ = newH;
                LeaveCriticalSection(&metaLock_);
                if (changed) {
                    std::printf("[Bridge] Screen rotation/resize (v4/v5 session): %ux%u\n", newW, newH);
                }
            }
            continue;
        }

        uint64_t ptsAndFlags = readU64BE(hdr);
        uint32_t pktSize = readU32BE(hdr + 8);
        if (pktSize > 16u * 1024u * 1024u) break; // sanity guard

        payload.resize(pktSize);
        if (pktSize > 0 && !recvAll(videoSock_, payload.data(), static_cast<int>(pktSize))) {
            break;
        }

        // Bit 63 (v2/v3) or Bit 62 (v4/v5) = SC_PACKET_FLAG_CONFIG (SPS/PPS)
        const bool isConfig = (ptsAndFlags & (3ULL << 62)) != 0;
        if (isConfig && pktSize > 4) {
            uint32_t newW = 0, newH = 0;
            if (SpsParser::parseH264Resolution(payload.data(), payload.size(), newW, newH)) {
                EnterCriticalSection(&metaLock_);
                bool changed = (newW != screenW_ || newH != screenH_);
                screenW_ = newW;
                screenH_ = newH;
                LeaveCriticalSection(&metaLock_);
                if (changed) {
                    std::printf("[Bridge] Screen rotation/resize (SPS): %ux%u\n", newW, newH);
                }
            }
        }
    }
    InterlockedExchange(&connected_, 0);
}

void ScrcpyBridge::runControlReader() {
    uint8_t buf[512];
    while (!stopThreads_) {
        int n = recv(controlSock_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n <= 0) break;
    }
}
