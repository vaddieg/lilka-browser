// Background image pipeline (see img.h). Runs on core 1.
#include "img.h"
#include "net.h"
#include <SD.h>
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <esp_jpeg_dec.h>   // esp_new_jpeg: decode JPEG directly to a target size (RGB565)
#include <JPEGDEC.h>
// NOTE: PNGdec's internal line buffer (ucPixels) is sized by PNG_MAX_BUFFERED_PIXELS,
// which must be defined GLOBALLY (build_flags) so the library itself is compiled with
// it — it needs to hold two lines (current + previous) for unfiltering. See decodePng().
#include <PNGdec.h>
#include <cstring>
#include <cmath>
#include <set>

namespace img {

namespace {
constexpr const char* kDir = "/browser/img";

// FNV-1a 64 -> 16 hex chars, deterministic per URL.
std::string hash16(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char out[17];
    snprintf(out, sizeof out, "%016llx", (unsigned long long)h);
    return out;
}

bool isJpeg(const std::string& d) { return d.size() >= 2 && (unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xD8; }
bool isPng(const std::string& d)  {
    static const uint8_t sig[4] = {0x89, 0x50, 0x4E, 0x47};
    return d.size() >= 4 && std::memcmp(d.data(), sig, 4) == 0;
}

// Which download cap applies to a URL, by its path extension (.jpg/.jpeg vs .png).
size_t urlMaxBytes(const std::string& url) {
    std::string low; for (char ch : url) low += (char)tolower((unsigned char)ch);
    size_t q = low.find_first_of("?#");
    std::string p = (q == std::string::npos) ? low : low.substr(0, q);
    bool jpeg = (p.size() > 4 && p.compare(p.size() - 4, 4, ".jpg") == 0)
             || (p.size() > 5 && p.compare(p.size() - 5, 5, ".jpeg") == 0);
    return jpeg ? kMaxJpegBytes : kMaxPngBytes;
}

// --- 16-bit BMP ---
static void w16(fs::File& f, uint16_t v) { f.write((uint8_t*)&v, 2); }
static void w32(fs::File& f, uint32_t v) { f.write((uint8_t*)&v, 4); }

bool writeBmp16(const std::string& path, const uint16_t* rgb565, int w, int h) {
    int rowBytes = w * 2;
    int rowPad = (4 - (rowBytes % 4)) % 4;
    size_t pixelBytes = (size_t)(rowBytes + rowPad) * h;
    size_t fileSize = 14 + 40 + pixelBytes;

    fs::File f = SD.open(path.c_str(), FILE_WRITE);
    if (!f) return false;
    f.write((const uint8_t*)"BM", 2);
    w32(f, (uint32_t)fileSize);
    w16(f, 0); w16(f, 0);
    w32(f, 14 + 40);
    w32(f, 40);                 // info header size
    w32(f, (uint32_t)w);
    w32(f, (uint32_t)h);
    w16(f, 1);
    w16(f, 16);                 // 16 bpp
    w32(f, 0);                  // BI_RGB
    w32(f, (uint32_t)pixelBytes);
    w32(f, 2835); w32(f, 2835); // ~110 dpi
    w32(f, 0); w32(f, 0);
    uint8_t pad[3] = {0, 0, 0};
    for (int y = h - 1; y >= 0; y--) {           // bottom-up
        f.write((const uint8_t*)rgb565 + (size_t)y * rowBytes, (size_t)rowBytes);
        if (rowPad) f.write(pad, (size_t)rowPad);
    }
    f.close();
    return true;
}

// --- decode helpers (buffers in PSRAM) ---
struct PngCtx { uint16_t* out; int w; int h; };
static PngCtx g_png;
struct JpgCtx { uint16_t* out; int w; int h; };
static JpgCtx g_jpg;

// JpegDEC calls this per MCU block; copy into the full buffer (bounds-checked).
static int jpegDraw(JPEGDRAW* d) {
    uint16_t* out = g_jpg.out; int W = g_jpg.w, H = g_jpg.h;
    int bw = d->iWidth, bh = d->iHeight;
    const uint16_t* src = d->pPixels;
    for (int r = 0; r < bh; r++) {
        int y = d->y + r; if (y < 0) continue; if (y >= H) break;
        for (int c = 0; c < bw; c++) {
            int x = d->x + c; if (x < 0) continue; if (x >= W) break;
            out[(size_t)y * W + x] = src[r * bw + c];
        }
    }
    return 1;  // 1 = continue
}

static int pngDraw(PNGDRAW* d) {
    int w = d->iWidth, bpp = d->iBpp, type = d->iPixelType;
    const uint8_t* p = d->pPixels;
    const uint8_t* pal = d->pPalette;
    uint16_t* out = g_png.out;
    int y = d->y;
    for (int x = 0; x < w; x++) {
        int r = 0, g = 0, b = 0;
        if (type == 0) { int v = (bpp == 16) ? p[x * 2] : p[x]; r = g = b = v; }
        else if (type == 2) {
            if (bpp == 16) { r = p[x * 6]; g = p[x * 6 + 2]; b = p[x * 6 + 4]; }
            else { r = p[x * 3]; g = p[x * 3 + 1]; b = p[x * 3 + 2]; }
        } else if (type == 3) {
            int idx = p[x]; if (pal) { r = pal[idx * 3]; g = pal[idx * 3 + 1]; b = pal[idx * 3 + 2]; }
        } else if (type == 4) { int v = (bpp == 16) ? p[x * 4] : p[x * 2]; r = g = b = v; }
        else if (type == 6) {
            if (bpp == 16) { r = p[x * 8]; g = p[x * 8 + 2]; b = p[x * 8 + 4]; }
            else { r = p[x * 4]; g = p[x * 4 + 1]; b = p[x * 4 + 2]; }
        }
        out[y * w + x] = (uint16_t)((r << 11) | (g << 5) | b);
    }
    return 1;  // 1 = continue decoding (0 = quit early)
}

// --- JPEG decode (esp_new_jpeg, direct to target size; JpegDEC as fallback) ---

// Fallback: JpegDEC (handles progressive JPEG, which esp_new_jpeg does not).
// Decodes full-res into a PSRAM buffer; caller downscales afterwards.
static uint16_t* decodeJpegFallback(const std::string& data, int& w, int& h) {
    JPEGDEC* j = new JPEGDEC();
    if (j->openRAM((uint8_t*)data.data(), (int)data.size(), jpegDraw) == 0) { Serial.printf("[img] jpeg(openRAM) fail\n"); delete j; return nullptr; }
    int sw = j->getWidth(), sh = j->getHeight();
    if (sw <= 0 || sh <= 0 || (long)sw * sh > kMaxPixels || sw > kMaxDim || sh > kMaxDim) { Serial.printf("[img] jpeg too big %dx%d\n", sw, sh); j->close(); delete j; return nullptr; }
    uint16_t* buf = (uint16_t*)heap_caps_malloc((size_t)sw * sh * 2, MALLOC_CAP_SPIRAM);
    if (!buf) { j->close(); delete j; return nullptr; }
    g_jpg.out = buf; g_jpg.w = sw; g_jpg.h = sh;
    int rc = j->decode(0, 0, 0);
    j->close(); delete j;
    if (rc == 0) { Serial.printf("[img] jpeg decode fail\n"); heap_caps_free(buf); return nullptr; }
    w = sw; h = sh;
    return buf;
}

// esp_new_jpeg: decode a baseline JPEG directly to a target resolution (<= kOutW x kOutH
// where possible) as RGB565, so the PSRAM buffer is only the target size. Falls back to
// JpegDEC if esp_new_jpeg can't handle the file (e.g. progressive JPEG).
static uint16_t* decodeJpeg(const std::string& data, int& w, int& h) {
    // 1) probe the source dimensions
    jpeg_dec_config_t probe = DEFAULT_JPEG_DEC_CONFIG();
    probe.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&probe, &dec) != JPEG_ERR_OK) return decodeJpegFallback(data, w, h);
    jpeg_dec_io_t io = { (uint8_t*)data.data(), (int)data.size(), (int)data.size(), nullptr, 0 };
    jpeg_dec_header_info_t info = {};
    bool ok = (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK && info.width > 0 && info.height > 0);
    jpeg_dec_close(dec);
    if (!ok) { Serial.printf("[img] esp jpeg: header fail\n"); return decodeJpegFallback(data, w, h); }
    int sw = info.width, sh = info.height;
    // esp_new_jpeg decodes *and* scales during the pass, so large sources are
    // fine (it never holds a full-res buffer) — the real bound is one dimension.
    if (sw > kMaxDim || sh > kMaxDim) { Serial.printf("[img] esp jpeg: too big %dx%d\n", sw, sh); return decodeJpegFallback(data, w, h); }

    // 2) pick the scale. esp_new_jpeg can shrink by at most 1/8 and requires the
    //    output dims to be multiples of 8. If the source is already small, skip scale.
    uint16_t scW = 0, scH = 0;
    int outW = sw, outH = sh;
    if (sw > kOutW || sh > kOutH) {
        double ratio = std::min((double)kOutW / sw, (double)kOutH / sh);
        if (ratio < 1.0 / 8.0) ratio = 1.0 / 8.0;
        // esp_new_jpeg: output must be a multiple of 8 AND >= source/8 (min scale).
        // Round the target UP to a multiple of 8, then bump to the 1/8 minimum.
        auto tgt = [](int src, double r) {
            int v = (int)(src * r);
            int minv = (src + 7) / 8;             // min allowed (1/8), rounded up
            v = (v + 7) / 8 * 8;                    // multiple of 8
            if (v < minv) v = (minv + 7) / 8 * 8;
            if (v < 8) v = 8;
            if (v > src) v = src;
            return v;
        };
        int tw = tgt(sw, ratio), th = tgt(sh, ratio);
        scW = (uint16_t)tw; scH = (uint16_t)th; outW = tw; outH = th;
    }

    // 3) open with the target scale and decode into a PSRAM, 16-byte-aligned buffer
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.scale.width = scW; cfg.scale.height = scH;
    jpeg_dec_handle_t dec2 = nullptr;
    if (jpeg_dec_open(&cfg, &dec2) != JPEG_ERR_OK) return decodeJpegFallback(data, w, h);
    // parse_header is required before process; do it on the scaled handle too.
    jpeg_dec_io_t io2 = { (uint8_t*)data.data(), (int)data.size(), (int)data.size(), nullptr, 0 };
    if (jpeg_dec_parse_header(dec2, &io2, &info) != JPEG_ERR_OK) { jpeg_dec_close(dec2); return decodeJpegFallback(data, w, h); }
    int outLen = 0;
    if (jpeg_dec_get_outbuf_len(dec2, &outLen) != JPEG_ERR_OK || outLen <= 0) { jpeg_dec_close(dec2); return decodeJpegFallback(data, w, h); }
    uint8_t* buf = (uint8_t*)heap_caps_aligned_alloc(16, (size_t)outLen, MALLOC_CAP_SPIRAM);
    if (!buf) { Serial.printf("[img] esp jpeg: alloc %d\n", outLen); jpeg_dec_close(dec2); return decodeJpegFallback(data, w, h); }
    io2.outbuf = buf; io2.out_size = outLen;
    jpeg_error_t rc = jpeg_dec_process(dec2, &io2);
    jpeg_dec_close(dec2);
    if (rc != JPEG_ERR_OK) {
        Serial.printf("[img] esp jpeg: process rc=%d -> fallback\n", (int)rc);
        heap_caps_free(buf);
        return decodeJpegFallback(data, w, h);
    }
    Serial.printf("[img] esp jpeg %dx%d -> %dx%d\n", sw, sh, outW, outH);
    w = outW; h = outH;
    return (uint16_t*)buf;
}

// --- PNG decode (PNGdec, full-res into PSRAM) ---
static uint16_t* decodePng(const std::string& data, int& w, int& h) {
    PNG* p = new PNG();
    if (p->openRAM((uint8_t*)data.data(), (int)data.size(), pngDraw) != 0) { Serial.printf("[img] png openRAM fail\n"); delete p; return nullptr; }
    int sw = p->getWidth(), sh = p->getHeight();
    Serial.printf("[img] png dims %dx%d\n", sw, sh);
    // PNGdec's line buffer must hold 2 lines; cap width to what it safely holds
    // (worst case RGBA = 4 bytes/px). PNGdec's own check is 2x too permissive.
    int maxW = (PNG_MAX_BUFFERED_PIXELS / 2 - 16) / 4;
    if (sw <= 0 || sh <= 0 || sw > maxW || (long)sw * sh > kMaxPixels || sh > kMaxDim) {
        Serial.printf("[img] png too big %dx%d (max %d)\n", sw, sh, maxW);
        p->close(); delete p; return nullptr;
    }
    uint16_t* buf = (uint16_t*)heap_caps_malloc((size_t)sw * sh * 2, MALLOC_CAP_SPIRAM);
    if (!buf) { p->close(); delete p; return nullptr; }
    g_png.out = buf; g_png.w = sw; g_png.h = sh;
    int rc = p->decode(nullptr, PNG_CHECK_CRC);
    p->close(); delete p;
    if (rc != 0) { Serial.printf("[img] png decode rc=%d\n", rc); heap_caps_free(buf); return nullptr; }
    w = sw; h = sh;
    return buf;
}

/// Decode a downloaded image into a PSRAM RGB565 buffer (w*h). JPEG is decoded
/// directly to ~target size by esp_new_jpeg; PNG is decoded full-res. nullptr on failure.
static uint16_t* decodeAny(const std::string& data, int& w, int& h) {
    if (isJpeg(data)) return decodeJpeg(data, w, h);
    if (isPng(data))  return decodePng(data, w, h);
    return nullptr;
}

/// Downscale (3x3 area average) to <= kOutW x kOutH, preserving aspect.
static void downscale(const uint16_t* src, int sw, int sh, uint16_t* dst, int tw, int th) {
    auto px = [&](int x, int y) -> uint16_t {
        if (x < 0) x = 0; else if (x >= sw) x = sw - 1;
        if (y < 0) y = 0; else if (y >= sh) y = sh - 1;
        return src[(size_t)y * sw + x];
    };
    for (int y = 0; y < th; y++) {
        int cy = (int)(((long)(y + 1) * sh) / th / 2);
        for (int x = 0; x < tw; x++) {
            int cx = (int)(((long)(x + 1) * sw) / tw / 2);
            long r = 0, g = 0, b = 0, n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    uint16_t p = px(cx + dx, cy + dy);
                    r += (p >> 11) & 31; g += (p >> 5) & 63; b += p & 31; n++;
                }
            dst[(size_t)y * tw + x] = (uint16_t)(((int)(r / n) << 11) | ((int)(g / n) << 5) | (int)(b / n));
        }
    }
}

// --- worker state ---
struct Job { std::string url; unsigned gen; };
std::vector<Job> g_jobs;
SemaphoreHandle_t g_mu = nullptr;
SemaphoreHandle_t g_sem = nullptr;
std::atomic<int> g_total{0};
std::atomic<int> g_done{0};
std::atomic<bool> g_busy{false};
std::atomic<unsigned> g_gen{0};   // bumped on every clear()/queue(); stale jobs are dropped
std::atomic<uint32_t> g_lastTick{0};  // worker heartbeat; watchdog reboots if it wedges
std::set<std::string> g_failed;       // URLs that failed this generation (guarded by g_mu)
bool g_started = false;

std::string cachePathFor(const std::string& url) { return std::string(kDir) + "/" + hash16(url) + ".bmp"; }

/// Download + decode + cache one image. Returns true iff a BMP is now available
/// (the caller records failures so the UI can mark the placeholder distinctly).
bool processOne(const std::string& url) {
    // The worker runs off the UI flow, so it must bring the radio up itself.
    // Without a connected STA the first netconn DNS/connect from this background
    // task hits an uninitialized lwIP core mutex and asserts (cold-boot crash).
    if (!net::ensureWifi()) { Serial.printf("[img] skip %s: no wifi\n", url.c_str()); return false; }
    size_t maxB = urlMaxBytes(url);
    std::string data, ct, finalUrl, err;
    net::Client c;
    if (!c.fetchBinary(url, data, ct, maxB, finalUrl, err, 20000)) {
        Serial.printf("[img] skip %s: %s\n", url.c_str(), err.c_str());
        return false;
    }
    int sw = 0, sh = 0;
    uint16_t* buf = decodeAny(data, sw, sh);
    if (!buf) { Serial.printf("[img] decode fail %s (unsupported/too big)\n", url.c_str()); return false; }

    std::string path = cachePathFor(url);
    // esp_new_jpeg already returns a target-size buffer; only downscale if still too big
    // (e.g. a huge image clamped to 1/8 by the decoder, or a full-res PNG).
    if (sw <= kOutW && sh <= kOutH) {
        bool ok = writeBmp16(path, buf, sw, sh);
        Serial.printf("[img] %s %s -> %dx%d BMP\n", ok ? "ok" : "FAIL", url.c_str(), sw, sh);
        heap_caps_free(buf);
        return ok;
    }
    double scale = std::min((double)kOutW / sw, (double)kOutH / sh);
    if (scale > 1.0) scale = 1.0;
    int tw = (int)(sw * scale), th = (int)(sh * scale);
    if (tw < 1) tw = 1; if (th < 1) th = 1;
    uint16_t* small = (uint16_t*)heap_caps_malloc((size_t)tw * th * 2, MALLOC_CAP_SPIRAM);
    if (!small) { Serial.printf("[img] alloc fail %s\n", url.c_str()); heap_caps_free(buf); return false; }
    downscale(buf, sw, sh, small, tw, th);
    heap_caps_free(buf);
    bool ok = writeBmp16(path, small, tw, th);
    Serial.printf("[img] %s %s -> %dx%d BMP\n", ok ? "ok" : "FAIL", url.c_str(), tw, th);
    heap_caps_free(small);
    return ok;
}

void markFailed(const std::string& url) {
    if (!g_mu) return;
    xSemaphoreTake(g_mu, portMAX_DELAY);
    g_failed.insert(url);
    xSemaphoreGive(g_mu);
}

void worker(void* arg) {
    for (;;) {
        g_lastTick = millis();
        // Wake on a nudge OR a short bounded poll. We deliberately do NOT use one
        // signal-per-job (a lost/drifted count made the worker wait forever on the
        // last image). Instead: wait briefly, then drain EVERY pending job.
        if (g_sem) xSemaphoreTake(g_sem, pdMS_TO_TICKS(200));
        g_lastTick = millis();
        for (;;) {
            Job job; bool has = false;
            if (g_mu) xSemaphoreTake(g_mu, portMAX_DELAY);
            if (!g_jobs.empty()) { job = std::move(g_jobs.front()); g_jobs.erase(g_jobs.begin()); has = true; }
            if (g_mu) xSemaphoreGive(g_mu);
            if (!has) break;
            if (job.gen != g_gen.load()) continue;   // stale (a newer page superseded it)
            Serial.printf("[img] -> %s\n", job.url.c_str());
            g_lastTick = millis();
            bool ok = processOne(job.url);
            if (!ok) markFailed(job.url);
            g_done++;
            vTaskDelay(pdMS_TO_TICKS(120));  // be gentle to servers (avoid 429s)
            g_lastTick = millis();
        }
    }
}

// Safety net: a wedged worker (blocking read that ignores the socket timeout)
// would otherwise kill all future image loading. If it hasn't heartbeated in a
// while while work is still pending, reboot to recover.
void watchdog(void* arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (g_total.load() > g_done.load() && (millis() - g_lastTick.load() > 50000)) {
            Serial.printf("[img] worker wedged (no progress >50s) — rebooting\n");
            delay(200);
            ESP.restart();
        }
    }
}

}  // namespace

void begin() {
    if (g_started) return;
    g_started = true;
    if (!SD.exists(kDir)) SD.mkdir(kDir);
    if (!g_mu)  g_mu  = xSemaphoreCreateMutex();
    // counting semaphore (max 256 jobs) so one signal per queued image is honoured
    if (!g_sem) g_sem = xSemaphoreCreateCounting(256, 0);
    xTaskCreatePinnedToCore(worker, "imgworker", 40960, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(watchdog, "imgwdog", 4096, nullptr, 1, nullptr, 0);
    Serial.printf("[img] worker started\n");
}

void queue(const std::vector<std::string>& urls) {
    unsigned gen = g_gen.fetch_add(1) + 1;  // invalidate any prior generation
    if (g_mu) xSemaphoreTake(g_mu, portMAX_DELAY);
    g_jobs.clear();
    g_failed.clear();  // fresh page: drop stale failure markers
    std::vector<std::string> seen;
    for (const auto& u : urls) {
        bool dup = false;
        for (const auto& s : seen) if (s == u) { dup = true; break; }
        if (!dup) { g_jobs.push_back({u, gen}); seen.push_back(u); }
    }
    if (g_mu) xSemaphoreGive(g_mu);
    g_total = (int)g_jobs.size();
    g_done = 0;
    g_busy = !g_jobs.empty();
    Serial.printf("[img] queued %d image(s) (gen=%u)\n", (int)g_jobs.size(), (unsigned)gen);
    for (int i = 0; i < (int)g_jobs.size(); i++) if (g_sem) xSemaphoreGive(g_sem);
}

void clear() {
    g_gen.fetch_add(1);   // mark all pending jobs stale
    net::requestCancel(); // abort an in-flight fetch promptly (checked per chunk)
    if (g_mu) xSemaphoreTake(g_mu, portMAX_DELAY);
    g_jobs.clear();
    if (g_mu) xSemaphoreGive(g_mu);
    g_busy = false;
}

int  total() { return g_total.load(); }
int  done()  { return g_done.load(); }
bool busy()  { return g_total.load() > g_done.load(); }

const char* statusText() {
    static char buf[24];
    int t = g_total.load(), d = g_done.load();
    if (t == 0) snprintf(buf, sizeof buf, "no images");
    else if (d < t) snprintf(buf, sizeof buf, "img %d/%d", d, t);
    else snprintf(buf, sizeof buf, "img %d/%d ok", d, t);
    return buf;
}

std::string cachePath(const std::string& url) { return cachePathFor(url); }

bool ready(const std::string& url) { return SD.exists(cachePathFor(url).c_str()); }

/// True if this URL was queued for this page but failed to download/decode.
bool failed(const std::string& url) {
    if (!g_mu) return false;
    xSemaphoreTake(g_mu, portMAX_DELAY);
    bool f = g_failed.count(url) > 0;
    xSemaphoreGive(g_mu);
    return f;
}

void listDir(std::string& out) {
    fs::File d = SD.open(kDir);
    if (!d) { out = "(no dir)"; return; }
    int n = 0;
    for (fs::File f = d.openNextFile(); f; f = d.openNextFile()) {
        if (n++) out += "; ";
        out += f.name();
        out += ":";
        out += std::to_string(f.size());
    }
    if (n == 0) out = "(empty)";
    d.close();
}

bool loadBmp(const std::string& url, std::vector<uint16_t>& out, int& w, int& h) {
    std::string path = cachePathFor(url);
    fs::File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return false;
    if (f.size() < 54) { f.close(); return false; }
    uint8_t hdr[54];
    if (f.read(hdr, 54) != 54) { f.close(); return false; }
    if (hdr[0] != 'B' || hdr[1] != 'M') { f.close(); return false; }
    uint32_t off = hdr[10] | (hdr[11] << 8) | (hdr[12] << 16) | (hdr[13] << 24);
    w = (int)(hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | (hdr[21] << 24));
    h = (int)(hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24));
    uint16_t bpp = (uint16_t)(hdr[28] | (hdr[29] << 8));
    if (w <= 0 || h <= 0 || bpp != 16) { f.close(); return false; }
    out.resize((size_t)w * h);
    // rows are bottom-up; read them into the vector top-down
    size_t rowBytes = (size_t)w * 2;
    size_t rowSize = (rowBytes + 3) & ~((size_t)3);
    f.seek(off);
    uint8_t* row = (uint8_t*)heap_caps_malloc(rowSize, MALLOC_CAP_SPIRAM);
    if (!row) { f.close(); return false; }
    for (int y = h - 1; y >= 0; y--) {
        if (f.read(row, rowSize) != (int)rowSize) { heap_caps_free(row); f.close(); return false; }
        std::memcpy(out.data() + (size_t)y * w, row, rowBytes);
    }
    heap_caps_free(row);
    f.close();
    return true;
}

}  // namespace img
