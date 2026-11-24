#pragma once

#include "config/Profile.hpp"

#include <string>
#include <vector>

class ProfileStore {
public:
    static bool load(const std::string& path, Profile& outProfile);
    static bool save(const std::string& path, const Profile& profile);

    static std::vector<std::string> listProfiles(const std::string& profilesDir);
    static std::string sanitizeFilename(const std::string& name);

    static std::string loadLastActive(const std::string& profilesDir);
    static void saveLastActive(const std::string& profilesDir, const std::string& fullPath);
};
