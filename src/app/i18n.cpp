#include "xpbd/app/i18n.hpp"

#include "xpbd/log.hpp"

#include <nlohmann/json.hpp>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace xpbd::app {
namespace {

Lang g_lang = kDefaultLang;

std::unordered_map<std::string, std::string> g_table;
std::unordered_map<std::string, std::string> g_fallback_en;
std::string g_i18n_dir;


const char* kBootEn[][2] = {
    {"about", "About"},
    {"about_title", "About XPBD Bone Baker"},
    {"about_close", "Close"},
    {"open_model", "Open Model"},
    {nullptr, nullptr},
};

std::filesystem::path exeDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::filesystem::path(buf).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

const char* fileForLang(Lang lang) {
    switch (lang) {
        case Lang::ZhCn:
            return "zh-CN.json";
        case Lang::En:
        default:
            return "en.json";
    }
}

bool loadJsonFile(const std::filesystem::path& path,
                  std::unordered_map<std::string, std::string>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        xpbd::log::errorf("i18n parse failed %s: %s", path.string().c_str(), e.what());
        return false;
    }
    if (!j.is_object()) {
        return false;
    }
    out.clear();
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.value().is_string()) {
            out[it.key()] = it.value().get<std::string>();
        }
    }
    return !out.empty();
}

std::vector<std::filesystem::path> searchRoots() {
    std::vector<std::filesystem::path> roots;
    roots.push_back(exeDir() / "i18n");
    roots.push_back(std::filesystem::current_path() / "i18n");
#if defined(_WIN32)

    roots.push_back(exeDir() / ".." / ".." / "i18n");
    roots.push_back(exeDir() / ".." / ".." / ".." / "i18n");
#endif
    return roots;
}

bool tryLoadLang(Lang lang, std::unordered_map<std::string, std::string>& out) {
    const char* file = fileForLang(lang);
    for (const auto& root : searchRoots()) {
        const auto path = root / file;
        if (std::filesystem::exists(path) && loadJsonFile(path, out)) {
            g_i18n_dir = root.string();
            xpbd::log::infof("i18n loaded %s (%zu keys)", path.string().c_str(), out.size());
            return true;
        }
    }
    return false;
}

void loadFallbackEn() {
    g_fallback_en.clear();
    if (!tryLoadLang(Lang::En, g_fallback_en)) {
        for (int i = 0; kBootEn[i][0] != nullptr; ++i) {
            g_fallback_en[kBootEn[i][0]] = kBootEn[i][1];
        }
        xpbd::log::warn("i18n: en.json missing, using minimal boot strings");
    }
}

void applyLang(Lang lang) {
    g_lang = lang;
    std::unordered_map<std::string, std::string> loaded;
    if (!tryLoadLang(lang, loaded)) {
        xpbd::log::warnf("i18n: missing %s, falling back to English", fileForLang(lang));
        g_table = g_fallback_en;
        g_lang = Lang::En;
        return;
    }

    g_table = g_fallback_en;
    for (auto& [k, v] : loaded) {
        g_table[k] = std::move(v);
    }
}

}

void initI18n() {
    loadFallbackEn();
    applyLang(kDefaultLang);
}

void setLang(Lang lang) {
    applyLang(lang);
}

Lang currentLang() {
    return g_lang;
}

const char* langName(Lang lang) {
    switch (lang) {
        case Lang::ZhCn:
            return tr("lang_zh_cn");
        case Lang::En:
        default:
            return tr("lang_en");
    }
}

const char* tr(const char* key) {
    if (key == nullptr) {
        return "";
    }
    auto it = g_table.find(key);
    if (it != g_table.end()) {
        return it->second.c_str();
    }
    it = g_fallback_en.find(key);
    if (it != g_fallback_en.end()) {
        return it->second.c_str();
    }
    return key;
}

const char* trf(const char* key, ...) {
    static thread_local char buf[512];
    const char* fmt = tr(key);
    va_list ap;
    va_start(ap, key);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

}
