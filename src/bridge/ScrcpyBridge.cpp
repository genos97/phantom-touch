#include "bridge/ScrcpyBridge.hpp"
#include "bridge/SpsParser.hpp"
#include "engine/ThreadScheduling.hpp"
#include "video/LatencyTelemetry.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

std::string ControlPacketQueue::truncateUtf8Cap(const std::string& utf8, size_t maxBytes) {
    if (utf8.size() <= maxBytes) return utf8;
    if (maxBytes == 0) return "";
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(utf8[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    return utf8.substr(0, cut);
}

void ControlPacketQueue::push(const std::vector<uint8_t>& pkt, bool highPrio) {
    if (highPrio) hi_.push_back(pkt);
    else          lo_.push_back(pkt);
}

void ControlPacketQueue::push(std::vector<uint8_t>&& pkt, bool highPrio) {
    if (highPrio) hi_.push_back(std::move(pkt));
    else          lo_.push_back(std::move(pkt));
}

bool ControlPacketQueue::popNext(std::vector<uint8_t>& outPkt) {
    if (!hi_.empty()) {
        outPkt = std::move(hi_.front());
        hi_.pop_front();
        return true;
    }
    if (!lo_.empty()) {
        outPkt = std::move(lo_.front());
        lo_.pop_front();
        return true;
    }
    return false;
}

void ControlPacketQueue::clear() {
    hi_.clear();
    lo_.clear();
}

uint64_t ClipboardSyncGuard::hashText(const std::string& utf8) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < utf8.size(); ++i) {
        h ^= static_cast<unsigned char>(utf8[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

void ClipboardSyncGuard::reset() {
    lastWinSeq_ = 0;
    hasWinSeq_ = false;
    lastSentToPhoneHash_ = 0;
    lastRecvFromPhoneHash_ = 0;
    suppressedEchoHash_ = 0;
    suppressedEchoUntilMs_ = 0;
    restorePending_ = false;
    restoreDueMs_ = 0;
    savedClip_.clear();
}

bool ClipboardSyncGuard::shouldSendPcToPhone(const std::string& utf8, uint32_t winSeq) {
    if (utf8.empty()) return false;
    if (hasWinSeq_ && winSeq == lastWinSeq_) return false;
    const uint64_t h = hashText(utf8);
    if (h == lastRecvFromPhoneHash_ || h == lastSentToPhoneHash_) {
        lastWinSeq_ = winSeq;
        hasWinSeq_ = true;
        return false;
    }
    lastWinSeq_ = winSeq;
    hasWinSeq_ = true;
    lastSentToPhoneHash_ = h;
    return true;
}

bool ClipboardSyncGuard::shouldApplyPhoneToPc(const std::string& utf8, uint64_t nowMs) {
    if (utf8.empty()) return false;
    const uint64_t h = hashText(utf8);
    if (h == lastSentToPhoneHash_) return false;
    if (nowMs <= suppressedEchoUntilMs_ && h == suppressedEchoHash_) return false;
    lastRecvFromPhoneHash_ = h;
    return true;
}

void ClipboardSyncGuard::noteLocalClipboardWrite(uint32_t newWinSeq, const std::string& utf8) {
    lastWinSeq_ = newWinSeq;
    hasWinSeq_ = true;
    lastRecvFromPhoneHash_ = hashText(utf8);
}

void ClipboardSyncGuard::beginNonAsciiTyping(const std::string& typedUtf8,
                                               const std::string& clipToPreserve,
                                               uint64_t nowMs) {
    if (!restorePending_) {
        savedClip_ = clipToPreserve;
        restorePending_ = true;
    }
    const uint64_t th = hashText(typedUtf8);
    suppressedEchoHash_ = th;
    suppressedEchoUntilMs_ = nowMs + 1500ULL;
    restoreDueMs_ = nowMs + 250ULL;
}

bool ClipboardSyncGuard::pollClipboardRestore(uint64_t nowMs, std::string& outRestoreText) {
    if (!restorePending_ || nowMs < restoreDueMs_) return false;
    restorePending_ = false;
    outRestoreText = savedClip_;
    savedClip_.clear();
    const uint64_t rh = hashText(outRestoreText);
    suppressedEchoHash_ = rh;
    suppressedEchoUntilMs_ = nowMs + 1500ULL;
    lastSentToPhoneHash_ = rh;
    return true;
}

namespace {

std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), NULL, 0);
    if (wlen <= 0) return std::wstring();
    std::wstring ws(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &ws[0], wlen);
    return ws;
}

std::string wideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), NULL, 0, NULL, NULL);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), &out[0], len, NULL, NULL);
    return out;
}

bool fileExists(const std::string& path) {
    std::wstring wp = utf8ToWide(path);
    if (wp.empty()) return false;
    DWORD attr = GetFileAttributesW(wp.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

std::string getExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::string s = wideToUtf8(buf);
    size_t pos = s.find_last_of("\\/");
    return (pos == std::string::npos) ? "." : s.substr(0, pos);
}

std::string findInPath(const std::string& filename) {
    wchar_t buf[MAX_PATH] = {0};
    std::wstring wname = utf8ToWide(filename);
    if (wname.empty()) return "";
    DWORD n = SearchPathW(NULL, wname.c_str(), NULL, MAX_PATH, buf, NULL);
    if (n > 0 && n < MAX_PATH) {
        return wideToUtf8(buf);
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
    // Expected first line: "scrcpy 5.0.1 <https://...>"
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

bool spawnProcessWithPipeCapture(CRITICAL_SECTION& procSpawnLock,
                                 const std::string& cmdLine,
                                 HANDLE writePipe,
                                 bool redirectStderr,
                                 PROCESS_INFORMATION& outPi) {
    std::wstring wcmd = utf8ToWide(cmdLine);
    if (wcmd.empty()) return false;
    std::vector<wchar_t> mutableCmd(wcmd.begin(), wcmd.end());
    mutableCmd.push_back(L'\0');

    EnterCriticalSection(&procSpawnLock);

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    // Must outlive CreateProcessW: UpdateProcThreadAttribute stores the pointer to inheritList, not a copy!
    HANDLE inheritList[1] = { writePipe };
    bool hasAttrList = false;
    if (attrSize > 0 && InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize)) {
        if (UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      inheritList, sizeof(inheritList), NULL, NULL)) {
            hasAttrList = true;
        } else {
            DeleteProcThreadAttributeList(attrList);
        }
    }

    STARTUPINFOEXW siex;
    ZeroMemory(&siex, sizeof(siex));
    siex.StartupInfo.cb = hasAttrList ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFOW);
    siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    siex.StartupInfo.wShowWindow = SW_HIDE;
    siex.StartupInfo.hStdOutput = writePipe;
    siex.StartupInfo.hStdError  = redirectStderr ? writePipe : NULL;
    siex.lpAttributeList = hasAttrList ? attrList : NULL;

    DWORD flags = CREATE_NO_WINDOW | (hasAttrList ? EXTENDED_STARTUPINFO_PRESENT : 0u);
    ZeroMemory(&outPi, sizeof(outPi));
    BOOL ok = CreateProcessW(NULL, mutableCmd.data(), NULL, NULL, TRUE,
                             flags, NULL, NULL, &siex.StartupInfo, &outPi);

    if (hasAttrList) {
        DeleteProcThreadAttributeList(attrList);
    }
    LeaveCriticalSection(&procSpawnLock);
    return ok != FALSE;
}

bool readPipeWithTimeout(HANDLE readPipe, HANDLE procHandle, DWORD timeoutMs,
                         std::vector<uint8_t>& outBytes, DWORD& outExitCode) {
    outBytes.clear();
    outExitCode = 1;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    uint8_t buf[4096];

    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(readPipe, NULL, 0, NULL, &avail, NULL)) {
            break;
        }
        if (avail > 0) {
            DWORD toRead = (avail > sizeof(buf)) ? static_cast<DWORD>(sizeof(buf)) : avail;
            DWORD bytesRead = 0;
            if (ReadFile(readPipe, buf, toRead, &bytesRead, NULL) && bytesRead > 0) {
                outBytes.insert(outBytes.end(), buf, buf + bytesRead);
            } else {
                break;
            }
            continue;
        }

        if (WaitForSingleObject(procHandle, 20) == WAIT_OBJECT_0) {
            // Drain any trailing bytes written before process exit
            while (PeekNamedPipe(readPipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                DWORD toRead = (avail > sizeof(buf)) ? static_cast<DWORD>(sizeof(buf)) : avail;
                DWORD bytesRead = 0;
                if (ReadFile(readPipe, buf, toRead, &bytesRead, NULL) && bytesRead > 0) {
                    outBytes.insert(outBytes.end(), buf, buf + bytesRead);
                } else {
                    break;
                }
            }
            GetExitCodeProcess(procHandle, &outExitCode);
            return true;
        }

        if (GetTickCount64() >= deadline) {
            TerminateProcess(procHandle, 1);
            WaitForSingleObject(procHandle, 500);
            outExitCode = STILL_ACTIVE;
            return false;
        }
    }

    if (WaitForSingleObject(procHandle, 500) == WAIT_OBJECT_0) {
        GetExitCodeProcess(procHandle, &outExitCode);
        return true;
    }
    TerminateProcess(procHandle, 1);
    outExitCode = STILL_ACTIVE;
    return false;
}

} // namespace

ScrcpyBridge::ScrcpyBridge() {
    InitializeCriticalSection(&sendLock_);
    InitializeCriticalSection(&metaLock_);
    InitializeCriticalSection(&procSpawnLock_);
    controlSendEvent_ = CreateEventW(NULL, FALSE, FALSE, NULL);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        wsaInit_ = true;
    }
}

ScrcpyBridge::~ScrcpyBridge() {
    disconnect();
    if (controlSendEvent_) {
        CloseHandle(controlSendEvent_);
        controlSendEvent_ = NULL;
    }
    if (wsaInit_) {
        WSACleanup();
    }
    DeleteCriticalSection(&procSpawnLock_);
    DeleteCriticalSection(&metaLock_);
    DeleteCriticalSection(&sendLock_);
}

bool ScrcpyBridge::runCommandCapture(const std::string& cmdLine, std::string& outText,
                                     DWORD& outExitCode, DWORD timeoutMs) {
    outText.clear();
    outExitCode = 1;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = NULL, writePipe = NULL;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    PROCESS_INFORMATION pi;
    bool ok = spawnProcessWithPipeCapture(procSpawnLock_, cmdLine, writePipe, true, pi);
    CloseHandle(writePipe);

    if (!ok) {
        CloseHandle(readPipe);
        return false;
    }

    std::vector<uint8_t> bytes;
    bool finished = readPipeWithTimeout(readPipe, pi.hProcess, timeoutMs, bytes, outExitCode);
    if (!bytes.empty()) {
        outText.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    CloseHandle(readPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return finished;
}

bool ScrcpyBridge::runCommandCaptureBinary(const std::string& cmdLine, std::vector<uint8_t>& outBytes,
                                           DWORD& outExitCode, DWORD timeoutMs) {
    outBytes.clear();
    outExitCode = 1;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = NULL, writePipe = NULL;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    PROCESS_INFORMATION pi;
    bool ok = spawnProcessWithPipeCapture(procSpawnLock_, cmdLine, writePipe, false, pi);
    CloseHandle(writePipe);

    if (!ok) {
        CloseHandle(readPipe);
        return false;
    }

    bool finished = readPipeWithTimeout(readPipe, pi.hProcess, timeoutMs, outBytes, outExitCode);

    CloseHandle(readPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return finished;
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
    EnterCriticalSection(&metaLock_);
    std::string exe = adbExe_;
    std::string ser = activeSerial_;
    LeaveCriticalSection(&metaLock_);
    if (ser.empty()) {
        return "\"" + exe + "\"";
    }
    return "\"" + exe + "\" -s \"" + ser + "\"";
}

bool ScrcpyBridge::runAdbShell(const std::string& shellArgs, std::string& outText, DWORD timeoutMs) {
    outText.clear();
    EnterCriticalSection(&metaLock_);
    std::string exe = adbExe_;
    LeaveCriticalSection(&metaLock_);
    if (exe.empty() || shellArgs.empty()) return false;
    DWORD ec = 0;
    return runCommandCapture(adbPrefix() + " shell " + shellArgs, outText, ec, timeoutMs) && ec == 0;
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

int ScrcpyBridge::parseMajorVersion(const std::string& versionStr) {
    int major = 0;
    bool any = false;
    for (size_t i = 0; i < versionStr.size(); ++i) {
        char c = versionStr[i];
        if (c >= '0' && c <= '9') {
            major = major * 10 + (c - '0');
            any = true;
        } else {
            break;
        }
    }
    return any ? major : 0;
}

bool ScrcpyBridge::parseV4SessionHeader(const uint8_t hdr[12], uint32_t& outW, uint32_t& outH, bool& outClientResized) {
    if ((hdr[0] & 0x80u) == 0) return false;
    outClientResized = ((hdr[3] & 0x01u) != 0);
    outW = readU32BE(hdr + 4);
    outH = readU32BE(hdr + 8);
    return (outW >= 64 && outH >= 64 && outW <= 8192 && outH <= 8192);
}

size_t ScrcpyBridge::serializeResizeDisplay(uint8_t* buf, uint16_t width, uint16_t height) {
    uint16_t ew = static_cast<uint16_t>(width & ~1u);
    uint16_t eh = static_cast<uint16_t>(height & ~1u);
    if (ew < 160) ew = 160;
    if (eh < 160) eh = 160;
    buf[0] = 21; // SC_CONTROL_MSG_TYPE_RESIZE_DISPLAY
    writeU16BE(buf + 1, ew);
    writeU16BE(buf + 3, eh);
    return 5;
}

size_t ScrcpyBridge::serializeUhidCreate(uint8_t* buf, size_t cap,
                                         uint16_t id, uint16_t vendorId, uint16_t productId,
                                         const std::string& name,
                                         const uint8_t* reportDesc, uint16_t reportDescSize) {
    if (!buf || (reportDescSize > 0 && !reportDesc)) return 0;
    size_t nameLen = name.size();
    if (nameLen > 127) {
        nameLen = 127;
        while (nameLen > 0 && (static_cast<unsigned char>(name[nameLen]) & 0xC0u) == 0x80u) {
            --nameLen;
        }
    }
    const size_t total = 10 + nameLen + static_cast<size_t>(reportDescSize);
    if (cap < total) return 0;

    buf[0] = 12; // SC_CONTROL_MSG_TYPE_UHID_CREATE
    writeU16BE(buf + 1, id);
    writeU16BE(buf + 3, vendorId);
    writeU16BE(buf + 5, productId);
    buf[7] = static_cast<uint8_t>(nameLen);
    if (nameLen > 0) std::memcpy(buf + 8, name.data(), nameLen);
    writeU16BE(buf + 8 + nameLen, reportDescSize);
    if (reportDescSize > 0) std::memcpy(buf + 10 + nameLen, reportDesc, reportDescSize);
    return total;
}

size_t ScrcpyBridge::serializeUhidInput(uint8_t* buf, size_t cap,
                                        uint16_t id, const uint8_t* data, uint16_t size) {
    if (!buf || (size > 0 && !data)) return 0;
    const size_t total = 5 + static_cast<size_t>(size);
    if (cap < total) return 0;
    buf[0] = 13; // SC_CONTROL_MSG_TYPE_UHID_INPUT
    writeU16BE(buf + 1, id);
    writeU16BE(buf + 3, size);
    if (size > 0) std::memcpy(buf + 5, data, size);
    return total;
}

size_t ScrcpyBridge::serializeUhidDestroy(uint8_t* buf, size_t cap, uint16_t id) {
    if (!buf || cap < 3) return 0;
    buf[0] = 14; // SC_CONTROL_MSG_TYPE_UHID_DESTROY
    writeU16BE(buf + 1, id);
    return 3;
}

bool ScrcpyBridge::isPureAscii(const std::string& utf8) {
    for (size_t i = 0; i < utf8.size(); ++i) {
        if (static_cast<unsigned char>(utf8[i]) >= 0x80u) return false;
    }
    return true;
}

std::vector<std::string> ScrcpyBridge::splitUtf8Chunks(const std::string& utf8, size_t maxBytes) {
    std::vector<std::string> chunks;
    if (utf8.empty()) return chunks;
    if (maxBytes < 4) maxBytes = 4;
    size_t off = 0;
    while (off < utf8.size()) {
        size_t end = off + maxBytes;
        if (end >= utf8.size()) {
            end = utf8.size();
        } else {
            // Never slice inside a multi-byte UTF-8 sequence (continuation bytes have bits 10xxxxxx)
            while (end > off && (static_cast<unsigned char>(utf8[end]) & 0xC0u) == 0x80u) {
                --end;
            }
            if (end == off) end = off + maxBytes; // Malformed input safety
        }
        chunks.push_back(utf8.substr(off, end - off));
        off = end;
    }
    return chunks;
}

std::string ScrcpyBridge::sanitizeServerOption(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size() && out.size() < 256; ++i) {
        unsigned char uc = static_cast<unsigned char>(raw[i]);
        bool ok = (uc >= 'A' && uc <= 'Z') ||
                  (uc >= 'a' && uc <= 'z') ||
                  (uc >= '0' && uc <= '9') ||
                  uc == '_' || uc == '=' || uc == ':' || uc == ',' || uc == '.' || uc == '-';
        if (ok) out.push_back(static_cast<char>(uc));
    }
    return out;
}

std::vector<VideoEncoderInfo> ScrcpyBridge::parseH264Encoders(const std::string& raw) {
    std::vector<VideoEncoderInfo> out;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("--video-codec=h264") == std::string::npos) continue;
        size_t encPos = line.find("--video-encoder=");
        if (encPos == std::string::npos) continue;
        encPos += 16;
        size_t encEnd = encPos;
        while (encEnd < line.size() &&
               line[encEnd] != ' ' && line[encEnd] != '\t' &&
               line[encEnd] != '\r' && line[encEnd] != '\n') {
            ++encEnd;
        }
        std::string name = sanitizeServerOption(line.substr(encPos, encEnd - encPos));
        if (name.empty()) continue;

        bool isHw = (line.find("(hw)") != std::string::npos) &&
                    (line.find("(sw)") == std::string::npos);
        if (name.find("c2.android.") == 0 || name.find("OMX.google.") == 0) {
            isHw = false;
        }
        bool dup = false;
        for (size_t i = 0; i < out.size(); ++i) {
            if (out[i].name == name) { dup = true; break; }
        }
        if (!dup) {
            VideoEncoderInfo info;
            info.codec = "h264";
            info.name = name;
            info.isHardware = isHw;
            out.push_back(info);
        }
    }
    return out;
}

std::string ScrcpyBridge::pickBestHardwareH264Encoder(const std::vector<VideoEncoderInfo>& encoders) {
    int bestRank = -1;
    std::string bestName;
    for (size_t i = 0; i < encoders.size(); ++i) {
        if (!encoders[i].isHardware || encoders[i].name.empty()) continue;
        int rank = 1;
        if (encoders[i].name.find("c2.qti.") == 0 ||
            encoders[i].name.find("c2.exynos.") == 0 ||
            encoders[i].name.find("c2.mtk.") == 0) {
            rank = 3;
        } else if (encoders[i].name.find("c2.") == 0) {
            rank = 2;
        }
        if (rank > bestRank) {
            bestRank = rank;
            bestName = encoders[i].name;
        }
    }
    return bestName;
}

PhoneThermalInfo ScrcpyBridge::parseThermalTelemetry(const std::string& combinedRaw) {
    PhoneThermalInfo info;
    std::istringstream iss(combinedRaw);
    std::string line;
    bool inThermal = false;
    bool gotLevel = false;
    bool gotTemp = false;

    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.find("---THERMAL---") != std::string::npos ||
            line.find("Thermal Status:") != std::string::npos ||
            line.find("IsStatusOverride:") != std::string::npos) {
            inThermal = true;
        }
        if (!inThermal) {
            if (line.find("ACTION_BATTERY_CHANGED") != std::string::npos) continue;
            size_t lp = line.find("level:");
            if (!gotLevel && lp != std::string::npos && line.find("Capacity") == std::string::npos) {
                info.batteryLevel = std::atoi(line.c_str() + lp + 6);
                gotLevel = true;
            }
            size_t tp = line.find("temperature:");
            if (!gotTemp && tp != std::string::npos) {
                int tenths = std::atoi(line.c_str() + tp + 12);
                info.batteryTempC = static_cast<float>(tenths) * 0.1f;
                gotTemp = true;
            }
        } else {
            size_t sp = line.find("Thermal Status:");
            if (sp != std::string::npos) {
                info.thermalStatus = std::atoi(line.c_str() + sp + 15);
                if (info.thermalStatus < 0) info.thermalStatus = 0;
                if (info.thermalStatus > 6) info.thermalStatus = 6;
            }
        }
    }

    static const char* kLabels[7] = {
        "OK", "LIGHT", "MODERATE", "SEVERE", "CRITICAL", "EMERGENCY", "SHUTDOWN"
    };
    info.thermalLabel = kLabels[info.thermalStatus];
    info.valid = (gotTemp || gotLevel);
    return info;
}

std::string ScrcpyBridge::selectAudioCodec(const std::string& deviceSerial) {
    return (deviceSerial.find(':') != std::string::npos) ? "aac" : "raw";
}

AudioCodecId ScrcpyBridge::parseAudioCodecId(uint32_t rawBeId) {
    if (rawBeId == 0x00726177u) return AUDIO_CODEC_RAW; // "raw"
    if (rawBeId == 0x00616163u) return AUDIO_CODEC_AAC; // "aac"
    return AUDIO_CODEC_NONE;
}

bool ScrcpyBridge::parseAudioPacketHeader(const uint8_t hdr[12], bool isV4Proto,
                                          uint64_t& outPtsUs, uint32_t& outSize, bool& outIsConfig) {
    if (!hdr) return false;
    const uint64_t ptsAndFlags = readU64BE(hdr);
    const uint32_t sz = readU32BE(hdr + 8);
    outIsConfig = isV4Proto ? ((ptsAndFlags & (1ULL << 62)) != 0)
                            : ((ptsAndFlags & (1ULL << 63)) != 0);
    outPtsUs = isV4Proto ? (ptsAndFlags & 0x1FFFFFFFFFFFFFFFULL)
                         : (ptsAndFlags & 0x3FFFFFFFFFFFFFFFULL);
    outSize = sz;
    return (sz > 0 && sz <= 1024u * 1024u);
}

bool ScrcpyBridge::listH264Encoders(const ToolPaths& tools, const std::string& deviceSerial,
                                      std::vector<VideoEncoderInfo>& outEncoders) {
    outEncoders.clear();
    if (tools.scrcpyExe.empty()) return false;
    std::string cmd = "\"" + tools.scrcpyExe + "\"";
    if (!deviceSerial.empty()) {
        cmd += " -s \"" + deviceSerial + "\"";
    }
    cmd += " --list-encoders";
    std::string raw;
    DWORD ec = 0;
    runCommandCapture(cmd, raw, ec, 8000);
    outEncoders = parseH264Encoders(raw);
    return !outEncoders.empty();
}

std::vector<DeviceAppInfo> ScrcpyBridge::parseAppListOutput(const std::string& raw) {
    // Format from scrcpy v5.0.1 LogUtils.java:248-295 (`list_apps=true`):
    //   [server] INFO: List of apps:
    //    * Maps                           com.google.android.apps.maps
    //    * Live Transcribe & Sound Notifications
    //                                     com.google.audio.hearing.visualization.accessibility.scribe
    //    - WhatsApp                       com.whatsapp
    std::vector<DeviceAppInfo> out;
    const bool hasHeader = (raw.find("List of apps:") != std::string::npos);
    bool inList = !hasHeader;
    bool hasPending = false;
    DeviceAppInfo pending;

    auto trimSpaces = [](const std::string& s) -> std::string {
        size_t a = 0;
        while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
        size_t b = s.size();
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
        return s.substr(a, b - a);
    };

    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!inList) {
            if (line.find("List of apps:") != std::string::npos) inList = true;
            continue;
        }
        if (line.size() >= 3 && line[0] == ' ' && (line[1] == '*' || line[1] == '-') && line[2] == ' ') {
            hasPending = false;
            const bool isSys = (line[1] == '*');
            std::string body = line.substr(3);
            while (!body.empty() && (body.back() == ' ' || body.back() == '\t')) body.pop_back();
            if (body.empty()) continue;

            // Find trailing non-whitespace package token separated by >= 2 spaces
            size_t tokStart = body.size();
            while (tokStart > 0 && body[tokStart - 1] != ' ' && body[tokStart - 1] != '\t') {
                --tokStart;
            }
            std::string tail = body.substr(tokStart);
            size_t gapCount = 0;
            size_t gapPos = tokStart;
            while (gapPos > 0 && (body[gapPos - 1] == ' ' || body[gapPos - 1] == '\t')) {
                --gapPos;
                ++gapCount;
            }
            if (gapCount >= 2 && gapPos > 0 && tail.find('.') != std::string::npos) {
                DeviceAppInfo app;
                app.name = trimSpaces(body.substr(0, gapPos));
                app.packageName = tail;
                app.isSystem = isSys;
                if (!app.name.empty() && !app.packageName.empty()) {
                    out.push_back(app);
                }
            } else {
                pending.name = trimSpaces(body);
                pending.packageName.clear();
                pending.isSystem = isSys;
                hasPending = !pending.name.empty();
            }
        } else if (hasPending) {
            size_t lead = 0;
            while (lead < line.size() && line[lead] == ' ') ++lead;
            std::string pkg = trimSpaces(line);
            if (lead >= 4 && !pkg.empty() && pkg.find(' ') == std::string::npos && pkg.find('.') != std::string::npos) {
                pending.packageName = pkg;
                out.push_back(pending);
            }
            hasPending = false;
        }
    }
    return out;
}

bool ScrcpyBridge::listInstalledApps(const ToolPaths& tools, const std::string& deviceSerial,
                                       std::vector<DeviceAppInfo>& outApps) {
    outApps.clear();
    const std::string adbPath = tools.adbExe.empty() ? adbExe_ : tools.adbExe;
    if (adbPath.empty() || tools.scrcpyServer.empty() || tools.scrcpyVersion.empty()) return false;

    std::string ser = deviceSerial;
    if (ser.empty()) ser = activeSerial();
    const std::string prefix = ser.empty()
        ? ("\"" + adbPath + "\"")
        : ("\"" + adbPath + "\" -s \"" + ser + "\"");

    const std::string remoteJar = "/data/local/tmp/km-scrcpy-listapps.jar";
    std::string raw;
    DWORD ec = 0;
    if (!runCommandCapture(prefix + " push \"" + tools.scrcpyServer + "\" " + remoteJar, raw, ec, 15000) || ec != 0) {
        return false;
    }

    std::string srvOut;
    bool ran = runCommandCapture(prefix + " shell \"CLASSPATH=" + remoteJar +
                                 " app_process / com.genymobile.scrcpy.Server " +
                                 tools.scrcpyVersion + " list_apps=true\"",
                                 srvOut, ec, 25000);
    std::string cleanupOut;
    DWORD cleanEc = 0;
    runCommandCapture(prefix + " shell \"rm -f " + remoteJar + "\"", cleanupOut, cleanEc, 5000);

    outApps = parseAppListOutput(srvOut);
    char lbuf[64];
    std::snprintf(lbuf, sizeof(lbuf), "[Apps] Loaded %zu installed Android apps", outApps.size());
    appendLog(lbuf);
    return ran && !outApps.empty();
}

size_t ScrcpyBridge::serializeScanFile(uint8_t* buf, size_t cap, const std::string& remotePathUtf8) {
    if (!buf || remotePathUtf8.empty()) return 0;
    // SC_CONTROL_MSG_TYPE_SCAN_FILE (type=22 in scrcpy v5.0.1 control_msg.c:192-196):
    //   [22:1][len:4 BE][utf8 path <= 256 bytes]
    std::string capped = ControlPacketQueue::truncateUtf8Cap(remotePathUtf8, 256);
    if (capped.empty()) return 0;
    const size_t total = 5 + capped.size();
    if (cap < total) return 0;
    buf[0] = 22;
    writeU32BE(buf + 1, static_cast<uint32_t>(capped.size()));
    std::memcpy(buf + 5, capped.data(), capped.size());
    return total;
}

std::string ScrcpyBridge::shellSingleQuote(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 4);
    out.push_back('\'');
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\'') out += "'\\''";
        else out.push_back(raw[i]);
    }
    out.push_back('\'');
    return out;
}

std::vector<RemoteFileEntry> ScrcpyBridge::parseLsLaOutput(const std::string& raw, const std::string& baseDir) {
    std::string normBase = baseDir.empty() ? std::string("/sdcard") : baseDir;
    if (normBase[0] != '/') normBase.insert(normBase.begin(), '/');
    while (normBase.size() > 1 && normBase.back() == '/') normBase.pop_back();

    auto isDateTok = [](const std::string& s) -> bool {
        if (s.size() != 10) return false;
        if (s[4] != '-' || s[7] != '-') return false;
        for (size_t i = 0; i < 10; ++i) {
            if (i == 4 || i == 7) continue;
            if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
        }
        return true;
    };

    auto parseUintTok = [](const std::string& s, uint64_t& outVal) -> bool {
        if (s.empty()) return false;
        uint64_t v = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char ch = static_cast<unsigned char>(s[i]);
            if (!std::isdigit(ch)) return false;
            v = v * 10u + static_cast<uint64_t>(ch - '0');
        }
        outVal = v;
        return true;
    };

    std::vector<RemoteFileEntry> out;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (line.compare(0, 6, "total ") == 0 || line.compare(0, 3, "ls:") == 0) continue;
        const char mode0 = line[0];
        if (mode0 != 'd' && mode0 != '-' && mode0 != 'l') continue;

        struct TokSpan { size_t start; size_t end; std::string text; };
        std::vector<TokSpan> toks;
        size_t pos = 0;
        while (pos < line.size() && toks.size() < 12) {
            while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
            if (pos >= line.size()) break;
            size_t end = pos;
            while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
            toks.push_back(TokSpan{ pos, end, line.substr(pos, end - pos) });
            pos = end;
        }
        if (toks.size() < 6) continue;

        int dateIdx = -1;
        for (size_t ti = 2; ti + 2 < toks.size(); ++ti) {
            if (isDateTok(toks[ti].text)) {
                dateIdx = static_cast<int>(ti);
                break;
            }
        }

        size_t nameOff = std::string::npos;
        uint64_t sz = 0;
        std::string mod;
        if (dateIdx >= 0 && static_cast<size_t>(dateIdx + 1) < toks.size()) {
            const int timeIdx = dateIdx + 1;
            if (dateIdx >= 1) parseUintTok(toks[dateIdx - 1].text, sz);
            mod = toks[dateIdx].text + " " + toks[timeIdx].text;
            nameOff = toks[timeIdx].end;
            while (nameOff < line.size() && (line[nameOff] == ' ' || line[nameOff] == '\t')) ++nameOff;
        } else if (toks.size() >= 8) {
            parseUintTok(toks[4].text, sz);
            nameOff = toks[6].end;
            while (nameOff < line.size() && (line[nameOff] == ' ' || line[nameOff] == '\t')) ++nameOff;
        }
        if (nameOff == std::string::npos || nameOff >= line.size()) continue;

        std::string name = line.substr(nameOff);
        if (mode0 == 'l') {
            size_t arrow = name.find(" -> ");
            if (arrow != std::string::npos) name = name.substr(0, arrow);
        }
        while (!name.empty() && (name.back() == '\r' || name.back() == '\n')) name.pop_back();
        if (name.empty() || name == "." || name == "..") continue;

        RemoteFileEntry e;
        e.name = name;
        e.fullPath = (normBase == "/") ? ("/" + name) : (normBase + "/" + name);
        e.isDir = (mode0 == 'd');
        e.isSymlink = (mode0 == 'l');
        e.sizeBytes = e.isDir ? 0u : sz;
        e.modified = mod;
        out.push_back(e);
    }

    auto ciLess = [](const std::string& a, const std::string& b) -> bool {
        const size_t n = (a.size() < b.size()) ? a.size() : b.size();
        for (size_t i = 0; i < n; ++i) {
            unsigned char ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
            unsigned char cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
            if (ca != cb) return ca < cb;
        }
        return a.size() < b.size();
    };
    std::sort(out.begin(), out.end(), [&](const RemoteFileEntry& a, const RemoteFileEntry& b) {
        bool aFolder = (a.isDir || a.isSymlink);
        bool bFolder = (b.isDir || b.isSymlink);
        if (aFolder != bFolder) return aFolder;
        return ciLess(a.name, b.name);
    });
    return out;
}

bool ScrcpyBridge::queryThermalTelemetry(PhoneThermalInfo& outInfo) {
    std::string raw;
    if (!runAdbShell("dumpsys battery | head -n 25; echo ---THERMAL---; dumpsys thermalservice | head -n 25",
                     raw, 4000)) {
        return false;
    }
    outInfo = parseThermalTelemetry(raw);
    return outInfo.valid;
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
        outError = "Could not detect scrcpy version from scrcpy.exe --version (expected v5.0.1).\n"
                   "Re-run setup_scrcpy.ps1 to install the pinned official scrcpy release.";
        return false;
    }
    adbExe_ = outPaths.adbExe;
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

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = borderless ? SW_SHOWNOACTIVATE : SW_SHOW;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::wstring wcmd = utf8ToWide(cmd);
    std::vector<wchar_t> mutableCmd(wcmd.begin(), wcmd.end());
    mutableCmd.push_back(L'\0');

    if (wcmd.empty() || !CreateProcessW(NULL, mutableCmd.data(), NULL, NULL, FALSE,
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

bool ScrcpyBridge::pairWireless(const ToolPaths& tools, const std::string& ipPort,
                                const std::string& pairCode, std::string& outMsg) {
    outMsg.clear();
    if (tools.adbExe.empty() || ipPort.empty() || pairCode.empty()) {
        outMsg = "Enter pairing IP:port and 6-digit code.";
        return false;
    }
    std::string raw;
    DWORD ec = 0;
    if (!runCommandCapture("\"" + tools.adbExe + "\" pair " + ipPort + " " + pairCode, raw, ec)) {
        outMsg = "Failed to launch adb pair.";
        return false;
    }
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
    outMsg = raw.empty() ? "ADB pairing finished." : raw;
    appendLog("[Pair] " + outMsg);
    return (ec == 0 && raw.find("Successfully paired") != std::string::npos);
}

std::vector<MdnsServiceInfo> ScrcpyBridge::parseMdnsServicesOutput(const std::string& raw) {
    // Format from `adb mdns services`:
    //   List of discovered mdns services
    //   studio-PhantomTouch    _adb-tls-pairing._tcp   192.168.1.42:37891
    //   adb-RZCW401ABCD-abc    _adb-tls-connect._tcp   192.168.1.42:41235
    std::vector<MdnsServiceInfo> out;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line.find("List of ") == 0) continue;
        std::istringstream ls(line);
        MdnsServiceInfo info;
        if (!(ls >> info.name >> info.type >> info.addr)) continue;
        if (info.addr.find(':') == std::string::npos) continue;
        out.push_back(info);
    }
    return out;
}

bool ScrcpyBridge::listMdnsServices(const ToolPaths& tools, std::vector<MdnsServiceInfo>& outServices) {
    outServices.clear();
    if (tools.adbExe.empty()) return false;
    std::string raw;
    DWORD ec = 0;
    if (!runCommandCapture("\"" + tools.adbExe + "\" mdns services", raw, ec)) {
        return false;
    }
    outServices = parseMdnsServicesOutput(raw);
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

bool ScrcpyBridge::reconnect(const ToolPaths& tools, const BridgeStreamConfig& cfg,
                             bool useNativeVideo, int controlMaxSize,
                             std::string& outError, const std::string& deviceSerial) {
    disconnect();
    if (useNativeVideo) {
        return connectSession(tools, cfg, outError, deviceSerial);
    }
    return connectControl(tools, controlMaxSize, outError, deviceSerial, cfg.stayAwake);
}

bool ScrcpyBridge::connectControl(const ToolPaths& tools, int controlMaxSize, std::string& outError,
                                  const std::string& deviceSerial, bool stayAwake) {
    BridgeStreamConfig cfg;
    cfg.maxSize = controlMaxSize;
    cfg.maxFps = 1;
    cfg.bitrateMbps = 0;
    cfg.metadataOnly = true;
    cfg.stayAwake = stayAwake;
    cfg.virtualDisplay = false;
    return connectSession(tools, cfg, outError, deviceSerial);
}

bool ScrcpyBridge::connectSession(const ToolPaths& tools, const BridgeStreamConfig& cfg,
                                  std::string& outError, const std::string& deviceSerial) {
    if (!wsaInit_) {
        outError = "Winsock initialization failed.";
        return false;
    }

    adbExe_ = tools.adbExe;
    virtualDisplayActive_ = cfg.virtualDisplay;
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

    // 2. Push scrcpy-server binary to a unique per-port path so cleanup=true and multi-phone tabs never collide!
    if (stepCb_) stepCb_(stepCbCtx_, 0);
    if (forwardPort_ < 1024 || forwardPort_ > 65535) forwardPort_ = 27199;
    char portBuf[16];
    std::snprintf(portBuf, sizeof(portBuf), "%d", forwardPort_);
    remoteServerJar_ = std::string("/data/local/tmp/km-scrcpy-server-") + portBuf + ".jar";

    std::string pushOut;
    DWORD exitCode = 0;
    if (!runCommandCapture(adb + " push \"" + tools.scrcpyServer +
                           "\" " + remoteServerJar_, pushOut, exitCode) || exitCode != 0) {
        outError = "Failed to push scrcpy-server to device:\n" + pushOut;
        return false;
    }

    // 3. Create unique 31-bit scid and adb forward tunnel for this session port
    std::srand(static_cast<unsigned int>(std::time(NULL) ^ GetTickCount() ^
                                         (static_cast<unsigned int>(forwardPort_) * 2654435761u) ^
                                         static_cast<unsigned int>(GetCurrentThreadId())));
    uint32_t scidVal = (static_cast<uint32_t>(std::rand()) << 16) ^
                       static_cast<uint32_t>(std::rand()) ^
                       (static_cast<uint32_t>(forwardPort_) * 2654435761u) ^
                       static_cast<uint32_t>(GetCurrentThreadId());
    scidVal &= 0x7FFFFFFFu;
    if (scidVal == 0) scidVal = 0x12345678u;

    char scidBuf[16];
    std::snprintf(scidBuf, sizeof(scidBuf), "%08x", scidVal);
    scid_ = scidBuf;

    std::string fwdOut;
    std::string fwdCmd = adb + " forward tcp:" + portBuf + " localabstract:scrcpy_" + scid_;
    if (!runCommandCapture(fwdCmd, fwdOut, exitCode) || exitCode != 0) {
        outError = "Failed to set up adb forward tunnel:\n" + fwdOut;
        return false;
    }

    // 4. Launch scrcpy-server (V10: uncapped FPS unless metadataOnly or explicit cap; B1: cleanup=true; B2/V5: virtual display)
    if (cfg.virtualDisplay) {
        std::string sdkOut;
        DWORD sdkEc = 0;
        if (runCommandCapture(adb + " shell getprop ro.build.version.sdk", sdkOut, sdkEc, 5000)) {
            int sdk = std::atoi(sdkOut.c_str());
            if (sdk > 0 && sdk < 29) {
                outError = "Second-Phone Virtual Display requires Android 10 (API 29) or newer.";
                return false;
            }
        }
    }

    std::string streamOpts;
    if (cfg.metadataOnly) {
        streamOpts = " max_fps=1 video_bit_rate=100000";
    } else {
        const uint32_t bps = (cfg.bitrateMbps > 0) ? static_cast<uint32_t>(cfg.bitrateMbps) * 1000000u : 16000000u;
        char bbuf[64];
        if (cfg.maxFps > 0) {
            std::snprintf(bbuf, sizeof(bbuf), " max_fps=%d video_bit_rate=%u", cfg.maxFps, bps);
        } else {
            std::snprintf(bbuf, sizeof(bbuf), " video_bit_rate=%u", bps);
        }
        streamOpts = bbuf;
    }

    const bool isV4OrNewer = (parseMajorVersion(tools.scrcpyVersion) >= 4);
    const bool wantAudio = (cfg.enableAudio && !cfg.metadataOnly);
    std::string aCodec = cfg.audioCodec.empty()
        ? selectAudioCodec(chosenSerial)
        : sanitizeServerOption(cfg.audioCodec);
    if (aCodec != "raw" && aCodec != "aac") {
        aCodec = selectAudioCodec(chosenSerial);
    }

    const std::string jarPath = !remoteServerJar_.empty()
        ? remoteServerJar_
        : "/data/local/tmp/km-scrcpy-server.jar";
    std::string srvCmd = adb + " shell CLASSPATH=" + jarPath + " "
                         "app_process / com.genymobile.scrcpy.Server " + tools.scrcpyVersion +
                         " scid=" + scid_ +
                         " log_level=info"
                         " video=true" +
                         (wantAudio ? (" audio=true audio_codec=" + aCodec) : std::string(" audio=false")) +
                         " control=true"
                         " video_codec=h264" +
                         streamOpts +
                         " tunnel_forward=true"
                         " send_dummy_byte=true"
                         " send_device_meta=true" +
                         std::string(isV4OrNewer ? "" : " send_codec_meta=true") +
                         " send_frame_meta=true"
                         " clipboard_autosync=" + std::string(cfg.clipboardAutosync ? "true" : "false") +
                         " power_on=true"
                         " stay_awake=" + std::string(cfg.stayAwake ? "true" : "false") +
                         " cleanup=true" +
                         buildShowTouchesServerArg(cfg.showTouches, true);
    if (cfg.maxSize > 0) {
        char szBuf[32];
        std::snprintf(szBuf, sizeof(szBuf), " max_size=%d", cfg.maxSize);
        srvCmd += szBuf;
    }
    if (!cfg.metadataOnly && !cfg.videoEncoder.empty()) {
        std::string safeEnc = sanitizeServerOption(cfg.videoEncoder);
        if (!safeEnc.empty()) {
            srvCmd += " video_encoder=" + safeEnc;
        }
    }
    if (!cfg.metadataOnly && !cfg.videoCodecOptions.empty()) {
        std::string safeOpts = sanitizeServerOption(cfg.videoCodecOptions);
        if (!safeOpts.empty()) {
            srvCmd += " video_codec_options=" + safeOpts;
        }
    }
    if (cfg.virtualDisplay) {
        // Stage 3 "Second Phone" Virtual Display Mode (keeps physical phone screen free!)
        // B2: display_ime_policy=local keep_active=true; V5: omit flex_display on "auto" so it uses device display size & DPI
        if (cfg.virtualDisplaySpec.empty() || cfg.virtualDisplaySpec == "auto") {
            srvCmd += " new_display= display_ime_policy=local keep_active=true vd_destroy_content=false";
        } else {
            srvCmd += " new_display=" + cfg.virtualDisplaySpec +
                      " flex_display=true display_ime_policy=local keep_active=true vd_destroy_content=false";
        }
    }

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    std::wstring wSrvCmd = utf8ToWide(srvCmd);
    std::vector<wchar_t> mutableSrv(wSrvCmd.begin(), wSrvCmd.end());
    mutableSrv.push_back(L'\0');

    if (wSrvCmd.empty()) {
        outError = "Failed to build scrcpy-server command.";
        return false;
    }
    if (stepCb_) stepCb_(stepCbCtx_, 1);
    if (!CreateProcessW(NULL, mutableSrv.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        outError = "Failed to start scrcpy-server process.";
        return false;
    }
    serverProc_ = pi.hProcess;
    CloseHandle(pi.hThread);

    // 5. Connect Video socket #1 and wait for the 1-byte dummy handshake
    if (stepCb_) stepCb_(stepCbCtx_, 2);
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

    // 6. Connect Audio socket #2 (if enabled) and Control socket (TCP_NODELAY)
    if (wantAudio) {
        audioSock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (audioSock_ == INVALID_SOCKET ||
            connect(audioSock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            outError = "Failed to connect to scrcpy audio socket.";
            disconnect();
            return false;
        }
        BOOL nodelayA = TRUE;
        setsockopt(audioSock_, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&nodelayA), sizeof(nodelayA));
        int aRcvBuf = 256 * 1024;
        setsockopt(audioSock_, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&aRcvBuf), sizeof(aRcvBuf));
    }

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
    setsockopt(videoSock_, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    int rcvBuf = 1024 * 1024;
    setsockopt(videoSock_, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

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

    // 8. Spawn background reader & priority control-sender threads
    EnterCriticalSection(&sendLock_);
    controlQueue_.clear();
    LeaveCriticalSection(&sendLock_);
    InterlockedExchange(&injectVideoStall_, 0);
    InterlockedExchange(&stopThreads_, 0);
    InterlockedExchange(&connected_, 1);

    controlSenderThread_ = CreateThread(NULL, 0, controlSenderProc, this, 0, NULL);
    if (controlSenderThread_) SetThreadPriority(controlSenderThread_, THREAD_PRIORITY_HIGHEST);
    videoThread_ = CreateThread(NULL, 0, videoReaderProc, this, 0, NULL);
    if (audioSock_ != INVALID_SOCKET) {
        audioThread_ = CreateThread(NULL, 0, audioReaderProc, this, 0, NULL);
    }
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
    if (controlSendEvent_) SetEvent(controlSendEvent_);

    if (videoSock_ != INVALID_SOCKET) {
        shutdown(videoSock_, SD_BOTH);
        closesocket(videoSock_);
        videoSock_ = INVALID_SOCKET;
    }
    if (audioSock_ != INVALID_SOCKET) {
        shutdown(audioSock_, SD_BOTH);
        closesocket(audioSock_);
        audioSock_ = INVALID_SOCKET;
    }
    if (controlSock_ != INVALID_SOCKET) {
        shutdown(controlSock_, SD_BOTH);
        closesocket(controlSock_);
        controlSock_ = INVALID_SOCKET;
    }

    if (controlSenderThread_) {
        WaitForSingleObject(controlSenderThread_, 1000);
        CloseHandle(controlSenderThread_);
        controlSenderThread_ = NULL;
    }
    if (videoThread_) {
        WaitForSingleObject(videoThread_, 1000);
        CloseHandle(videoThread_);
        videoThread_ = NULL;
    }
    if (audioThread_) {
        WaitForSingleObject(audioThread_, 1000);
        CloseHandle(audioThread_);
        audioThread_ = NULL;
    }
    if (controlThread_) {
        WaitForSingleObject(controlThread_, 1000);
        CloseHandle(controlThread_);
        controlThread_ = NULL;
    }

    EnterCriticalSection(&sendLock_);
    controlQueue_.clear();
    LeaveCriticalSection(&sendLock_);

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
        const std::string jarToRm = !remoteServerJar_.empty()
            ? remoteServerJar_
            : "/data/local/tmp/km-scrcpy-server.jar";
        runCommandCapture(adb + " shell rm -f " + jarToRm, dummyOut, ec);
        scid_.clear();
        remoteServerJar_.clear();
    }
}

int ScrcpyBridge::allocateUniqueForwardPort(const std::vector<int>& usedPorts, int basePort, int maxOffset) {
    if (basePort < 1024) basePort = 27199;
    if (maxOffset < 1) maxOffset = 32;
    for (int off = 0; off < maxOffset; ++off) {
        int cand = basePort + off;
        if (cand > 65535) break;
        bool inUse = false;
        for (size_t i = 0; i < usedPorts.size(); ++i) {
            if (usedPorts[i] == cand) {
                inUse = true;
                break;
            }
        }
        if (!inUse) return cand;
    }
    return basePort + maxOffset;
}

void ScrcpyBridge::setForwardPort(int port) {
    if (port < 1024 || port > 65535) return;
    forwardPort_ = port;
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

bool ScrcpyBridge::enqueueControlPacket(std::vector<uint8_t>&& pkt, bool highPrio) {
    if (!connected_ || pkt.empty()) return false;
    EnterCriticalSection(&sendLock_);
    bool ok = (controlSock_ != INVALID_SOCKET);
    if (ok) {
        controlQueue_.push(std::move(pkt), highPrio);
    }
    LeaveCriticalSection(&sendLock_);
    if (ok && controlSendEvent_) {
        SetEvent(controlSendEvent_);
    }
    return ok;
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

    std::vector<uint8_t> msg(32);
    msg[0] = 2; // SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT
    msg[1] = static_cast<uint8_t>(action);
    writeU64BE(msg.data() + 2, pointerId);
    writeU32BE(msg.data() + 10, static_cast<uint32_t>(px));
    writeU32BE(msg.data() + 14, static_cast<uint32_t>(py));
    writeU16BE(msg.data() + 18, static_cast<uint16_t>(w));
    writeU16BE(msg.data() + 20, static_cast<uint16_t>(h));
    uint16_t pressure = (action == TOUCH_UP) ? 0x0000u : 0xFFFFu;
    writeU16BE(msg.data() + 22, pressure);
    writeU32BE(msg.data() + 24, 0u); // action_button = 0 (multi-touch finger)
    writeU32BE(msg.data() + 28, 0u); // buttons = 0

    return enqueueControlPacket(std::move(msg), true);
}

bool ScrcpyBridge::sendTouchNorm(TouchAction action, uint64_t pointerId, float nx, float ny) {
    uint32_t w = 0, h = 0;
    getScreenSize(w, h);
    int32_t px = static_cast<int32_t>(nx * static_cast<float>(w) + 0.5f);
    int32_t py = static_cast<int32_t>(ny * static_cast<float>(h) + 0.5f);
    return sendTouchPixels(action, pointerId, px, py);
}

bool ScrcpyBridge::sendKeycode(uint8_t action, uint32_t androidKeycode, uint32_t metastate) {
    if (!connected_) return false;
    // SC_CONTROL_MSG_TYPE_INJECT_KEYCODE (14 bytes):
    // [0] type=0, [1] action, [2..5] keycode, [6..9] repeat, [10..13] metastate
    std::vector<uint8_t> msg(14, 0);
    msg[0] = 0;
    msg[1] = action;
    writeU32BE(msg.data() + 2, androidKeycode);
    writeU32BE(msg.data() + 6, 0);
    writeU32BE(msg.data() + 10, metastate);
    return enqueueControlPacket(std::move(msg), true);
}

bool ScrcpyBridge::sendText(const std::string& utf8Text) {
    if (!connected_ || utf8Text.empty()) return false;
    // B3: Android KeyCharacterMap only injects ASCII; route any non-ASCII UTF-8 via clipboard-paste
    if (!isPureAscii(utf8Text)) {
        return sendSetClipboard(utf8Text, true);
    }
    // SC_CONTROL_MSG_TYPE_INJECT_TEXT (type=1): [1:1][len:4 BE][utf8 bytes] (low priority behind touch/key)
    std::vector<std::string> chunks = splitUtf8Chunks(utf8Text, 250);
    for (size_t i = 0; i < chunks.size(); ++i) {
        const std::string& chunk = chunks[i];
        std::vector<uint8_t> msg(5 + chunk.size());
        msg[0] = 1;
        writeU32BE(msg.data() + 1, static_cast<uint32_t>(chunk.size()));
        std::memcpy(msg.data() + 5, chunk.data(), chunk.size());
        if (!enqueueControlPacket(std::move(msg), false)) return false;
    }
    return true;
}

bool ScrcpyBridge::sendSetClipboard(const std::string& utf8Text, bool paste) {
    if (!connected_) return false;
    // 1.7: cap clipboard payload to 32 KB on a UTF-8 code-point boundary & enqueue at low priority
    std::string capped = ControlPacketQueue::truncateUtf8Cap(utf8Text, ControlPacketQueue::kMaxClipboardBytes);
    const size_t len = capped.size();
    std::vector<uint8_t> msg(14 + len);
    msg[0] = 9;
    EnterCriticalSection(&sendLock_);
    bool ok = (controlSock_ != INVALID_SOCKET);
    if (ok) {
        uint64_t seq = ++clipSeq_;
        writeU64BE(msg.data() + 1, seq);
        msg[9] = paste ? 1u : 0u;
        writeU32BE(msg.data() + 10, static_cast<uint32_t>(len));
        if (len > 0) std::memcpy(msg.data() + 14, capped.data(), len);
        controlQueue_.push(std::move(msg), false);
    }
    LeaveCriticalSection(&sendLock_);
    if (ok && controlSendEvent_) SetEvent(controlSendEvent_);
    return ok;
}

bool ScrcpyBridge::sendGetClipboard(uint8_t copyKey) {
    if (!connected_) return false;
    // SC_CONTROL_MSG_TYPE_GET_CLIPBOARD (type=8): [8:1][copy_key:1] (0=NONE, 1=COPY, 2=CUT)
    std::vector<uint8_t> msg(2);
    msg[0] = 8;
    msg[1] = copyKey;
    return enqueueControlPacket(std::move(msg), false);
}

void ScrcpyBridge::setClipboardCallback(DeviceClipboardFn fn, void* ctx) {
    EnterCriticalSection(&metaLock_);
    clipCb_ = fn;
    clipCbCtx_ = ctx;
    LeaveCriticalSection(&metaLock_);
}

void ScrcpyBridge::setConnectStepCallback(ConnectStepFn fn, void* ctx) {
    EnterCriticalSection(&metaLock_);
    stepCb_ = fn;
    stepCbCtx_ = ctx;
    LeaveCriticalSection(&metaLock_);
}

void ScrcpyBridge::setVideoPacketCallback(VideoPacketFn fn, void* ctx) {
    EnterCriticalSection(&metaLock_);
    videoCb_ = fn;
    videoCbCtx_ = ctx;
    LeaveCriticalSection(&metaLock_);
}

void ScrcpyBridge::setAudioPacketCallback(AudioPacketFn fn, void* ctx) {
    EnterCriticalSection(&metaLock_);
    audioCb_ = fn;
    audioCbCtx_ = ctx;
    LeaveCriticalSection(&metaLock_);
}

bool ScrcpyBridge::sendStartApp(const std::string& appName) {
    if (!connected_ || appName.empty()) return false;
    // SC_CONTROL_MSG_TYPE_START_APP (type=16 in scrcpy 3.0+/5.0+): [16:1][len:1][utf8 name]
    size_t len = appName.size();
    if (len > 255) len = 255;
    std::vector<uint8_t> msg(2 + len);
    msg[0] = 16;
    msg[1] = static_cast<uint8_t>(len);
    std::memcpy(msg.data() + 2, appName.data(), len);
    return enqueueControlPacket(std::move(msg), true);
}

bool ScrcpyBridge::sendResetVideo() {
    if (!connected_) return false;
    std::vector<uint8_t> msg(1, 17); // SC_CONTROL_MSG_TYPE_RESET_VIDEO
    return enqueueControlPacket(std::move(msg), true);
}

bool ScrcpyBridge::sendResizeDisplay(uint16_t width, uint16_t height) {
    if (!connected_) return false;
    std::vector<uint8_t> msg(5);
    size_t len = serializeResizeDisplay(msg.data(), width, height);
    msg.resize(len);
    return enqueueControlPacket(std::move(msg), true);
}

bool ScrcpyBridge::sendScanFile(const std::string& remotePathUtf8) {
    if (!connected_ || remotePathUtf8.empty()) return false;
    std::vector<uint8_t> msg(5 + 256);
    size_t len = serializeScanFile(msg.data(), msg.size(), remotePathUtf8);
    if (len == 0) return false;
    msg.resize(len);
    return enqueueControlPacket(std::move(msg), false);
}

size_t ScrcpyBridge::serializeBackOrScreenOn(uint8_t* buf, size_t cap, uint8_t action) {
    if (!buf || cap < 2) return 0;
    buf[0] = 4; // SC_CONTROL_MSG_TYPE_BACK_OR_SCREEN_ON
    buf[1] = action;
    return 2;
}

size_t ScrcpyBridge::serializeExpandNotificationPanel(uint8_t* buf, size_t cap) {
    if (!buf || cap < 1) return 0;
    buf[0] = 5; // SC_CONTROL_MSG_TYPE_EXPAND_NOTIFICATION_PANEL
    return 1;
}

size_t ScrcpyBridge::serializeExpandSettingsPanel(uint8_t* buf, size_t cap) {
    if (!buf || cap < 1) return 0;
    buf[0] = 6; // SC_CONTROL_MSG_TYPE_EXPAND_SETTINGS_PANEL
    return 1;
}

size_t ScrcpyBridge::serializeCollapsePanels(uint8_t* buf, size_t cap) {
    if (!buf || cap < 1) return 0;
    buf[0] = 7; // SC_CONTROL_MSG_TYPE_COLLAPSE_PANELS
    return 1;
}

size_t ScrcpyBridge::serializeRotateDevice(uint8_t* buf, size_t cap) {
    if (!buf || cap < 1) return 0;
    buf[0] = 11; // SC_CONTROL_MSG_TYPE_ROTATE_DEVICE
    return 1;
}

size_t ScrcpyBridge::serializeResetVideo(uint8_t* buf, size_t cap) {
    if (!buf || cap < 1) return 0;
    buf[0] = 17; // SC_CONTROL_MSG_TYPE_RESET_VIDEO
    return 1;
}

std::string ScrcpyBridge::buildShowTouchesServerArg(bool showTouches, bool cleanupEnabled) {
    // LATENCY B1: never leave show_touches enabled on the phone without server cleanup=true
    if (!showTouches || !cleanupEnabled) return "";
    return " show_touches=true";
}

bool ScrcpyBridge::sendBackOrScreenOn() {
    if (!connected_) return false;
    std::vector<uint8_t> dn(2), up(2);
    serializeBackOrScreenOn(dn.data(), dn.size(), 0);
    serializeBackOrScreenOn(up.data(), up.size(), 1);
    bool a = enqueueControlPacket(std::move(dn), true);
    bool b = enqueueControlPacket(std::move(up), true);
    return a && b;
}

bool ScrcpyBridge::sendExpandNotificationPanel() {
    if (!connected_) return false;
    std::vector<uint8_t> msg(1);
    serializeExpandNotificationPanel(msg.data(), msg.size());
    return enqueueControlPacket(std::move(msg), true);
}

bool ScrcpyBridge::sendExpandSettingsPanel() {
    if (!connected_) return false;
    std::vector<uint8_t> msg(1);
    serializeExpandSettingsPanel(msg.data(), msg.size());
    return enqueueControlPacket(std::move(msg), true);
}

bool ScrcpyBridge::sendCollapsePanels() {
    if (!connected_) return false;
    std::vector<uint8_t> msg(1);
    serializeCollapsePanels(msg.data(), msg.size());
    return enqueueControlPacket(std::move(msg), true);
}

bool ScrcpyBridge::sendRotateDevice() {
    if (!connected_) return false;
    std::vector<uint8_t> msg(1);
    serializeRotateDevice(msg.data(), msg.size());
    return enqueueControlPacket(std::move(msg), true);
}

bool ScrcpyBridge::sendPowerButton() {
    if (!connected_) return false;
    sendAndroidNav(26); // AKEYCODE_POWER
    return true;
}

bool ScrcpyBridge::sendVolumeMute() {
    if (!connected_) return false;
    sendAndroidNav(164); // AKEYCODE_VOLUME_MUTE
    return true;
}

bool ScrcpyBridge::sendUhidCreate(uint16_t id, uint16_t vendorId, uint16_t productId,
                                  const std::string& name, const uint8_t* reportDesc,
                                  uint16_t reportDescSize) {
    if (!connected_) return false;
    std::vector<uint8_t> msg(10 + 127 + static_cast<size_t>(reportDescSize));
    size_t len = serializeUhidCreate(msg.data(), msg.size(), id, vendorId, productId,
                                     name, reportDesc, reportDescSize);
    if (len == 0) return false;
    msg.resize(len);
    return enqueueControlPacket(std::move(msg), true);
}

bool ScrcpyBridge::sendUhidInput(uint16_t id, const uint8_t* data, uint16_t size) {
    if (!connected_) return false;
    std::vector<uint8_t> msg(5 + static_cast<size_t>(size));
    size_t len = serializeUhidInput(msg.data(), msg.size(), id, data, size);
    if (len == 0) return false;
    msg.resize(len);
    return enqueueControlPacket(std::move(msg), true);
}

bool ScrcpyBridge::sendUhidDestroy(uint16_t id) {
    if (!connected_) return false;
    std::vector<uint8_t> msg(3);
    size_t len = serializeUhidDestroy(msg.data(), msg.size(), id);
    if (len == 0) return false;
    msg.resize(len);
    return enqueueControlPacket(std::move(msg), true);
}

std::string ScrcpyBridge::parseForegroundPackage(const std::string& raw) {
    // Parses Android 8-16 dumpsys window / dumpsys activity lines such as:
    //   mCurrentFocus=Window{abc u0 com.dts.freefiremax/com.dts.freefireth.FFMainActivity}
    //   mFocusedApp=ActivityRecord{abc u0 com.whatsapp/.HomeActivity t42}
    //   topResumedActivity=ActivityRecord{abc u0 com.android.chrome/org.chromium.chrome.browser.ChromeTabbedActivity t19}
    std::istringstream iss(raw);
    std::string line;
    std::string fallback;

    auto extractPkgFromLine = [](const std::string& ln) -> std::string {
        size_t slash = ln.find('/');
        while (slash != std::string::npos) {
            size_t start = slash;
            while (start > 0) {
                char c = ln[start - 1];
                bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_';
                if (!ok) break;
                --start;
            }
            if (slash > start) {
                std::string pkg = ln.substr(start, slash - start);
                // Valid Android package names contain at least one dot and aren't transient system overlays
                if (pkg.find('.') != std::string::npos &&
                    pkg != "com.android.systemui" &&
                    pkg.find("inputmethod") == std::string::npos) {
                    return pkg;
                }
            }
            slash = ln.find('/', slash + 1);
        }
        return "";
    };

    while (std::getline(iss, line)) {
        if (line.find("NotificationShade") != std::string::npos ||
            line.find("InputMethod") != std::string::npos ||
            line.find("StatusBar") != std::string::npos) {
            continue;
        }
        bool isPrimary = (line.find("mCurrentFocus") != std::string::npos ||
                          line.find("topResumedActivity") != std::string::npos);
        bool isSecondary = (line.find("mFocusedApp") != std::string::npos ||
                            line.find("mResumedActivity") != std::string::npos);
        if (!isPrimary && !isSecondary) continue;

        std::string pkg = extractPkgFromLine(line);
        if (!pkg.empty()) {
            if (isPrimary) return pkg;
            if (fallback.empty()) fallback = pkg;
        }
    }
    return fallback;
}

bool ScrcpyBridge::parseWindowSecureFlag(const std::string& dumpsysRaw) {
    if (dumpsysRaw.empty()) return false;
    return (dumpsysRaw.find("FLAG_SECURE") != std::string::npos ||
            dumpsysRaw.find("isSecure=true") != std::string::npos);
}

bool ScrcpyBridge::queryForegroundInfo(std::string& outPackage, bool& outSecure) {
    outPackage.clear();
    outSecure = false;
    if (!connected_ || adbExe_.empty()) return false;
    std::string raw;
    DWORD ec = 0;
    std::string adb = adbPrefix();
    if (runCommandCapture(adb + " shell \"dumpsys window displays | grep -E 'mCurrentFocus|mFocusedApp|FLAG_SECURE'\"", raw, ec, 5000)) {
        outSecure = parseWindowSecureFlag(raw);
        outPackage = parseForegroundPackage(raw);
        if (!outPackage.empty()) return true;
    }
    if (runCommandCapture(adb + " shell \"dumpsys activity activities | grep -E 'topResumedActivity|mResumedActivity'\"", raw, ec, 5000)) {
        outPackage = parseForegroundPackage(raw);
        if (!outPackage.empty()) return true;
    }
    return false;
}

std::string ScrcpyBridge::queryForegroundPackage() {
    std::string pkg;
    bool sec = false;
    queryForegroundInfo(pkg, sec);
    return pkg;
}

bool ScrcpyBridge::installApk(const std::string& localApkPath, std::string& outMsg) {
    outMsg.clear();
    if (adbExe_.empty()) { outMsg = "ADB not ready"; return false; }
    std::string raw;
    DWORD ec = 0;
    if (!runCommandCapture(adbPrefix() + " install -r \"" + localApkPath + "\"", raw, ec, 60000)) {
        outMsg = "Failed to execute adb install";
        return false;
    }
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
    bool ok = (ec == 0 && raw.find("Success") != std::string::npos);
    outMsg = ok ? "APK installed successfully" : (raw.empty() ? "APK install failed" : raw);
    appendLog("[APK] " + outMsg);
    return ok;
}

bool ScrcpyBridge::pushFileToRemoteDir(const std::string& localFilePath, const std::string& remoteDir, std::string& outMsg) {
    outMsg.clear();
    if (adbExe_.empty()) { outMsg = "ADB not ready"; return false; }
    size_t slash = localFilePath.find_last_of("\\/");
    std::string fname = (slash == std::string::npos) ? localFilePath : localFilePath.substr(slash + 1);
    if (fname.empty()) { outMsg = "Invalid file path"; return false; }
    std::string dir = remoteDir.empty() ? std::string("/sdcard/Download") : remoteDir;
    if (dir[0] != '/') dir.insert(dir.begin(), '/');
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    std::string remote = (dir == "/") ? ("/" + fname) : (dir + "/" + fname);
    std::string raw;
    DWORD ec = 0;
    if (!runCommandCapture(adbPrefix() + " push \"" + localFilePath + "\" \"" + remote + "\"", raw, ec, 120000) || ec != 0) {
        outMsg = "Push failed for " + fname;
        appendLog("[Push] " + outMsg);
        return false;
    }
    if (connected_) {
        sendScanFile(remote);
        appendLog("[Scan] SCAN_FILE queued -> " + remote);
    }
    outMsg = "Sent " + fname + " -> " + dir + "/";
    appendLog("[Push] " + outMsg);
    return true;
}

bool ScrcpyBridge::pushFileToDownloads(const std::string& localFilePath, std::string& outMsg) {
    return pushFileToRemoteDir(localFilePath, "/sdcard/Download", outMsg);
}

bool ScrcpyBridge::listRemoteDirectory(const std::string& remoteDir, std::vector<RemoteFileEntry>& outEntries, std::string& outErr) {
    outEntries.clear();
    outErr.clear();
    if (adbExe_.empty()) {
        outErr = "ADB not ready";
        return false;
    }
    std::string dir = remoteDir.empty() ? std::string("/sdcard") : remoteDir;
    if (dir[0] != '/') dir.insert(dir.begin(), '/');
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    std::string listTarget = (dir == "/") ? "/" : (dir + "/");
    std::string raw;
    DWORD ec = 0;
    std::string cmd = adbPrefix() + " shell \"ls -la " + shellSingleQuote(listTarget) + " 2>&1\"";
    if (!runCommandCapture(cmd, raw, ec, 10000)) {
        outErr = "Timed out listing " + dir;
        return false;
    }
    if (ec != 0 && raw.find("No such file") != std::string::npos) {
        outErr = "Directory not found: " + dir;
        return false;
    }
    if (ec != 0 && raw.find("Permission denied") != std::string::npos) {
        outErr = "Permission denied: " + dir;
        return false;
    }
    outEntries = parseLsLaOutput(raw, dir);
    return true;
}

bool ScrcpyBridge::pullRemoteFile(const std::string& remotePath, const std::string& localDestPath, std::string& outMsg) {
    outMsg.clear();
    if (adbExe_.empty()) { outMsg = "ADB not ready"; return false; }
    if (remotePath.empty() || localDestPath.empty()) { outMsg = "Invalid pull path"; return false; }
    size_t slash = remotePath.find_last_of('/');
    std::string fname = (slash == std::string::npos) ? remotePath : remotePath.substr(slash + 1);
    std::string raw;
    DWORD ec = 0;
    if (!runCommandCapture(adbPrefix() + " pull \"" + remotePath + "\" \"" + localDestPath + "\"", raw, ec, 120000) || ec != 0) {
        outMsg = "Save to PC failed for " + fname;
        appendLog("[Pull] " + outMsg);
        return false;
    }
    outMsg = "Saved " + fname + " to PC";
    appendLog("[Pull] " + outMsg + " -> " + localDestPath);
    return true;
}

void ScrcpyBridge::sendAndroidNav(uint32_t androidKeycode) {
    if (!connected_) return;
    sendKeycode(0 /* ACTION_DOWN */, androidKeycode);
    sendKeycode(1 /* ACTION_UP */, androidKeycode);
}

bool ScrcpyBridge::setDisplayPower(bool on) {
    if (!connected_) return false;
    // SC_CONTROL_MSG_TYPE_SET_DISPLAY_POWER (type=10, 2 bytes: [10, on ? 1 : 0])
    std::vector<uint8_t> msg(2);
    msg[0] = 10;
    msg[1] = on ? 1u : 0u;
    return enqueueControlPacket(std::move(msg), true);
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

DWORD WINAPI ScrcpyBridge::audioReaderProc(LPVOID param) {
    static_cast<ScrcpyBridge*>(param)->runAudioReader();
    return 0;
}

void ScrcpyBridge::runAudioReader() {
    DWORD taskIdx = 0;
    HANDLE avrt = ThreadScheduling::enableMmcssAudio(taskIdx);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    uint8_t idBuf[4] = {0};
    if (!recvAll(audioSock_, idBuf, 4)) {
        ThreadScheduling::disableMmcss(avrt);
        return;
    }

    const uint32_t rawCodecId = readU32BE(idBuf);
    if (rawCodecId == 0u) {
        appendLog("[Audio] Stream disabled by device (Android <11 or capture denied); continuing video-only.");
        ThreadScheduling::disableMmcss(avrt);
        return;
    }
    if (rawCodecId == 1u) {
        appendLog("[Audio] Device audio configuration error; continuing video-only.");
        ThreadScheduling::disableMmcss(avrt);
        return;
    }

    const AudioCodecId codec = parseAudioCodecId(rawCodecId);
    if (codec == AUDIO_CODEC_NONE) {
        char msg[80];
        std::snprintf(msg, sizeof(msg), "[Audio] Unknown audio codec 0x%08X; continuing video-only.",
                      static_cast<unsigned>(rawCodecId));
        appendLog(msg);
        ThreadScheduling::disableMmcss(avrt);
        return;
    }

    appendLog(codec == AUDIO_CODEC_RAW
        ? "[Audio] Native in-app stream active (raw PCM 48kHz stereo)"
        : "[Audio] Native in-app stream active (AAC 48kHz stereo)");

    std::vector<uint8_t> payload;
    while (!stopThreads_) {
        uint8_t hdr[12];
        if (!recvAll(audioSock_, hdr, 12)) break;

        uint64_t ptsUs = 0;
        uint32_t pktSize = 0;
        bool isConfig = false;
        if (!parseAudioPacketHeader(hdr, isV4Proto_, ptsUs, pktSize, isConfig)) {
            break;
        }

        payload.resize(pktSize);
        if (!recvAll(audioSock_, payload.data(), static_cast<int>(pktSize))) {
            break;
        }

        AudioPacketFn acb = nullptr;
        void* actx = nullptr;
        EnterCriticalSection(&metaLock_);
        acb = audioCb_;
        actx = audioCbCtx_;
        LeaveCriticalSection(&metaLock_);
        if (acb && pktSize > 0) {
            acb(actx, codec, payload.data(), pktSize, ptsUs, isConfig);
        }
    }

    ThreadScheduling::disableMmcss(avrt);
}

DWORD WINAPI ScrcpyBridge::controlReaderProc(LPVOID param) {
    static_cast<ScrcpyBridge*>(param)->runControlReader();
    return 0;
}

DWORD WINAPI ScrcpyBridge::controlSenderProc(LPVOID param) {
    static_cast<ScrcpyBridge*>(param)->runControlSender();
    return 0;
}

void ScrcpyBridge::runControlSender() {
    DWORD taskIdx = 0;
    HANDLE avrt = ThreadScheduling::enableMmcssGames(taskIdx);

    std::vector<uint8_t> pkt;
    while (!stopThreads_) {
        bool hasPkt = false;
        SOCKET sock = INVALID_SOCKET;
        EnterCriticalSection(&sendLock_);
        sock = controlSock_;
        if (sock != INVALID_SOCKET) {
            hasPkt = controlQueue_.popNext(pkt);
        }
        LeaveCriticalSection(&sendLock_);

        if (!hasPkt) {
            if (controlSendEvent_) {
                WaitForSingleObject(controlSendEvent_, 20);
            } else {
                Sleep(1);
            }
            continue;
        }

        if (!sendAll(sock, pkt.data(), static_cast<int>(pkt.size()))) {
            InterlockedExchange(&connected_, 0);
            break;
        }
    }

    ThreadScheduling::disableMmcss(avrt);
}

void ScrcpyBridge::runVideoReader() {
    DWORD taskIdx = 0;
    HANDLE avrt = ThreadScheduling::enableMmcssGames(taskIdx);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    std::vector<uint8_t> payload;
    int consecutiveSkips = 0;
    while (!stopThreads_) {
        uint8_t hdr[12];
        if (!recvAll(videoSock_, hdr, 12)) break;
        const int64_t qpcPktRecv = LatencyTelemetry::nowQpc();

        // 1.1 debug stall test: simulate a 300ms decoder/network stall on Ctrl+Shift+T
        if (InterlockedCompareExchange(&injectVideoStall_, 0, 1) == 1) {
            appendLog("[Video] Injecting 300ms stall test...");
            Sleep(300);
        }

        // scrcpy v4/v5+ emits a 12-byte session header (top bit set!) on rotation or client-requested resize
        if (isV4Proto_ && (hdr[0] & 0x80u) != 0) {
            uint32_t newW = 0, newH = 0;
            bool clientResized = false;
            if (parseV4SessionHeader(hdr, newW, newH, clientResized)) {
                EnterCriticalSection(&metaLock_);
                bool changed = (newW != screenW_ || newH != screenH_);
                screenW_ = newW;
                screenH_ = newH;
                LeaveCriticalSection(&metaLock_);
                if (changed) {
                    std::printf("[Bridge] Screen rotation/resize (v4/v5 session%s): %ux%u\n",
                                clientResized ? " [client]" : "", newW, newH);
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

        // scrcpy v2/v3: Bit 63 = CONFIG, Bit 62 = KEY_FRAME, Bits 61..0 = PTS
        // scrcpy v4/v5: Bit 63 = SESSION_META, Bit 62 = CONFIG, Bit 61 = KEY_FRAME, Bits 60..0 = PTS
        const bool isConfig   = isV4Proto_ ? ((ptsAndFlags & (1ULL << 62)) != 0)
                                           : ((ptsAndFlags & (1ULL << 63)) != 0);
        const bool isKeyFrame = isV4Proto_ ? ((ptsAndFlags & (1ULL << 61)) != 0)
                                           : ((ptsAndFlags & (1ULL << 62)) != 0);
        const uint64_t ptsUs  = isV4Proto_ ? (ptsAndFlags & 0x1FFFFFFFFFFFFFFFULL)
                                           : (ptsAndFlags & 0x3FFFFFFFFFFFFFFFULL);

        // On scrcpy v4/v5+, writeSessionMeta [0x80000000][W][H] is the exact authoritative videoSize
        // required by PositionMapper.map() — NEVER overwrite it with coded/cropped H.264 SPS dimensions!
        if (!isV4Proto_ && isConfig && pktSize > 4) {
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

        // 1.1: Decode every packet for H.264 P-frame reference integrity, but only present
        // when no newer packet is queued in the socket (with a 3-skip safety valve).
        bool wantPresent = true;
        if (!isConfig && pktSize > 0) {
            u_long avail = 0;
            ioctlsocket(videoSock_, FIONREAD, &avail);
            wantPresent = (avail < 12);
            if (!wantPresent && consecutiveSkips < 3) {
                ++consecutiveSkips;
            } else {
                wantPresent = true;
                consecutiveSkips = 0;
            }
        }

        VideoPacketFn vcb = nullptr;
        void* vctx = nullptr;
        EnterCriticalSection(&metaLock_);
        vcb = videoCb_;
        vctx = videoCbCtx_;
        LeaveCriticalSection(&metaLock_);
        if (vcb && pktSize > 0) {
            vcb(vctx, payload.data(), pktSize, ptsUs, isConfig, isKeyFrame, wantPresent, qpcPktRecv);
        }
    }
    ThreadScheduling::disableMmcss(avrt);
    InterlockedExchange(&connected_, 0);
}

void ScrcpyBridge::runControlReader() {
    // Parse framed scrcpy DeviceMessages (clipboard sync from Android -> Windows!)
    while (!stopThreads_) {
        uint8_t msgType = 0;
        if (!recvAll(controlSock_, &msgType, 1)) break;

        if (msgType == 0 /* DEVICE_MSG_TYPE_CLIPBOARD */) {
            uint8_t lenBuf[4];
            if (!recvAll(controlSock_, lenBuf, 4)) break;
            uint32_t len = readU32BE(lenBuf);
            if (len > 4u * 1024u * 1024u) break;
            std::string clipText(len, '\0');
            if (len > 0 && !recvAll(controlSock_, reinterpret_cast<uint8_t*>(&clipText[0]), static_cast<int>(len))) {
                break;
            }
            DeviceClipboardFn cb = nullptr;
            void* ctx = nullptr;
            EnterCriticalSection(&metaLock_);
            cb = clipCb_;
            ctx = clipCbCtx_;
            LeaveCriticalSection(&metaLock_);
            if (cb && !clipText.empty()) {
                cb(ctx, clipText);
            }
        } else if (msgType == 1 /* DEVICE_MSG_TYPE_ACK_CLIPBOARD */) {
            uint8_t seqBuf[8];
            if (!recvAll(controlSock_, seqBuf, 8)) break;
        } else if (msgType == 2 /* DEVICE_MSG_TYPE_UHID_OUTPUT */) {
            uint8_t hdr[4];
            if (!recvAll(controlSock_, hdr, 4)) break;
            uint16_t sz = static_cast<uint16_t>((static_cast<uint16_t>(hdr[2]) << 8) | hdr[3]);
            if (sz > 0) {
                std::vector<uint8_t> skip(sz);
                if (!recvAll(controlSock_, skip.data(), static_cast<int>(sz))) break;
            }
        } else {
            // Unknown future device message type: stop parsing stream safely
            break;
        }
    }
}
