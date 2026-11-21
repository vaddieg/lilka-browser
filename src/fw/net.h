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

    /// POST `form` (application/x-www-form-urlencoded) to `url`.
    /// Redirects are followed with GET (browser behavior for 301/302/303).
    bool post(const std::string& url, const std::string& form,
              std::string& outBody, std::string& outFinalUrl, std::string& err,
              int timeoutMs = 15000, int maxRedirects = 5,
              const std::string& userAgent =
                  "Lilka/1.0 (ESP32-S3; browser; +https://github.com/lilka-dev/lilka)");

private:
    bool open(const Url& u, int timeoutMs, WiFiClientSecure& client, std::string& err);
    /// One request: fills body (decoded), location header, status code; handles gzip.
    /// `method` is "GET" or "POST"; `postBody` sent as-is for POST.
    bool request(const Url& u, std::string& body, std::string& location,
                 std::string& err, int timeoutMs, const std::string& userAgent, int& status,
                 const std::string& method = "GET", const std::string& postBody = "");
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

}  // namespace net
