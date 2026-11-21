#include "net.h"
#include "miniz.h"
#include <Preferences.h>
#include <WiFi.h>
#include <cstring>

namespace net {

std::string Url::toString() const {
    std::string s = scheme + "://";
    s += host;
    if (port && ((scheme == "https" && port != 443) || (scheme == "http" && port != 80)))
        s += ":" + std::to_string(port);
    s += path;
    return s;
}

bool parseUrl(const std::string& in, Url& out) {
    std::string s = in;
    size_t p = s.find("://");
    if (p == std::string::npos) {
        out.scheme = "https";
        s = "https://" + s;
        p = 5;
    } else {
        out.scheme = s.substr(0, p);
        if (out.scheme != "http" && out.scheme != "https") return false;
    }
    size_t rest = p + 3;
    size_t end = s.find('/', rest);
    std::string hostport = (end == std::string::npos) ? s.substr(rest) : s.substr(rest, end - rest);
    if (hostport.empty()) return false;
    size_t at = hostport.find('@');
    if (at != std::string::npos) hostport = hostport.substr(at + 1);
    size_t cp = hostport.find(':');
    if (cp != std::string::npos) {
        out.host = hostport.substr(0, cp);
        out.port = (int)strtol(hostport.c_str() + cp + 1, nullptr, 10);
    } else {
        out.host = hostport;
    }
    out.path = (end == std::string::npos) ? "/" : s.substr(end);
    size_t hash = out.path.find('#');
    if (hash != std::string::npos) out.path = out.path.substr(0, hash);
    return !out.host.empty();
}

std::string resolve(const Url& base, const std::string& ref) {
    if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0) return ref;
    if (ref.empty() || ref[0] == '#') return base.toString();
    if (ref[0] == '?') return base.scheme + "://" + base.host + base.path + ref;
    if (ref[0] == '/' && ref[1] == '/') {
        std::string r = ref.substr(2);
        if (r.rfind("http", 0) == 0) return r;  // full absolute in Location
        return base.scheme + "://" + r;
    }
    if (ref[0] == '/') return base.scheme + "://" + base.host + ref;
    size_t slash = base.path.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? "/" : base.path.substr(0, slash + 1);
    std::string out = dir + ref;
    while (true) {
        size_t d = out.find("/./");
        if (d == std::string::npos) break;
        out = out.substr(0, d) + out.substr(d + 2);
    }
    return out;
}

std::string rewriteUrl(const std::string& in) {
    Url u;
    if (!parseUrl(in, u)) return in;

    std::string h = u.host;
    for (auto& c : h) c = (char)tolower((unsigned char)c);

    // Google Search: append &gbv=1 for clean no-JS HTML results
    bool isGoogle = (h == "google.com" || h == "www.google.com" ||
                     h.find(".google.") != std::string::npos ||
                     h.rfind("google.", 0) == 0);
    if (isGoogle) {
        if (u.path == "/search" || u.path.rfind("/search?", 0) == 0 || u.path.rfind("/search/", 0) == 0) {
            if (u.path.find("gbv=") == std::string::npos) {
                u.path += (u.path.find('?') == std::string::npos ? "?gbv=1" : "&gbv=1");
                return u.toString();
            }
        }
    }
    return in;
}

// ---------------------------------------------------------------- cookies

namespace {

struct Cookie {
    std::string domain;
    std::string name;
    std::string value;
};

std::vector<Cookie> g_cookies;
constexpr size_t kMaxCookies = 30;

bool domainMatches(const std::string& requestHost, const std::string& cookieDomain) {
    if (requestHost == cookieDomain) return true;
    if (requestHost.size() > cookieDomain.size()) {
        size_t off = requestHost.size() - cookieDomain.size();
        if (requestHost[off - 1] == '.' &&
            requestHost.compare(off, cookieDomain.size(), cookieDomain) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

void setCookie(const std::string& domain, const std::string& name, const std::string& value) {
    if (name.empty()) return;
    std::string d = domain;
    for (auto& c : d) c = (char)tolower((unsigned char)c);
    if (!d.empty() && d[0] == '.') d.erase(0, 1);

    for (auto it = g_cookies.begin(); it != g_cookies.end(); ++it) {
        if (it->domain == d && it->name == name) {
            if (value.empty()) {
                g_cookies.erase(it);
                return;
            }
            it->value = value;
            return;
        }
    }
    if (value.empty()) return;
    if (g_cookies.size() >= kMaxCookies) {
        g_cookies.erase(g_cookies.begin());
    }
    g_cookies.push_back({d, name, value});
    Serial.printf("[cookie] set %s on %s (total %u)\n", name.c_str(), d.c_str(), (unsigned)g_cookies.size());
}

std::string getCookieHeader(const std::string& host) {
    std::string h = host;
    for (auto& c : h) c = (char)tolower((unsigned char)c);
    std::string out;
    for (const auto& ck : g_cookies) {
        if (domainMatches(h, ck.domain)) {
            if (!out.empty()) out += "; ";
            out += ck.name + "=" + ck.value;
        }
    }
    return out;
}

void clearCookies() {
    g_cookies.clear();
    Serial.println("[cookie] cleared");
}

size_t cookieCount() {
    return g_cookies.size();
}

std::string dumpCookies() {
    std::string out;
    for (const auto& ck : g_cookies) {
        out += ck.domain + "\t" + ck.name + "=" + ck.value + "\n";
    }
    return out;
}

void parseSetCookie(const std::string& host, const std::string& v) {
    if (v.empty()) return;
    size_t semi = v.find(';');
    std::string nv = (semi == std::string::npos) ? v : v.substr(0, semi);
    size_t eq = nv.find('=');
    if (eq == std::string::npos) return;
    std::string name = nv.substr(0, eq);
    std::string val = nv.substr(eq + 1);

    size_t nb = name.find_first_not_of(" \t");
    size_t ne = name.find_last_not_of(" \t");
    if (nb == std::string::npos) return;
    name = name.substr(nb, ne - nb + 1);

    size_t vb = val.find_first_not_of(" \t");
    size_t ve = val.find_last_not_of(" \t");
    val = (vb == std::string::npos) ? "" : val.substr(vb, ve - vb + 1);

    std::string domain;
    if (semi != std::string::npos) {
        std::string attrs = v.substr(semi + 1);
        size_t p = 0;
        while (p < attrs.size()) {
            size_t nextSemi = attrs.find(';', p);
            std::string attr = (nextSemi == std::string::npos) ? attrs.substr(p) : attrs.substr(p, nextSemi - p);
            size_t aeq = attr.find('=');
            if (aeq != std::string::npos) {
                std::string ak = attr.substr(0, aeq);
                std::string av = attr.substr(aeq + 1);
                size_t akb = ak.find_first_not_of(" \t");
                size_t ake = ak.find_last_not_of(" \t");
                if (akb != std::string::npos) ak = ak.substr(akb, ake - akb + 1);
                for (auto& c : ak) c = (char)tolower((unsigned char)c);
                if (ak == "domain") {
                    size_t avb = av.find_first_not_of(" \t");
                    size_t ave = av.find_last_not_of(" \t");
                    if (avb != std::string::npos) domain = av.substr(avb, ave - avb + 1);
                } else if (ak == "max-age") {
                    long ma = strtol(av.c_str(), nullptr, 10);
                    if (ma <= 0) val.clear();
                }
            }
            if (nextSemi == std::string::npos) break;
            p = nextSemi + 1;
        }
    }
    if (domain.empty()) domain = host;
    setCookie(domain, name, val);
}

// ---------------------------------------------------------------- wifi

namespace {
std::string keiraSsidHash(const std::string& s) {
    uint64_t h = 0;
    for (unsigned i = 0; i < s.size(); i++)
        h = (h << 5) - h + (unsigned char)s[i];
    char buf[9];
    snprintf(buf, sizeof(buf), "%08x", (unsigned int)h);
    return buf;
}

// Skip the gzip header (magic + FLG-dependent fields). Returns offset of the
// deflate stream, or 0 on garbage.
size_t gzipPayloadOffset(const std::string& raw) {
    if (raw.size() < 10 || (unsigned char)raw[0] != 0x1f || (unsigned char)raw[1] != 0x8b)
        return 0;
    uint8_t flg = (uint8_t)raw[3];
    size_t p = 10;
    if (flg & 4) {  // FEXTRA
        if (p + 2 > raw.size()) return 0;
        uint16_t xlen = (uint16_t)((unsigned)raw[p] | ((unsigned)raw[p + 1] << 8));
        p += 2 + xlen;
    }
    if (flg & 8) { while (p < raw.size() && (unsigned char)raw[p]) p++; p++; }       // FNAME
    if (flg & 16) { while (p < raw.size() && (unsigned char)raw[p]) p++; p++; }     // FCOMMENT
    if (flg & 2) p += 2;                                                            // FHCRC
    return (p < raw.size()) ? p : 0;
}

bool readLine(WiFiClient& c, std::string& line) {
    line.clear();
    uint32_t t0 = millis();
    while (millis() - t0 < 10000) {
        if (!c.available()) {
            if (c.connected()) { delay(5); continue; }
            return false;
        }
        int ch = c.read();
        if (ch < 0) continue;  // read() returns -1 on timeout — do NOT append 0xFF
        if (ch == '\n') return true;
        if (ch != '\r') line += (char)ch;
    }
    return false;
}

}  // namespace

bool wifiCreds(std::string& ssid, std::string& pass) {
    // Keira OS stores: namespace "network", key "last_ssid",
    // and "<hash(ssid)>_pw" for the password of that network.
    Preferences prefs;
    prefs.begin("network", true);
    String ss = prefs.getString("last_ssid", "");
    std::string s = ss.c_str();
    String ps = s.empty() ? String("") : prefs.getString((keiraSsidHash(s) + "_pw").c_str(), "");
    std::string p = ps.c_str();
    prefs.end();
    ssid = s;
    pass = p;
    return !s.empty() && !p.empty();
}

bool connect(const std::string& ssid, const std::string& pass, int timeoutMs) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < (uint32_t)timeoutMs)
        delay(100);
    return WiFi.status() == WL_CONNECTED;
}

// ---------------------------------------------------------------- client

StageFn g_stageFn = nullptr;
void setStageFn(StageFn fn) { g_stageFn = fn; }

std::string g_acceptLang = "en";  // explicit user preference, not an assumption
void setAcceptLanguage(const std::string& v) { g_acceptLang = v.empty() ? "en" : v; }
static void stage(const char* s) { if (g_stageFn) g_stageFn(s); }

bool Client::open(const Url& u, int timeoutMs, WiFiClientSecure& client, std::string& err) {
    client.setInsecure();  // no CA store on-device; MITM risk accepted for v1
    client.setTimeout(timeoutMs);
    uint16_t port = (uint16_t)(u.port ? u.port : (u.scheme == "https" ? 443 : 80));
    // Split DNS and TCP so the error message says which one failed.
    for (int attempt = 1; attempt <= 2; attempt++) {
        stage("dns");
        IPAddress ip;
        if (!WiFi.hostByName(u.host.c_str(), ip)) {
            err = "dns " + u.host + " failed";
            delay(300);
            continue;
        }
        stage("tcp");
        Serial.printf("[net] %s -> %s:%u\n", u.host.c_str(), ip.toString().c_str(), (unsigned)port);
        if (client.connect(ip, port, 15000)) {
            stage("tls");
            return true;
        }
        err = "connect " + u.host + " failed";
        client.stop();
        delay(300);
    }
    return false;
}

namespace {
std::string g_lastRaw;
}

const std::string& lastRawBody() { return g_lastRaw; }

bool inflateBody(const std::string& raw, const std::string& encoding, std::string& out, std::string& err) {
    bool wantGzip = encoding.find("gzip") != std::string::npos;
    bool isGzip = raw.size() >= 2 && (unsigned char)raw[0] == 0x1f && (unsigned char)raw[1] == 0x8b;
    bool isZlib = raw.size() >= 2 && (unsigned char)raw[0] == 0x78;
    if (!((wantGzip && (isGzip || isZlib)) || isGzip)) {
        out = raw;
        return true;
    }
    size_t off = isGzip ? gzipPayloadOffset(raw) : 0;
    if (isGzip && off == 0) { err = "bad gzip header"; return false; }
    out.clear();
    out.reserve(raw.size() * 2 + 1024);
    mz_stream st;
    std::memset(&st, 0, sizeof(st));
    st.next_in = (uint8_t*)raw.data() + off;
    st.avail_in = (uInt)(raw.size() - off);
    uint8_t obuf[4096];
    int windowBits = isZlib ? 15 : -15;  // 15 = zlib header, -15 = raw deflate (post-gzip-header)
    int initRet = mz_inflateInit2(&st, windowBits);
    int lastRet = initRet;
    if (initRet == MZ_OK) {
        for (;;) {
            st.next_out = obuf;
            st.avail_out = sizeof(obuf);
            int ret = mz_inflate(&st, MZ_NO_FLUSH);  // MZ_FINISH assumes out fits everything
            out.append((char*)obuf, sizeof(obuf) - st.avail_out);
            lastRet = ret;
            if (ret == MZ_STREAM_END) break;
            if (ret == MZ_BUF_ERROR) {  // no more input: done or truncated
                if (st.avail_in == 0) break;
                continue;
            }
            if (ret != MZ_OK) break;
        }
        int trailerLeft = (int)st.avail_in;
        mz_inflateEnd(&st);
        if (isGzip && trailerLeft >= 7 && raw.size() >= 8) {
            const unsigned char* tr2 = (const unsigned char*)raw.data() + raw.size() - 8;
            // raw still contains the full trailer (tinfl may or may not have
            // consumed 1 byte of it — irrelevant, we read from `raw`).
            uint32_t fileCrc = (uint32_t)tr2[0] | ((uint32_t)tr2[1] << 8) | ((uint32_t)tr2[2] << 16) | ((uint32_t)tr2[3] << 24);
            uint32_t calc = mz_crc32(0, Z_NULL, 0);
            calc = mz_crc32(calc, (const unsigned char*)out.data(), (uInt)out.size());
            Serial.printf("[net] gzip crc file=%08x calc=%08x %s\n", fileCrc, calc, fileCrc == calc ? "OK" : "MISMATCH");
        }
    }
    if (lastRet != MZ_STREAM_END || out.size() < raw.size() / 2) {
        err = "gzip decode failed (ret=" + std::to_string(lastRet) + ")";
        return false;
    }
    return true;
}

bool Client::request(const Url& u, std::string& body, std::string& location,
                     std::string& err, int timeoutMs, const std::string& userAgent,
                     int& status, const std::string& method, const std::string& postBody) {
    WiFiClientSecure client;
    if (!open(u, timeoutMs, client, err)) return false;

    std::string cookieHdr = getCookieHeader(u.host);
    std::string req = method + " " + u.path + " HTTP/1.1\r\n"
                      "Host: " + u.host + "\r\n"
                      "User-Agent: " + userAgent + "\r\n"
                      "Accept: text/html,application/xhtml+xml;q=0.9,*/*;q=0.8\r\n"
                      "Accept-Encoding: gzip\r\n"
                      "Accept-Language: " + g_acceptLang + "\r\n" +
                      (cookieHdr.empty() ? "" : ("Cookie: " + cookieHdr + "\r\n"));
    if (method == "POST" && !postBody.empty()) {
        req += "Content-Type: application/x-www-form-urlencoded\r\n";
        req += "Content-Length: " + std::to_string(postBody.size()) + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    if (!cookieHdr.empty()) {
        Serial.printf("[net] send cookie (%u chars)\n", (unsigned)cookieHdr.size());
    }
    stage("send");
    client.print(req.c_str());
    if (method == "POST" && !postBody.empty()) client.print(postBody.c_str());

    std::string line;
    if (!readLine(client, line) || line.empty()) { err = "no response"; return false; }
    // line = "HTTP/1.1 200 OK" — status is the second token
    size_t sp = line.find(' ');
    status = (sp == std::string::npos) ? 0 : (int)strtol(line.c_str() + sp + 1, nullptr, 10);
    Serial.printf("[net] status %d (%s)\n", status, line.c_str());
    if (status < 100 || status > 599) { err = "bad status"; return false; }

    bool chunked = false;
    long contentLength = -1;
    std::string encoding;
    for (;;) {
        if (!readLine(client, line)) break;
        if (line.empty()) break;
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c), v = line.substr(c + 1);
        for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
        size_t vs = v.find_first_not_of(" \t");
        if (vs != std::string::npos) v = v.substr(vs);
        if (k == "set-cookie") {
            parseSetCookie(u.host, v);
        }
        else if (k == "location") {
            // Some servers (yes, Google) emit invalid chars in Location;
            // strip anything that can't be in a URL.
            std::string clean;
            for (char ch : v)
                if (ch != '<' && ch != '>' && ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') clean += ch;
            location = clean;
        }
        else if (k == "content-length") contentLength = atol(v.c_str());
        else if (k == "transfer-encoding") { if (v.find("chunked") != std::string::npos) chunked = true; }
        else if (k == "content-encoding") encoding = v;
    }

    std::string raw;
    size_t lastReported = 0;
    if (chunked) {
        std::string sizeLine;
        int nChunks = 0;
        long lastSz = -1;
        long sumSizes = 0;
        for (;;) {
            if (!readLine(client, sizeLine)) { Serial.printf("[net] chunk: size line read FAIL\n"); break; }
            long sz = strtol(sizeLine.c_str(), nullptr, 16);
            lastSz = sz;
            if (sz <= 0) break;
            sumSizes += sz;
            nChunks++;
            // NOTE: read into a temp buffer and append only on success.
            // Pre-resizing `raw` and then failing to read leaves null padding
            // in the middle of the body (this corrupted the gzip stream).
            uint8_t rbuf[4096];
            while (sz > 0 && (client.available() || client.connected())) {
                int want = (int)(sz < 4096 ? sz : 4096);
                int n = client.read(rbuf, (size_t)want);
                if (n <= 0) { delay(2); continue; }
                raw.append((const char*)rbuf, (size_t)n);
                sz -= n;
                if (raw.size() - lastReported >= 8192) {
                    lastReported = raw.size();
                    stage(("body " + std::to_string(raw.size() / 1024) + "KB").c_str());
                }
            }
            if (sz > 0) { Serial.printf("[net] chunk: left %ld (gave up)\n", sz); break; }
            if (!readLine(client, sizeLine)) { Serial.printf("[net] chunk %d: CRLF read FAIL\n", nChunks); break; }
        }
        Serial.printf("[net] chunked: %d chunks, raw=%u %s\n", nChunks, (unsigned)raw.size(), (long)raw.size() == sumSizes ? "FRAMING-OK" : "FRAMING-MISMATCH");
    } else {
        if (contentLength > 0) raw.reserve((size_t)contentLength);
        uint32_t t0 = millis();
        uint8_t buf[2048];
        for (;;) {
            if (contentLength > 0 && (long)raw.size() >= contentLength) break;
            if (millis() - t0 > (uint32_t)timeoutMs) break;
            if (client.available()) {
                int n = client.read(buf, sizeof(buf));
                if (n > 0) {
                    raw.append((char*)buf, n);
                    if (raw.size() - lastReported >= 8192) {
                        lastReported = raw.size();
                        stage(("body " + std::to_string(raw.size() / 1024) + "KB").c_str());
                    }
                    continue;
                }
            }
            if (!client.connected()) break;
            delay(2);
        }
    }
    client.stop();
    Serial.printf("[net] body raw=%u enc=%s\n", (unsigned)raw.size(), encoding.c_str());

    g_lastRaw = raw;  // keep for debug commands (saveraw/reinflate)
    stage("inflate");
    if (!inflateBody(raw, encoding, body, err)) {
        body = body.empty() ? raw : body;
        return false;
    }
    Serial.printf("[net] body final=%u\n", (unsigned)body.size());
    return true;
}

bool Client::get(const std::string& url, std::string& outBody, std::string& outFinalUrl,
                 std::string& err, int timeoutMs, int maxRedirects, const std::string& userAgent) {
    std::string cur = rewriteUrl(url);
    for (int i = 0; i <= maxRedirects; i++) {
        Url u;
        if (!parseUrl(cur, u)) { err = "bad url: " + url; return false; }
        std::string body, location;
        int status = 0;
        if (!request(u, body, location, err, timeoutMs, userAgent, status)) return false;
        if (status == 404) {
            // Some servers (Wikipedia) 404 on case-variant titles that their own
            // redirects produce (e.g. /wiki/esp32 -> /wiki/Esp32 -> 404, while
            // /wiki/ESP32 exists). Retry once with the first path segment
            // uppercased.
            size_t q = u.path.find('?');
            size_t end = (q == std::string::npos) ? u.path.size() : q;
            size_t s = u.path.find('/', 1);
            if (s != std::string::npos && s < end) {
                size_t e = u.path.find('/', s + 1);
                if (e == std::string::npos || e > end) e = end;
                std::string seg = u.path.substr(s + 1, e - s - 1);
                bool anyLower = false;
                for (char& ch : seg) { if (ch >= 'a' && ch <= 'z') { ch -= 32; anyLower = true; } }
                if (anyLower) {
                    std::string alt = u.scheme + "://" + u.host +
                        (u.port ? ":" + std::to_string(u.port) : "") +
                        u.path.substr(0, s + 1) + seg + u.path.substr(e);
                    Url u2;
                    if (parseUrl(alt, u2)) {
                        std::string b2, loc2;
                        int st2 = 0;
                        if (request(u2, b2, loc2, err, timeoutMs, userAgent, st2) && st2 == 200) {
                            Serial.printf("[net] 404 retry ok: %s\n", alt.c_str());
                            stage("done");
                            outBody = std::move(b2);
                            outFinalUrl = u2.toString();
                            return true;
                        }
                    }
                }
            }
        }
        if (status >= 300 && status < 400 && !location.empty()) {
            cur = resolve(u, location);
            // Defensive: some Google endpoints emit mangled Location values that
            // concatenate an absolute URL into a relative path
            // ("/https://www.google.com/..." or "/mail/https://accounts...").
            // A literal "http" inside the path (real targets percent-encode it)
            // is a strong signal of that bug — jump to the embedded URL.
            Url probe;
            if (parseUrl(cur, probe)) {
                // only inspect the path (query params may legitimately carry URLs)
                size_t q = probe.path.find('?');
                std::string pathOnly = (q == std::string::npos) ? probe.path : probe.path.substr(0, q);
                size_t h = pathOnly.find("http");
                if (h != std::string::npos && h > 0) {
                    size_t at = cur.find(pathOnly.substr(h));
                    Url probe2;
                    if (at != std::string::npos && parseUrl(cur.substr(at), probe2))
                        cur = cur.substr(at);
                }
            }
            cur = rewriteUrl(cur);
            Serial.printf("[net] redirect -> %s\n", cur.c_str());
            if (!parseUrl(cur, probe)) { err = "bad redirect: " + cur; return false; }
            continue;
        }
        if (status != 200) { err = "http " + std::to_string(status); return false; }
        stage("done");
        outBody = std::move(body);
        outFinalUrl = u.toString();
        return true;
    }
    err = "too many redirects";
    return false;
}

bool Client::post(const std::string& url, const std::string& form,
                  std::string& outBody, std::string& outFinalUrl, std::string& err,
                  int timeoutMs, int maxRedirects, const std::string& userAgent) {
    std::string cur = rewriteUrl(url);
    Url u;
    if (!parseUrl(cur, u)) { err = "bad url: " + url; return false; }
    std::string body, location;
    int status = 0;
    if (!request(u, body, location, err, timeoutMs, userAgent, status, "POST", form)) return false;
    if (status >= 300 && status < 400 && !location.empty()) {
        // browser behavior: follow form redirects with GET
        std::string follow = resolve(u, location);
        Url probe;
        if (!parseUrl(follow, probe)) { err = "bad redirect: " + follow; return false; }
        Serial.printf("[net] post redirect -> %s\n", follow.c_str());
        return get(follow, outBody, outFinalUrl, err, timeoutMs, maxRedirects, userAgent);
    }
    if (status != 200) { err = "http " + std::to_string(status); return false; }
    stage("done");
    outBody = std::move(body);
    outFinalUrl = u.toString();
    return true;
}

}  // namespace net
