#include "store.h"
#include <cstring>
#include <mbedtls/md5.h>

namespace store {

namespace {
constexpr const char* kDir = "/browser";
constexpr const char* kSettings = "/browser/settings.txt";
constexpr const char* kHistory = "/browser/history.txt";

bool ensureDir() {
    if (SD.exists(kDir)) return true;
    return SD.mkdir(kDir);
}

std::string hashOf(const std::string& s) {
    unsigned char md[16];
    mbedtls_md5((const unsigned char*)s.data(), s.size(), md);
    char out[33];
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", md[i]);
    return std::string(out, 16);
}
}  // namespace

bool begin() {
    if (!SD.begin()) return false;
    return ensureDir();
}

std::string get(const std::string& key, const std::string& fallback) {
    File f = SD.open(kSettings, FILE_READ);
    if (!f) return fallback;
    std::string val = fallback;
    size_t klen = key.size();
    while (f.available()) {
        std::string line;
        char c;
        while (f.readBytes(&c, 1) && c != '\n') line += c;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > klen + 1 && line[0] == key[0] &&
            std::memcmp(line.data(), key.data(), klen) == 0 && line[klen] == '=') {
            val = line.substr(klen + 1);
        }
    }
    f.close();
    return val;
}

void set(const std::string& key, const std::string& value) {
    // rewrite the file (small)
    std::vector<std::string> kept;
    File f = SD.open(kSettings, FILE_READ);
    if (f) {
        std::string line;
        while (f.available()) {
            line.clear();
            char c;
            while (f.readBytes(&c, 1) && c != '\n') line += c;
            size_t eq = line.find('=');
            if (eq != std::string::npos && line.substr(0, eq) != key)
                kept.push_back(line);
        }
        f.close();
    }
    File out = SD.open(kSettings, FILE_WRITE);
    if (out) {
        for (auto& l : kept) { out.print(l.c_str()); out.println(); }
        out.print(key.c_str()); out.print('='); out.println(value.c_str());
        out.close();
    }
}

void addUrl(const std::string& url) {
    auto list = urls(50);
    if (!list.empty() && list[0] == url) return;
    list.insert(list.begin(), url);
    if (list.size() > 20) list.resize(20);
    File f = SD.open(kHistory, FILE_WRITE);
    if (f) {
        for (auto& u : list) { f.print(u.c_str()); f.println(); }
        f.close();
    }
}

namespace {
constexpr const char* kFavorites = "/browser/favorites.txt";
}

void addFavorite(const std::string& url) {
    auto list = favorites(50);
    if (!list.empty() && list[0] == url) return;
    list.insert(list.begin(), url);
    if (list.size() > 10) list.resize(10);
    File f = SD.open(kFavorites, FILE_WRITE);
    if (f) {
        for (auto& u : list) { f.print(u.c_str()); f.println(); }
        f.close();
    }
}

std::vector<std::string> favorites(int max) {
    std::vector<std::string> out;
    File f = SD.open(kFavorites, FILE_READ);
    if (!f) return out;
    std::string line;
    while (f.available() && out.size() < (size_t)max) {
        line.clear();
        char c;
        while (f.readBytes(&c, 1) && c != '\n') line += c;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    f.close();
    return out;
}

bool isFavorite(const std::string& url) {
    for (auto& u : favorites(50)) if (u == url) return true;
    return false;
}

std::vector<std::string> urls(int max) {
    std::vector<std::string> out;
    File f = SD.open(kHistory, FILE_READ);
    if (!f) return out;
    std::string line;
    while (f.available() && out.size() < (size_t)max) {
        line.clear();
        char c;
        while (f.readBytes(&c, 1) && c != '\n') line += c;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    f.close();
    return out;
}

bool cachePut(const std::string& url, const std::string& html) {
    if (html.size() > 200 * 1024) return false;  // don't bloat the card
    std::string path = std::string("/browser/cache/") + hashOf(url) + ".html";
    File f = SD.open(path.c_str(), FILE_WRITE);
    if (!f) return false;
    f.write((const uint8_t*)html.data(), html.size());
    f.close();
    return true;
}

bool cacheGet(const std::string& url, std::string& html) {
    std::string path = std::string("/browser/cache/") + hashOf(url) + ".html";
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return false;
    html.reserve(min((size_t)256 * 1024, f.size()));
    uint8_t buf[1024];
    while (f.available()) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        html.append((char*)buf, n);
    }
    f.close();
    return !html.empty();
}

namespace {
constexpr const char* kCookies = "/browser/cookies.txt";
}

bool cookiesLoad(std::vector<std::string>& rows) {
    rows.clear();
    File f = SD.open(kCookies, FILE_READ);
    if (!f) return false;
    std::string line;
    while (f.available()) {
        line.clear();
        char c;
        while (f.readBytes(&c, 1) && c != '\n') line += c;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) rows.push_back(line);
    }
    f.close();
    return true;
}

void cookiesSave(const std::vector<std::string>& rows) {
    File f = SD.open(kCookies, FILE_WRITE);
    if (!f) return;
    for (auto& r : rows) { f.print(r.c_str()); f.println(); }
    f.close();
}

void cookiesClear() {
    if (SD.exists(kCookies)) SD.remove(kCookies);
}

}  // namespace store
