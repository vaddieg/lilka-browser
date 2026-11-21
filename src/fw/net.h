// HTTP/HTTPS client for the Lilka browser (ESP32-S3).
// - GET with redirects (301/302/303/307/308), relative Location resolution
// - gzip/deflate response decoding (miniz)
// - body buffered in PSRAM (std::string, allocator override)
#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <memory>
#include <string>

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

/// Decode a gzip/zlib body (miniz). Pass-through if not compressed.
/// `encoding` is the Content-Encoding header value (may be empty).
bool inflateBody(const std::string& raw, const std::string& encoding, std::string& out, std::string& err);

/// Last raw (pre-inflate) response body — kept for debug commands.
const std::string& lastRawBody();

/// Resolve `ref` against `base` (RFC 3986-ish, no fragments).
std::string resolve(const Url& base, const std::string& ref);

class Client {
public:
    /// GET `url`. Follows up to `maxRedirects` redirects.
    /// On success: outBody holds decoded HTML (PSRAM), outFinalUrl the last URL.
    /// Returns false with a short human reason in `err`.
    bool get(const std::string& url, std::string& outBody, std::string& outFinalUrl,
             std::string& err, int timeoutMs = 15000, int maxRedirects = 5,
             const std::string& userAgent =
                 "Lilka/1.0 (ESP32-S3; browser; +https://github.com/lilka-dev/lilka)");

private:
    bool open(const Url& u, int timeoutMs, WiFiClientSecure& client, std::string& err);
    /// One request: fills body (decoded), location header, status code; handles gzip.
    bool request(const Url& u, std::string& body, std::string& location,
                 std::string& err, int timeoutMs, const std::string& userAgent, int& status);
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
