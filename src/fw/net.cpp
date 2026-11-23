#include "net.h"
#include "miniz.h"
#include "store.h"
#include <Preferences.h>
#include <WiFi.h>
#include <FreeRTOS.h>
#include <semphr.h>
#include <atomic>
#include <cstring>
#include <map>

namespace net {

namespace {
std::atomic<bool> g_cancel{false};
SemaphoreHandle_t g_netLock = nullptr;
void netLockInit() {
    if (!g_netLock) g_netLock = xSemaphoreCreateMutex();
}
}

NetGuard::NetGuard() {
    netLockInit();
    if (g_netLock) xSemaphoreTake(g_netLock, portMAX_DELAY);
    g_cancel = false;  // whoever holds the lock owns the request; clear any stale cancel
}
NetGuard::~NetGuard() {
    if (g_netLock) xSemaphoreGive(g_netLock);
}
void requestCancel() { g_cancel = true; }
bool cancelled() { return g_cancel.load(); }

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
    // host plus an explicit port when it's non-default (base.host alone would
    // drop the port and break relative links on non-default-port sites).
    std::string hp = base.host;
    if (base.port && ((base.scheme == "https" && base.port != 443) || (base.scheme == "http" && base.port != 80)))
        hp += ":" + std::to_string(base.port);
    if (ref[0] == '?') return base.scheme + "://" + hp + base.path + ref;
    if (ref[0] == '/' && ref[1] == '/') {
        std::string r = ref.substr(2);
        if (r.rfind("http", 0) == 0) return r;  // full absolute in Location
        return base.scheme + "://" + r;
    }
    if (ref[0] == '/') return base.scheme + "://" + hp + ref;
    size_t slash = base.path.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? "/" : base.path.substr(0, slash + 1);
    std::string out = dir + ref;
    while (true) {
        size_t d = out.find("/./");
        if (d == std::string::npos) break;
        out = out.substr(0, d) + out.substr(d + 2);
    }
    // Relative refs must keep the host — dropping it made the worker reject
    // root/site-relative <img src> (e.g. "./static/x.jpg") as "bad url".
    return base.scheme + "://" + hp + out;
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

struct Cookie {
    std::string domain;
    std::string name;
    std::string value;
};

std::vector<Cookie> g_cookies;
constexpr size_t kMaxCookies = 30;

void loadCookieJar() {
    std::vector<std::string> rows;
    if (!store::cookiesLoad(rows)) return;
    for (auto& row : rows) {
        size_t tab = row.find('\t');
        size_t eq = row.find('=');
        if (tab == std::string::npos || eq == std::string::npos || eq < tab) continue;
        std::string domain = row.substr(0, tab);
        std::string name = row.substr(tab + 1, eq - tab - 1);
        std::string value = row.substr(eq + 1);
        if (g_cookies.size() >= kMaxCookies) break;
        g_cookies.push_back({domain, name, value});
    }
    if (!g_cookies.empty())
        Serial.printf("[cookie] loaded %u from SD\n", (unsigned)g_cookies.size());
}

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

void saveCookieJar() {
    std::vector<std::string> rows;
    for (const auto& ck : g_cookies)
        rows.push_back(ck.domain + "\t" + ck.name + "=" + ck.value);
    store::cookiesSave(rows);
}

void setCookie(const std::string& domain, const std::string& name, const std::string& value) {
    if (name.empty()) return;
    std::string d = domain;
    for (auto& c : d) c = (char)tolower((unsigned char)c);
    if (!d.empty() && d[0] == '.') d.erase(0, 1);

    for (auto it = g_cookies.begin(); it != g_cookies.end(); ++it) {
        if (it->domain == d && it->name == name) {
            if (value.empty()) {
                g_cookies.erase(it);
            } else {
                it->value = value;
            }
            saveCookieJar();
            return;
        }
    }
    if (value.empty()) { saveCookieJar(); return; }
    if (g_cookies.size() >= kMaxCookies) {
        g_cookies.erase(g_cookies.begin());
    }
    g_cookies.push_back({d, name, value});
    saveCookieJar();
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
    store::cookiesClear();
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

bool ensureWifi(int timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    std::string ssid, pass;
    if (!wifiCreds(ssid, pass)) return false;
    return connect(ssid, pass, timeoutMs);
}

// ---------------------------------------------------------------- client

StageFn g_stageFn = nullptr;
void setStageFn(StageFn fn) { g_stageFn = fn; }

std::string g_acceptLang = "en";  // explicit user preference, not an assumption
void setAcceptLanguage(const std::string& v) { g_acceptLang = v.empty() ? "en" : v; }
static void stage(const char* s) { if (g_stageFn) g_stageFn(s); }

// Per-host IP cache. A netconn DNS lookup (WiFi.hostByName) from a background
// task can block for tens of seconds with no user-settable timeout — and the
// image worker re-resolves the same host for every <img>. Caching the resolved
// IP means we only do one DNS lookup per host, so a slow/hung resolution on one
// request doesn't keep happening on the next.
static std::map<std::string, std::pair<IPAddress, uint32_t>> g_ipCache;
static const uint32_t kIpTTL = 300000;  // 5 min — plenty for a browsing session
static bool resolveHost(const std::string& host, IPAddress& ip) {
    auto it = g_ipCache.find(host);
    if (it != g_ipCache.end() && (millis() - it->second.second < kIpTTL)) {
        ip = it->second.first;
        return true;
    }
    if (WiFi.hostByName(host.c_str(), ip)) { g_ipCache[host] = {ip, millis()}; return true; }
    return false;
}

bool Client::open(const Url& u, int timeoutMs, WiFiClientSecure& client, std::string& err) {
    client.setInsecure();  // no CA store on-device; MITM risk accepted for v1
    client.setTimeout(timeoutMs);
    uint16_t port = (uint16_t)(u.port ? u.port : (u.scheme == "https" ? 443 : 80));
    // Split DNS and TCP so the error message says which one failed.
    for (int attempt = 1; attempt <= 2; attempt++) {
        stage("dns");
        IPAddress ip;
        if (!resolveHost(u.host, ip)) {
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
std::string g_charset;   // declared charset from the last response (may be empty)
std::string g_contentType;  // main type of the last response (e.g. "application/pdf")
std::string g_etag;
std::string g_lastModified;
bool g_notModified = false;
}

const std::string& lastRawBody() { return g_lastRaw; }

const std::string& lastCharset() { return g_charset; }

const std::string& lastContentType() { return g_contentType; }

const std::string& lastEtag() { return g_etag; }

const std::string& lastModified() { return g_lastModified; }

bool lastNotModified() { return g_notModified; }

bool inflateBody(const std::string& raw, const std::string& encoding, std::string& out, std::string& err) {
    bool wantGzip = encoding.find("gzip") != std::string::npos;
    bool isGzip = raw.size() >= 2 && (unsigned char)raw[0] == 0x1f && (unsigned char)raw[1] == 0x8b;
    bool isZlib = raw.size() >= 2 && (unsigned char)raw[0] == 0x78;
    if (!((wantGzip && (isGzip || isZlib)) || isGzip)) {
        out = std::move(raw);
        return true;
    }
    size_t off = isGzip ? gzipPayloadOffset(raw) : 0;
    if (isGzip && off == 0) { err = "bad gzip header"; return false; }
    // decompress into a local string; raw is needed for the gzip trailer
    // check afterwards, so it must outlive the loop (no move before then).
    std::string inflated;
    inflated.reserve(raw.size() * 2 + 1024);
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
            inflated.append((char*)obuf, sizeof(obuf) - st.avail_out);
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
            calc = mz_crc32(calc, (const unsigned char*)inflated.data(), (uInt)inflated.size());
            Serial.printf("[net] gzip crc file=%08x calc=%08x %s\n", fileCrc, calc, fileCrc == calc ? "OK" : "MISMATCH");
        }
    }
    if (lastRet != MZ_STREAM_END || inflated.size() < raw.size() / 2) {
        err = "gzip decode failed (ret=" + std::to_string(lastRet) + ")";
        return false;
    }
    out = std::move(inflated);
    return true;
}

bool Client::sendRequest(const Url& u, WiFiClientSecure& client, Headers& h, std::string& err,
                         int timeoutMs, const std::string& userAgent, const std::string& method,
                         const std::string& postBody, const std::string& ifNoneMatch,
                         const std::string& ifModifiedSince) {
    std::string cookieHdr = getCookieHeader(u.host);
    std::string req = method + " " + u.path + " HTTP/1.1\r\n"
                      "Host: " + u.host + "\r\n"
                      "User-Agent: " + userAgent + "\r\n"
                      "Accept: text/html,application/xhtml+xml;q=0.9,*/*;q=0.8\r\n"
                      "Accept-Encoding: gzip\r\n"
                      "Accept-Language: " + g_acceptLang + "\r\n" +
                      "Sec-CH-UA-Mobile: ?1\r\n" +
                      (cookieHdr.empty() ? "" : ("Cookie: " + cookieHdr + "\r\n"));
    if (!ifNoneMatch.empty())     req += "If-None-Match: " + ifNoneMatch + "\r\n";
    if (!ifModifiedSince.empty()) req += "If-Modified-Since: " + ifModifiedSince + "\r\n";
    if (method == "POST" && !postBody.empty()) {
        req += "Content-Type: application/x-www-form-urlencoded\r\n";
        req += "Content-Length: " + std::to_string(postBody.size()) + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    if (!cookieHdr.empty()) Serial.printf("[net] send cookie (%u chars)\n", (unsigned)cookieHdr.size());
    stage("send");
    client.print(req.c_str());
    if (method == "POST" && !postBody.empty()) client.print(postBody.c_str());

    std::string line;
    if (!readLine(client, line) || line.empty()) { err = "no response"; return false; }
    size_t sp = line.find(' ');
    h.status = (sp == std::string::npos) ? 0 : (int)strtol(line.c_str() + sp + 1, nullptr, 10);
    Serial.printf("[net] status %d (%s)\n", h.status, line.c_str());
    if (h.status < 100 || h.status > 599) { err = "bad status"; return false; }
    g_notModified = (h.status == 304);

    for (;;) {
        if (!readLine(client, line)) break;
        if (line.empty()) break;
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c), v = line.substr(c + 1);
        for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
        size_t vs = v.find_first_not_of(" \t");
        if (vs != std::string::npos) v = v.substr(vs);
        if (k == "set-cookie") parseSetCookie(u.host, v);
        else if (k == "location") {
            std::string clean;
            for (char ch : v)
                if (ch != '<' && ch != '>' && ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') clean += ch;
            h.location = clean;
        }
        else if (k == "content-length") h.contentLength = atol(v.c_str());
        else if (k == "transfer-encoding") { if (v.find("chunked") != std::string::npos) h.chunked = true; }
        else if (k == "content-encoding") h.encoding = v;
        else if (k == "content-type") h.contentType = v;
        else if (k == "etag") g_etag = v;
        else if (k == "last-modified") g_lastModified = v;
    }
    return true;
}

bool Client::readBody(WiFiClientSecure& client, const Headers& h, std::string& out, std::string& err, int timeoutMs) {
    size_t lastReported = 0;
    uint32_t bodyT0 = millis();
    uint32_t lastData = bodyT0;
    if (h.chunked) {
        std::string sizeLine;
        int nChunks = 0;
        long sumSizes = 0;
        for (;;) {
            if (millis() - bodyT0 > (uint32_t)timeoutMs) { err = "chunked timeout"; break; }
            if (millis() - lastData > 10000) { err = "chunked stall"; break; }
            if (!readLine(client, sizeLine)) { err = "chunk size read fail"; break; }
            long sz = strtol(sizeLine.c_str(), nullptr, 16);
            if (sz <= 0) break;
            sumSizes += sz;
            nChunks++;
            uint8_t rbuf[4096];
            while (sz > 0 && (client.available() || client.connected())) {
                if (millis() - lastData > 10000) { err = "chunk data stall"; break; }
                int want = (int)(sz < 4096 ? sz : 4096);
                int n = client.read(rbuf, (size_t)want);
                if (n <= 0) { delay(2); continue; }
                out.append((const char*)rbuf, (size_t)n);
                lastData = millis();
                sz -= n;
                if (out.size() - lastReported >= 8192) {
                    lastReported = out.size();
                    stage(("body " + std::to_string(out.size() / 1024) + "KB").c_str());
                }
            }
            if (!err.empty() || sz > 0) { if (sz > 0) err = err.empty() ? "chunk short" : err; break; }
            if (!readLine(client, sizeLine)) { err = "chunk CRLF fail"; break; }
        }
        Serial.printf("[net] chunked: %d chunks, raw=%u %s\n", nChunks, (unsigned)out.size(), (long)out.size() == sumSizes ? "FRAMING-OK" : "FRAMING-MISMATCH");
    } else {
        if (h.contentLength > 0) out.reserve((size_t)h.contentLength);
        uint8_t buf[2048];
        for (;;) {
            if (h.contentLength > 0 && (long)out.size() >= h.contentLength) break;
            if (millis() - bodyT0 > (uint32_t)timeoutMs) { err = "body timeout"; break; }
            if (millis() - lastData > 10000) { err = "body stall"; break; }
            if (client.available()) {
                int n = client.read(buf, sizeof(buf));
                if (n > 0) {
                    out.append((char*)buf, n);
                    lastData = millis();
                    if (out.size() - lastReported >= 8192) {
                        lastReported = out.size();
                        stage(("body " + std::to_string(out.size() / 1024) + "KB").c_str());
                    }
                    continue;
                }
            }
            if (!client.connected()) break;
            delay(2);
        }
    }
    return err.empty();
}

bool Client::request(const Url& u, std::string& body, std::string& location,
                     std::string& err, int timeoutMs, const std::string& userAgent,
                     int& status, const std::string& method, const std::string& postBody,
                     const std::string& ifNoneMatch, const std::string& ifModifiedSince) {
    NetGuard guard;
    WiFiClientSecure client;
    if (!open(u, timeoutMs, client, err)) return false;

    Headers h;
    if (!sendRequest(u, client, h, err, timeoutMs, userAgent, method, postBody, ifNoneMatch, ifModifiedSince)) {
        client.stop();
        return false;
    }
    status = h.status;
    location = h.location;

    if (h.status == 304) {
        g_notModified = true;
        client.stop();
        body.clear();
        Serial.printf("[net] 304 Not Modified (reuse cache)\n");
        return true;
    }

    std::string raw;
    if (!readBody(client, h, raw, err, timeoutMs)) { client.stop(); return false; }
    client.stop();
    Serial.printf("[net] body raw=%u enc=%s ctype=%s\n", (unsigned)raw.size(), h.encoding.c_str(), h.contentType.c_str());

    // remember the declared charset so the caller can transcode legacy encodings
    g_charset.clear();
    {
        std::string ct = h.contentType;
        for (auto& ch : ct) ch = (char)tolower((unsigned char)ch);
        size_t semi = ct.find(';');
        if (semi != std::string::npos) ct = ct.substr(0, semi);
        size_t ws = ct.find_last_not_of(" \t");
        if (ws != std::string::npos) ct = ct.substr(0, ws + 1);
        g_contentType = ct;

        size_t c = ct.find("charset=");
        if (c != std::string::npos) {
            size_t i = c + 8;
            if (i < ct.size() && (ct[i] == '"' || ct[i] == '\'')) i++;
            size_t s = i;
            while (i < ct.size() && ct[i] != '"' && ct[i] != '\''
                   && ct[i] != ';' && ct[i] != ' ') i++;
            g_charset = ct.substr(s, i - s);
        }
    }

    stage("inflate");
    bool ok = inflateBody(raw, h.encoding, body, err);
    g_lastRaw = std::move(raw);  // keep raw for debug commands (saveraw/reinflate)
    if (!ok) {
        body = body.empty() ? std::move(g_lastRaw) : body;
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

long Client::download(const std::string& url, const std::string& sdPath,
                      std::string& err, std::string& finalUrl, std::string& outContentType,
                      int timeoutMs, int maxRedirects, const std::string& userAgent) {
    NetGuard guard;
    std::string cur = rewriteUrl(url);
    for (int i = 0; i <= maxRedirects; i++) {
        Url u;
        if (!parseUrl(cur, u)) { err = "bad url: " + url; return -1; }

        WiFiClientSecure client;
        if (!open(u, timeoutMs, client, err)) return -1;

        // Downloads want the exact bytes: request identity encoding so a binary
        // isn't gzipped on us (we stream to SD, no room to inflate a big file).
        Headers h;
        {
            std::string cookieHdr = getCookieHeader(u.host);
            std::string req = "GET " + u.path + " HTTP/1.1\r\n"
                              "Host: " + u.host + "\r\n"
                              "User-Agent: " + userAgent + "\r\n"
                              "Accept: */*\r\n"
                              "Accept-Encoding: identity\r\n"
                              "Accept-Language: " + g_acceptLang + "\r\n" +
                              (cookieHdr.empty() ? "" : ("Cookie: " + cookieHdr + "\r\n")) +
                              "Connection: close\r\n\r\n";
            stage("send");
            client.print(req.c_str());

            std::string line;
            if (!readLine(client, line) || line.empty()) { err = "no response"; client.stop(); return -1; }
            size_t sp = line.find(' ');
            h.status = (sp == std::string::npos) ? 0 : (int)strtol(line.c_str() + sp + 1, nullptr, 10);
            Serial.printf("[net] dl status %d (%s)\n", h.status, line.c_str());
            if (h.status < 100 || h.status > 599) { err = "bad status"; client.stop(); return -1; }
            for (;;) {
                if (!readLine(client, line)) break;
                if (line.empty()) break;
                size_t c = line.find(':');
                if (c == std::string::npos) continue;
                std::string k = line.substr(0, c), v = line.substr(c + 1);
                for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
                size_t vs = v.find_first_not_of(" \t");
                if (vs != std::string::npos) v = v.substr(vs);
                if (k == "set-cookie") parseSetCookie(u.host, v);
                else if (k == "location") {
                    std::string clean;
                    for (char ch : v)
                        if (ch != '<' && ch != '>' && ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') clean += ch;
                    h.location = clean;
                }
                else if (k == "content-length") h.contentLength = atol(v.c_str());
                else if (k == "transfer-encoding") { if (v.find("chunked") != std::string::npos) h.chunked = true; }
                else if (k == "content-encoding") h.encoding = v;
                else if (k == "content-type") h.contentType = v;
            }
        }

        if (h.status >= 300 && h.status < 400 && !h.location.empty()) {
            cur = resolve(u, h.location);
            client.stop();
            Serial.printf("[net] dl redirect -> %s\n", cur.c_str());
            continue;
        }
        if (h.status != 200) { err = "http " + std::to_string(h.status); client.stop(); return -1; }

        // If it's actually text (HTML/plain), don't stream to SD — let the caller
        // render it as a page. (GitHub /blob/ links return text/html; /raw/ returns octet-stream.)
        {
            std::string ct = h.contentType;
            for (auto& ch : ct) ch = (char)tolower((unsigned char)ch);
            outContentType = ct;
            if (ct.rfind("text/", 0) == 0 || ct.find("application/json") != std::string::npos
                || ct.find("xml") != std::string::npos || ct.find("svg") != std::string::npos) {
                client.stop();
                Serial.printf("[net] dl: response is %s — not saving as file\n", ct.c_str());
                return -2;
            }
        }

        // Stream the body straight to SD — never buffer the whole file in RAM.
        fs::File f = SD.open(sdPath.c_str(), FILE_WRITE);
        if (!f) { err = "open SD fail: " + sdPath; client.stop(); return -1; }

        long total = 0;
        long lastReported = 0;
        uint32_t bodyT0 = millis();
        uint32_t lastData = bodyT0;
        bool fail = false;

        uint8_t buf[4096];
        auto writeChunk = [&](const uint8_t* p, int n) -> bool {
            int off = 0;
            while (off < n) {
                int w = f.write(p + off, (size_t)(n - off));
                if (w <= 0) return false;
                off += w;
            }
            total += n;
            lastData = millis();
            if (total - lastReported >= 16384) {
                lastReported = total;
                std::string st = "dl " + std::to_string(total / 1024) + "KB";
                if (h.contentLength > 0) st += "/" + std::to_string(h.contentLength / 1024) + "KB";
                stage(st.c_str());
            }
            return true;
        };

        if (h.chunked) {
            std::string sizeLine;
            for (;;) {
                if (millis() - bodyT0 > (uint32_t)timeoutMs) { fail = true; err = "dl timeout"; break; }
                if (millis() - lastData > 10000) { fail = true; err = "dl stall"; break; }
                if (!readLine(client, sizeLine)) { fail = true; err = "chunk read fail"; break; }
                long sz = strtol(sizeLine.c_str(), nullptr, 16);
                if (sz <= 0) break;
                while (sz > 0) {
                    if (millis() - lastData > 10000) { fail = true; err = "chunk stall"; break; }
                    if (!client.available()) { delay(2); continue; }
                    int want = (int)(sz < (long)sizeof(buf) ? sz : (long)sizeof(buf));
                    int n = client.read(buf, (size_t)want);
                    if (n <= 0) continue;
                    if (!writeChunk(buf, n)) { fail = true; err = "SD write fail"; break; }
                    sz -= n;
                }
                if (fail) break;
                if (!readLine(client, sizeLine)) { fail = true; err = "chunk CRLF fail"; break; }
            }
        } else {
            long left = h.contentLength;
            for (;;) {
                if (left > 0 && total >= left) break;
                if (millis() - bodyT0 > (uint32_t)timeoutMs) { fail = true; err = "dl timeout"; break; }
                if (millis() - lastData > 10000) { fail = true; err = "dl stall"; break; }
                if (!client.available()) { if (!client.connected()) break; delay(2); continue; }
                int n = client.read(buf, sizeof(buf));
                if (n > 0) {
                    if (left > 0 && (long)n > left - total) n = (int)(left - total);
                    if (!writeChunk(buf, n)) { fail = true; err = "SD write fail"; break; }
                }
            }
        }

        f.close();
        client.stop();
        if (fail) { SD.remove(sdPath.c_str()); Serial.printf("[net] dl FAILED %s (%ld bytes)\n", err.c_str(), total); return -1; }
        Serial.printf("[net] dl OK %s (%ld bytes)\n", sdPath.c_str(), total);
        finalUrl = u.toString();
        return total;
    }
    err = "too many redirects";
    return -1;
}

bool Client::fetchBinary(const std::string& url, std::string& out, std::string& outContentType,
                         long maxSize, std::string& outFinalUrl, std::string& err,
                         int timeoutMs, int maxRedirects) {
    NetGuard guard;
    std::string cur = rewriteUrl(url);
    for (int i = 0; i <= maxRedirects; i++) {
        Url u;
        if (!parseUrl(cur, u)) { err = "bad url"; return false; }

        WiFiClientSecure client;
        if (!open(u, timeoutMs, client, err)) return false;
        client.setNoDelay(true);  // non-blocking: read() must not wedge the worker

        std::string cookieHdr = getCookieHeader(u.host);
        std::string req = "GET " + u.path + " HTTP/1.1\r\n"
                          "Host: " + u.host + "\r\n"
                          "User-Agent: Mozilla/5.0 (FreeRTOS; ESP32; Mobile) Mobile Safari/537.36\r\n"
                          "Accept: image/jpeg,image/png,*/*;q=0.2\r\n"
                          "Accept-Encoding: identity\r\n" +
                          (cookieHdr.empty() ? "" : ("Cookie: " + cookieHdr + "\r\n")) +
                          "Connection: close\r\n\r\n";
        client.print(req.c_str());

        std::string line;
        if (!readLine(client, line) || line.empty()) { err = "no response"; client.stop(); return false; }
        size_t sp = line.find(' ');
        int status = (sp == std::string::npos) ? 0 : (int)strtol(line.c_str() + sp + 1, nullptr, 10);
        bool chunked = false; long contentLength = -1; std::string contentType, location;
        for (;;) {
            if (!readLine(client, line)) break;
            if (line.empty()) break;
            size_t c = line.find(':');
            if (c == std::string::npos) continue;
            std::string k = line.substr(0, c), v = line.substr(c + 1);
            for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
            size_t vs = v.find_first_not_of(" \t");
            if (vs != std::string::npos) v = v.substr(vs);
            if (k == "set-cookie") parseSetCookie(u.host, v);
            else if (k == "location") {
                std::string clean;
                for (char ch : v) if (ch != '<' && ch != '>' && ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') clean += ch;
                location = clean;
            }
            else if (k == "content-length") contentLength = atol(v.c_str());
            else if (k == "transfer-encoding") { if (v.find("chunked") != std::string::npos) chunked = true; }
            else if (k == "content-type") contentType = v;
        }
        if (status >= 300 && status < 400 && !location.empty()) {
            cur = resolve(u, location); client.stop(); continue;
        }
        if (status != 200) { err = "http " + std::to_string(status); client.stop(); return false; }

        // type check: only jpeg/png
        std::string ct = contentType;
        for (auto& ch : ct) ch = (char)tolower((unsigned char)ch);
        outContentType = ct;
        bool okType = ct.rfind("image/jpeg", 0) == 0 || ct.rfind("image/png", 0) == 0;
        if (!okType) { client.stop(); err = "not an image (" + ct + ")"; return false; }

        // stream into `out` with a hard size cap + cancel check
        if (contentLength > maxSize) { client.stop(); err = "too large (" + std::to_string(contentLength) + ")"; return false; }
        out.clear();
        uint32_t t0 = millis(); uint32_t lastData = t0;
        uint8_t buf[4096];
        if (chunked) {
            std::string sizeLine;
            for (;;) {
                if (cancelled()) { err = "cancelled"; break; }
                if (millis() - t0 > (uint32_t)timeoutMs) { err = "timeout"; break; }
                if (millis() - lastData > 10000) { err = "stall"; break; }
                if (!readLine(client, sizeLine)) { err = "chunk read fail"; break; }
                long sz = strtol(sizeLine.c_str(), nullptr, 16);
                if (sz <= 0) break;
                while (sz > 0) {
                    if (cancelled()) { err = "cancelled"; break; }
                    if (millis() - lastData > 10000) { err = "stall"; break; }
                    if (!client.available()) { delay(2); continue; }
                    int want = (int)(sz < (long)sizeof(buf) ? sz : (long)sizeof(buf));
                    int n = client.read(buf, (size_t)want);
                    if (n <= 0) continue;
                    out.append((char*)buf, n); lastData = millis(); sz -= n;
                    if ((long)out.size() > maxSize) { err = "too large"; break; }
                }
                if (!err.empty()) break;
                if (!readLine(client, sizeLine)) { err = "chunk CRLF fail"; break; }
            }
        } else {
            for (;;) {
                if (contentLength > 0 && (long)out.size() >= contentLength) break;
                if (cancelled()) { err = "cancelled"; break; }
                if (millis() - t0 > (uint32_t)timeoutMs) { err = "timeout"; break; }
                if (millis() - lastData > 10000) { err = "stall"; break; }
                if (client.available()) {
                    int n = client.read(buf, sizeof(buf));
                    if (n > 0) {
                        out.append((char*)buf, n); lastData = millis();
                        if ((long)out.size() > maxSize) { err = "too large"; break; }
                        continue;
                    }
                }
                if (!client.connected()) break;
                delay(2);
            }
        }
        client.stop();
        if (!err.empty()) return false;
        Serial.printf("[img] fetched %s (%u bytes) %s\n", u.host.c_str(), (unsigned)out.size(), ct.c_str());
        outFinalUrl = u.toString();
        return true;
    }
    err = "too many redirects";
    return false;
}

bool Client::getCond(const std::string& url, std::string& outBody, std::string& outFinalUrl,
                     std::string& err, const std::string& ifNoneMatch, const std::string& ifModifiedSince,
                     int timeoutMs, int maxRedirects, const std::string& userAgent) {
    std::string cur = rewriteUrl(url);
    for (int i = 0; i <= maxRedirects; i++) {
        Url u;
        if (!parseUrl(cur, u)) { err = "bad url: " + url; return false; }
        std::string body, location;
        int status = 0;
        if (!request(u, body, location, err, timeoutMs, userAgent, status, "GET", "",
                     ifNoneMatch, ifModifiedSince)) return false;
        if (status == 304) {
            g_notModified = true;
            stage("done");
            outFinalUrl = u.toString();
            return true;  // caller reuses its cache
        }
        if (status >= 300 && status < 400 && !location.empty()) {
            // redirected: follow as a plain GET (revalidation already consumed)
            cur = resolve(u, location);
            cur = rewriteUrl(cur);
            if (!parseUrl(cur, u)) { err = "bad redirect: " + cur; return false; }
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
