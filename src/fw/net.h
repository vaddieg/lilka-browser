// HTTP/HTTPS client for the Lilka browser (ESP32-S3).
// - GET with redirects (301/302/303/307/308), relative Location resolution
// - gzip/deflate response decoding (miniz)
// - session-only cookie jar
// - URL rewrites for compatibility/lite pages (e.g. Google Search gbv=1)
// - body buffered in PSRAM (std::string, allocator override)
#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <memory>
#include <string>
#include <vector>

namespace net {

struct Url {
    std::string scheme = "https";
    std::string host;
    int port = 0;
    std::string path = "/";
    std::string toString() const;
};

/// Parse an absolute URL. Returns false on garbage.
bool parseUrl(const std::string& in, Url& out);

/// Rewrite URLs for compatibility/lite modes (e.g. Google Search gbv=1).
std::string rewriteUrl(const std::string& url);

/// Decode a gzip/zlib body (miniz). Pass-through if not compressed.
/// `encoding` is the Content-Encoding header value (may be empty).
bool inflateBody(const std::string& raw, const std::string& encoding, std::string& out, std::string& err);

/// Last raw (pre-inflate) response body — kept for debug commands.
const std::string& lastRawBody();

/// Declared charset from the last response (may be empty).
const std::string& lastCharset();

/// Main MIME type of the last response ("text/html", "application/pdf", …;
/// empty if none).
const std::string& lastContentType();

/// ETag / Last-Modified from the last 200 response (may be empty).
const std::string& lastEtag();
const std::string& lastModified();

/// True if the last GET answered 304 Not Modified (body is empty; reuse cache).
bool lastNotModified();

// --- Concurrency (background image worker on core 1) ---
/// Global lock: only one network request in flight at a time (page fetch vs.
/// image worker). Public fetch methods hold this for the duration.
struct NetGuard {
    NetGuard();
    ~NetGuard();
    NetGuard(const NetGuard&) = delete;
    NetGuard& operator=(const NetGuard&) = delete;
};
/// Ask any in-flight background fetch to abort promptly (checked per chunk).
void requestCancel();
bool cancelled();

/// Resolve `ref` against `base` (RFC 3986-ish, no fragments).
std::string resolve(const Url& base, const std::string& ref);

// --- Session Cookie Jar ---
void setCookie(const std::string& domain, const std::string& name, const std::string& value);
std::string getCookieHeader(const std::string& host);
void parseSetCookie(const std::string& host, const std::string& headerVal);
void clearCookies();
void loadCookieJar();   // call after store::begin()
size_t cookieCount();
std::string dumpCookies();

class Client {
public:
    /// GET `url`. Follows up to `maxRedirects` redirects.
    /// On success: outBody holds decoded HTML (PSRAM), outFinalUrl the last URL.
    /// Returns false with a short human reason in `err`.
    bool get(const std::string& url, std::string& outBody, std::string& outFinalUrl,
             std::string& err, int timeoutMs = 15000, int maxRedirects = 5,
             const std::string& userAgent =
                 "Lilka/1.0 (ESP32-S3; browser; +https://github.com/lilka-dev/lilka)");

    /// GET with conditional revalidation: sends If-None-Match / If-Modified-Since.
    /// On 304 the body is left empty and lastNotModified() is true.
    bool getCond(const std::string& url, std::string& outBody, std::string& outFinalUrl,
                 std::string& err, const std::string& ifNoneMatch, const std::string& ifModifiedSince,
                 int timeoutMs = 15000, int maxRedirects = 5,
                 const std::string& userAgent =
                     "Lilka/1.0 (ESP32-S3; browser; +https://github.com/lilka-dev/lilka)");

    /// Fetch a binary (image) into `out` (PSRAM), streaming with a hard size cap.
    /// Only accepts image/jpeg / image/png; aborts (returns false) on other types,
    /// oversize bodies, or a concurrent requestCancel(). Follows a few redirects.
    /// `outContentType` receives the (lowercased) main MIME type.
    bool fetchBinary(const std::string& url, std::string& out, std::string& outContentType,
                     long maxSize, std::string& outFinalUrl, std::string& err,
                     int timeoutMs = 20000, int maxRedirects = 4);

    /// POST `form` (application/x-www-form-urlencoded) to `url`.
    /// Redirects are followed with GET (browser behavior for 301/302/303).
    bool post(const std::string& url, const std::string& form,
              std::string& outBody, std::string& outFinalUrl, std::string& err,
              int timeoutMs = 15000, int maxRedirects = 5,
              const std::string& userAgent =
                  "Lilka/1.0 (ESP32-S3; browser; +https://github.com/lilka-dev/lilka)");

private:
    bool open(const Url& u, int timeoutMs, WiFiClientSecure& client, std::string& err);

    // ---- shared request plumbing (used by get/post AND the streaming download) ----
    struct Headers {
        int status = 0;
        bool chunked = false;
        long contentLength = -1;
        std::string encoding;      // Content-Encoding (gzip/zlib) if any
        std::string contentType;   // raw Content-Type
        std::string location;      // redirect target if 3xx
    };
    /// Open the connection, send the request, read status + headers. The client
    /// is left OPEN for the caller to read the body.
    bool sendRequest(const Url& u, WiFiClientSecure& client, Headers& h, std::string& err,
                     int timeoutMs, const std::string& userAgent,
                     const std::string& method = "GET", const std::string& postBody = "",
                     const std::string& ifNoneMatch = "", const std::string& ifModifiedSince = "");
    /// Read a response body into `out`, honoring chunked / content-length; decompresses gzip.
    bool readBody(WiFiClientSecure& client, const Headers& h, std::string& out, std::string& err, int timeoutMs);

    /// One request: fills body (decoded), location header, status code; handles gzip.
    /// `method` is "GET" or "POST"; `postBody` sent as-is for POST.
    bool request(const Url& u, std::string& body, std::string& location,
                 std::string& err, int timeoutMs, const std::string& userAgent, int& status,
                 const std::string& method = "GET", const std::string& postBody = "",
                 const std::string& ifNoneMatch = "", const std::string& ifModifiedSince = "");

public:
    /// Stream a binary file straight to SD (no PSRAM buffering of the body).
    /// Follows redirects. Returns the final byte count, or:
    ///   -1  on error (see `err`)
    ///   -2  the response was text (HTML/plain) — nothing written; caller should
    ///       render it as a page instead (outContentType tells you which).
    /// `sdPath` is the destination file (created/overwritten only for real files).
    long download(const std::string& url, const std::string& sdPath,
                  std::string& err, std::string& finalUrl, std::string& outContentType,
                  int timeoutMs = 180000, int maxRedirects = 5,
                  const std::string& userAgent =
                      "Lilka/1.0 (ESP32-S3; browser; +https://github.com/lilka-dev/lilka)");
};

/// Progress/stage reporting: called with "dns", "tcp", "tls", "send",
/// "body 12KB", "inflate", "done". Keep the callback light; it runs inline
/// in the network path (UI may redraw+flush there).
using StageFn = void (*)(const char*);
void setStageFn(StageFn fn);

/// Accept-Language header value (user's language preference; default "en").
void setAcceptLanguage(const std::string& v);

/// Read Keira-stored Wi-Fi credentials (NVS "network", same as lilka-boombox).
bool wifiCreds(std::string& ssid, std::string& pass);

/// Connect STA mode; true on success.
bool connect(const std::string& ssid, const std::string& pass, int timeoutMs);

/// Ensure the radio is connected (no-op if already up). Worker-safe: the image
/// pipeline runs off the UI flow, so it must establish Wi-Fi itself before doing
/// any netconn work — otherwise a cold-boot first DNS/connect from a background
/// task hits an uninitialized lwIP core mutex and crashes.
bool ensureWifi(int timeoutMs = 15000);

}  // namespace net
