#pragma once

#include <map>
#include <string>
#include <vector>

struct LanguageInfo {
    const char* code;
    const char* shortLabel;
    const char* nativeName;
};

namespace Strings {

void init(const std::string& langDir, const std::string& langCode);
bool setLanguage(const std::string& langCode);
std::string currentLanguage();

std::string get(const char* key, const char* fallbackEn);
inline std::string tr(const char* key, const char* fallbackEn) {
    return get(key, fallbackEn);
}

const std::vector<LanguageInfo>& languages();
int languageIndex(const std::string& code);
void parseCatalogText(const std::string& utf8Content, std::map<std::string, std::string>& outMap);

} // namespace Strings
