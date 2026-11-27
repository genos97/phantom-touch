#pragma once

#include <string>

// Creates Windows .lnk desktop shortcuts that launch PhantomTouch in Second-Phone mode
// directly into a chosen Android app (`--app=<package>`).
class DesktopShortcut {
public:
    static std::string sanitizeTitle(const std::string& rawTitle, const std::string& fallbackPkg);

    static bool createAppShortcutFile(const std::string& lnkPathUtf8,
                                      const std::string& exePathUtf8,
                                      const std::string& workDirUtf8,
                                      const std::string& appNameUtf8,
                                      const std::string& packageNameUtf8,
                                      std::string& outError);

    static bool createDesktopAppShortcut(const std::string& exePathUtf8,
                                         const std::string& workDirUtf8,
                                         const std::string& appNameUtf8,
                                         const std::string& packageNameUtf8,
                                         std::string& outLnkPathUtf8,
                                         std::string& outError);
};
