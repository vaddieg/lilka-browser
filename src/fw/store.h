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

/// Cache helpers (v1: raw page cache keyed by URL hash).
bool cachePut(const std::string& url, const std::string& html);
bool cacheGet(const std::string& url, std::string& html);

}  // namespace store
