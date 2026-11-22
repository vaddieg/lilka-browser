#include "store.h"
#include "hw.h"   // initSD(): SD on the shared SPI bus (must match the display bus)
#include <cstring>
#include <mbedtls/md5.h>

namespace store {

namespace {
constexpr const char* kDir = "/browser";
constexpr const char* kSettings = "/browser/settings.txt";
constexpr const char* kHistory = "/browser/history.txt";

bool ensureDir() {
    bool ok = true;
    if (!SD.exists(kDir)) ok = SD.mkdir(kDir);
    if (!SD.exists("/browser/cache")) ok = SD.mkdir("/browser/cache") && ok;
    return ok;
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
    // Use the shared-SPI-bus init (correct CS + SPIClass + freq), not the
    // no-arg SD.begin() which does not bind the bus the display already owns.
    if (!initSD()) { Serial.printf("[store] SD init FAILED\n"); return false; }
    Serial.printf("[store] SD ok, total=%u used=%u\n", (unsigned)SD.totalBytes(), (unsigned)SD.usedBytes());
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

// Cache file layout (deterministic): two validator header lines, a blank
// separator line, then the raw body. etag / last-modified never contain tabs
// or newlines in practice. Missing validators are stored as empty values.
bool cachePut(const std::string& url, const std::string& html,
              const std::string& etag, const std::string& lastModified) {
    if (html.size() > 512 * 1024) return false;  // don't bloat the card
    if (!SD.exists("/browser/cache")) SD.mkdir("/browser/cache");
    std::string path = std::string("/browser/cache/") + hashOf(url) + ".html";
    File f = SD.open(path.c_str(), FILE_WRITE);
    if (!f) { Serial.printf("[store] cachePut open FAIL %s\n", path.c_str()); return false; }
    Serial.printf("[store] cachePut %s (%u bytes, etag='%s')\n", path.c_str(), (unsigned)html.size(), etag.c_str());
    f.print("ETAG\t"); f.print(etag.c_str()); f.print("\n");
    f.print("LM\t");   f.print(lastModified.c_str()); f.print("\n");
    f.print("\n");
    f.write((const uint8_t*)html.data(), html.size());
    f.close();
    return true;
}

bool cacheGet(const std::string& url, std::string& html, std::string* etag, std::string* lastModified) {
    std::string path = std::string("/browser/cache/") + hashOf(url) + ".html";
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return false;
    if (etag) etag->clear();
    if (lastModified) lastModified->clear();
    std::string all;
    all.reserve(min((size_t)256 * 1024, f.size()));
    uint8_t buf[1024];
    while (f.available()) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        all.append((char*)buf, n);
    }
    f.close();
    // body starts after the first blank line ("\n\n")
    size_t sep = all.find("\n\n");
    if (sep == std::string::npos) { html = all; return !html.empty(); }
    const std::string head = all.substr(0, sep);
    html = all.substr(sep + 2);
    // parse the two validator lines
    size_t nl = head.find('\n');
    std::string l1 = head.substr(0, nl);
    std::string l2 = (nl == std::string::npos) ? "" : head.substr(nl + 1);
    if (l1.rfind("ETAG\t", 0) == 0 && etag) *etag = l1.substr(5);
    if (l2.rfind("LM\t", 0) == 0 && lastModified) *lastModified = l2.substr(3);
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
