// Background image pipeline: download JPG/PNG one-by-one to the SD cache,
// decode + downscale to a 16-bit BMP (<= 240x180). Runs on a FreeRTOS task on
// core 1 so the UI (core 0) stays responsive. See TASK.md / IDEAS.md.
#pragma once
#include <Arduino.h>
#include <string>
#include <vector>
#include <cstdint>

namespace img {

// Per-format download caps (checked by URL extension before fetching).
constexpr int kMaxJpegBytes = 512 * 1024;  // JPEG (esp_new_jpeg decodes at target size, so bigger is fine)
constexpr int kMaxPngBytes  = 128 * 1024;  // PNG (PNGdec buffers full-res in PSRAM, so keep it small)
constexpr int kMaxPixels = 1500000;        // skip images whose full-res buffer is huge
constexpr int kMaxDim    = 4096;           // hard cap on a single dimension
constexpr int kOutW = 240, kOutH = 180;    // max rendered size (proportional)

void begin();                                          // make /browser/img + spawn worker task
void queue(const std::vector<std::string>& urls);      // enqueue (document order, dedup)
void clear();                                          // cancel in-flight + drop pending

int  total();
int  done();
bool busy();
const char* statusText();                              // e.g. "3/5", "done", "idle"

bool        ready(const std::string& url);             // cached BMP present?
std::string cachePath(const std::string& url);         // /browser/img/<hash>.bmp
void        listDir(std::string& out);                 // "name:size bytes; ..." of cached BMPs
/// Load a cached 16-bit BMP into `out` (row-major RGB565, w*h). false if absent/bad.
bool loadBmp(const std::string& url, std::vector<uint16_t>& out, int& w, int& h);

}  // namespace img
