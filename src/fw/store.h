// SD storage: /browser/settings.txt (key=value), /browser/history.txt (URLs,
// newest first, capped), /browser/cache/ (page + image cache, later).
#pragma once
#include <Arduino.h>
#include <SD.h>
#include <string>
#include <vector>

namespace store {

bool begin();

/// Settings (plain key=value, one per line).
std::string get(const std::string& key, const std::string& fallback = "");
void set(const std::string& key, const std::string& value);

/// History: newest first, max `cap` entries, dedup.
void addUrl(const std::string& url);
std::vector<std::string> urls(int max = 10);

/// Favorites: /browser/favorites.txt, newest first, dedup, capped.
void addFavorite(const std::string& url);
std::vector<std::string> favorites(int max = 10);
bool isFavorite(const std::string& url);

/// Cache helpers (page cache keyed by URL hash; stores body + validators).
bool cachePut(const std::string& url, const std::string& html,
              const std::string& etag = "", const std::string& lastModified = "");
bool cacheGet(const std::string& url, std::string& html,
              std::string* etag = nullptr, std::string* lastModified = nullptr);

/// Cookie jar persistence: /browser/cookies.txt (domain<TAB>name=value per line).
bool cookiesLoad(std::vector<std::string>& rows);
void cookiesSave(const std::vector<std::string>& rows);
void cookiesClear();

/// File downloads: /browser/downloads/<name>
struct FileEntry {
    std::string name;
    size_t size = 0;
};
bool saveDownload(const std::string& name, const std::string& data);
std::vector<FileEntry> downloads(int max = 20);
bool deleteDownload(const std::string& name);

}  // namespace store
