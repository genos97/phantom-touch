#include "ui/DesktopShortcut.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <vector>

namespace {

std::wstring utf8ToWideLocal(const std::string& u8) {
    if (u8.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), static_cast<int>(u8.size()), NULL, 0);
    if (n <= 0) return L"";
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), static_cast<int>(u8.size()), &w[0], n);
    return w;
}

std::string wideToUtf8Local(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), NULL, 0, NULL, NULL);
    if (n <= 0) return "";
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], n, NULL, NULL);
    return out;
}

} // namespace

std::string DesktopShortcut::sanitizeTitle(const std::string& rawTitle, const std::string& fallbackPkg) {
    std::string src = rawTitle.empty() ? fallbackPkg : rawTitle;
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size() && out.size() < 96; ++i) {
        unsigned char c = static_cast<unsigned char>(src[i]);
        if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            out.push_back(' ');
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    size_t lead = 0;
    while (lead < out.size() && out[lead] == ' ') ++lead;
    if (lead > 0) out.erase(0, lead);
    if (out.empty()) out = fallbackPkg.empty() ? std::string("PhantomApp") : fallbackPkg;
    return out;
}

bool DesktopShortcut::createAppShortcutFile(const std::string& lnkPathUtf8,
                                             const std::string& exePathUtf8,
                                             const std::string& workDirUtf8,
                                             const std::string& appNameUtf8,
                                             const std::string& packageNameUtf8,
                                             std::string& outError) {
    outError.clear();
    if (lnkPathUtf8.empty() || exePathUtf8.empty() || packageNameUtf8.empty()) {
        outError = "Missing shortcut path, executable path, or package name.";
        return false;
    }

    const std::wstring wLnk = utf8ToWideLocal(lnkPathUtf8);
    const std::wstring wExe = utf8ToWideLocal(exePathUtf8);
    const std::wstring wDir = utf8ToWideLocal(workDirUtf8);
    const std::wstring wArgs = L"--app=" + utf8ToWideLocal(packageNameUtf8);
    const std::string title = sanitizeTitle(appNameUtf8, packageNameUtf8);
    const std::wstring wDesc = utf8ToWideLocal("Open " + title + " on Phantom Touch Second Phone");

    HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool shouldUninit = (hrInit == S_OK || hrInit == S_FALSE);
    if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE) {
        outError = "COM initialization failed.";
        return false;
    }

    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, reinterpret_cast<void**>(&psl));
    if (FAILED(hr) || !psl) {
        if (shouldUninit) CoUninitialize();
        outError = "Failed creating IShellLinkW instance.";
        return false;
    }

    psl->SetPath(wExe.c_str());
    psl->SetArguments(wArgs.c_str());
    if (!wDir.empty()) psl->SetWorkingDirectory(wDir.c_str());
    psl->SetDescription(wDesc.c_str());
    psl->SetIconLocation(wExe.c_str(), 0);

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
    bool ok = false;
    if (SUCCEEDED(hr) && ppf) {
        hr = ppf->Save(wLnk.c_str(), TRUE);
        ok = SUCCEEDED(hr);
        if (!ok) outError = "IPersistFile::Save failed.";
        ppf->Release();
    } else {
        outError = "Failed querying IPersistFile interface.";
    }
    psl->Release();
    if (shouldUninit) CoUninitialize();
    return ok;
}

bool DesktopShortcut::createDesktopAppShortcut(const std::string& exePathUtf8,
                                                const std::string& workDirUtf8,
                                                const std::string& appNameUtf8,
                                                const std::string& packageNameUtf8,
                                                std::string& outLnkPathUtf8,
                                                std::string& outError) {
    outLnkPathUtf8.clear();
    wchar_t deskBuf[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, deskBuf))) {
        outError = "Could not locate Windows Desktop folder.";
        return false;
    }
    std::string deskUtf8 = wideToUtf8Local(deskBuf);
    if (deskUtf8.empty()) {
        outError = "Desktop path conversion failed.";
        return false;
    }
    const std::string title = sanitizeTitle(appNameUtf8, packageNameUtf8);
    outLnkPathUtf8 = deskUtf8 + "\\" + title + " (Phone).lnk";
    return createAppShortcutFile(outLnkPathUtf8, exePathUtf8, workDirUtf8, appNameUtf8, packageNameUtf8, outError);
}
