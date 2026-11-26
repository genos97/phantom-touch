#pragma once

#include "config/Profile.hpp"

#include <string>
#include <vector>

class ProfileStore {
public:
    static bool load(const std::string& path, Profile& outProfile);
    static bool save(const std::string& path, const Profile& profile);

    static std::string serializeToString(const Profile& profile);
    static bool parseFromString(const std::string& text, Profile& outProfile, std::string* outError = nullptr);

    static bool importProfileFile(const std::string& srcUtf8Path, const std::string& profilesDir,
                                  std::string& outSavedPath, std::string& outError);
    static bool importProfileText(const std::string& cfgText, const std::string& profilesDir,
                                  std::string& outSavedPath, std::string& outError);
    static bool moveToTrash(const std::string& profilePath, const std::string& profilesDir,
                            std::string& outTrashPath);
    static bool restoreFromTrash(const std::string& trashPath, const std::string& originalPath);

    static std::vector<std::string> listProfiles(const std::string& profilesDir);
    static std::string sanitizeFilename(const std::string& name);

    static std::string loadLastActive(const std::string& profilesDir);
    static void saveLastActive(const std::string& profilesDir, const std::string& fullPath);
};
