#include "ui/Strings.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cctype>
#include <fstream>
#include <sstream>

namespace {

std::string g_langDir;
std::string g_langCode = "en";
std::map<std::string, std::string> g_catalog;

std::string trimCopy(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool readFileUtf8(const std::string& path, std::string& out) {
    out.clear();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0);
    if (wlen > 1) {
        std::wstring wp(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wp[0], wlen);
        HANDLE hf = CreateFileW(wp.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            DWORD sz = GetFileSize(hf, NULL);
            if (sz != INVALID_FILE_SIZE && sz < 4u * 1024u * 1024u) {
                out.resize(sz);
                DWORD rd = 0;
                if (sz == 0 || ReadFile(hf, &out[0], sz, &rd, NULL)) {
                    out.resize(rd);
                    CloseHandle(hf);
                    return true;
                }
            }
            CloseHandle(hf);
            return false;
        }
    }
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

} // namespace

namespace Strings {

const std::vector<LanguageInfo>& languages() {
    static std::vector<LanguageInfo> list;
    if (list.empty()) {
        list.push_back(LanguageInfo{ "en",    "EN",     "English" });
        list.push_back(LanguageInfo{ "pt-BR", "PT-BR",  "Portugu\xC3\xAAs (BR)" });
        list.push_back(LanguageInfo{ "es",    "ES",     "Espa\xC3\xB1ol" });
        list.push_back(LanguageInfo{ "id",    "ID",     "Bahasa Indonesia" });
        list.push_back(LanguageInfo{ "hi",    "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80", "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80" });
        list.push_back(LanguageInfo{ "ru",    "RU",     "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9" });
    }
    return list;
}

int languageIndex(const std::string& code) {
    const std::vector<LanguageInfo>& list = languages();
    for (size_t i = 0; i < list.size(); ++i) {
        if (code == list[i].code) return static_cast<int>(i);
    }
    return 0;
}

void parseCatalogText(const std::string& utf8Content, std::map<std::string, std::string>& outMap) {
    outMap.clear();
    size_t start = 0;
    if (utf8Content.size() >= 3 &&
        static_cast<unsigned char>(utf8Content[0]) == 0xEFu &&
        static_cast<unsigned char>(utf8Content[1]) == 0xBBu &&
        static_cast<unsigned char>(utf8Content[2]) == 0xBFu) {
        start = 3;
    }

    std::istringstream iss(utf8Content.substr(start));
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = trimCopy(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trimCopy(line.substr(0, eq));
        std::string v = trimCopy(line.substr(eq + 1));
        if (!k.empty()) {
            outMap[k] = v;
        }
    }
}

void init(const std::string& langDir, const std::string& langCode) {
    g_langDir = langDir;
    setLanguage(langCode);
}

bool setLanguage(const std::string& langCode) {
    int idx = languageIndex(langCode);
    g_langCode = languages()[idx].code;
    g_catalog.clear();

    if (g_langDir.empty()) return (g_langCode == "en");

    std::string content;
    std::string path = g_langDir + "\\" + g_langCode + ".txt";
    if (!readFileUtf8(path, content)) {
        if (g_langCode != "en") {
            std::string enPath = g_langDir + "\\en.txt";
            if (readFileUtf8(enPath, content)) {
                parseCatalogText(content, g_catalog);
            }
        }
        return (g_langCode == "en");
    }

    parseCatalogText(content, g_catalog);
    return !g_catalog.empty() || (g_langCode == "en");
}

std::string currentLanguage() {
    return g_langCode;
}

std::string get(const char* key, const char* fallbackEn) {
    if (key && *key) {
        std::map<std::string, std::string>::const_iterator it = g_catalog.find(key);
        if (it != g_catalog.end() && !it->second.empty()) {
            return it->second;
        }
    }
    return fallbackEn ? std::string(fallbackEn) : std::string();
}

} // namespace Strings
