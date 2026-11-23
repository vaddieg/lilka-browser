#include "ui.h"
#include "font.h"
#include "net.h"
#include "store.h"
#include "img.h"
#include "html_parser.h"
#include "charset.h"
#include <WiFi.h>
#include <algorithm>
#include <cctype>

namespace ui { class Browser; }
extern ui::Browser g_browser;  // defined in main.cpp (stage callback trampoline)

namespace ui {

using BlockKind = lilka::BlockKind;

namespace {
constexpr int kHeaderH = 18;
constexpr int kFooterH = 14;
// The panel has rounded corners, so keep the top/bottom bars off the extreme
// left/right edges (inset from both sides).
constexpr int kBarInset = 25;
const uint16_t C_ACCENT = 0x07FF;    // cyan (R0 G31 B31) — same convention as lilka-boombox
const uint16_t C_LINK = 0x07FF;      // cyan for links
const uint16_t C_DIM = 0x841C;

// Top bar background: full width (looks better edge-to-edge; the panel's rounded
// corners clip it naturally). The title text and footer hint are still inset by
// kBarInset so they stay out of the rounded corners.
void topBar(Arduino_GFX* g, uint16_t color) {
    g->fillRect(0, 0, g->width(), kHeaderH, color);
}
void headerTitle(Arduino_GFX* g, const char* text, uint16_t color) {
    int w = g->width() - 2 * kBarInset;
    font::text(g, kBarInset, 3, font::fit(text, w, 1), color, 1);
}
void footer(Arduino_GFX* g, const char* text, uint16_t color) {
    int w = g->width() - 2 * kBarInset;
    font::text(g, kBarInset, g->height() - kFooterH + 1, font::fit(text, w, 1), color, 1);
}


void wrapWords(const std::string& text, int16_t maxW, std::vector<std::string>& out, uint8_t scale = 1) {
    std::string cur;
    auto push = [&]() { if (!cur.empty()) out.push_back(cur); cur.clear(); };
    size_t i = 0;
    while (i < text.size()) {
        size_t sp = text.find(' ', i);
        std::string word = (sp == std::string::npos) ? text.substr(i) : text.substr(i, sp - i);
        if (word.empty()) { i = (sp == std::string::npos) ? text.size() : sp + 1; continue; }
        if (!cur.empty()) {
            std::string probe = cur + " " + word;
            if (font::width(probe.c_str(), scale) <= maxW) { cur = probe; }
            else { push(); cur = word; }
        } else {
            if (font::width(word.c_str(), scale) > maxW) {
                // hard-break long word
                cur = font::fit(word.c_str(), maxW, scale);
                push();
                // drop the fitted remainder (rare on this screen)
            } else {
                cur = word;
            }
        }
        i = (sp == std::string::npos) ? text.size() : sp + 1;
    }
    push();
}
}  // namespace

void Browser::setButtons(Buttons* b) { btns_ = b; }

int Browser::lineH() const {
    return fontScale_ >= 3 ? 42 : (fontScale_ >= 2 ? 22 : 15);
}

uint8_t Browser::lineScale(int lineIdx) const {
    if (fontScale_ >= 3) return 3;
    if (fontScale_ >= 2) return 2;
    if (lineIdx >= 0 && lineIdx < (int)lines_.size()) {
        int bi = lines_[lineIdx].block;
        if (bi >= 0 && bi < (int)page_.blocks.size()) {
            if (page_.blocks[bi].kind == BlockKind::Heading) return 2;
        }
    }
    return 1;
}

int Browser::lineH(int lineIdx) const {
    if (lineIdx >= 0 && lineIdx < (int)lines_.size() && lines_[lineIdx].img)
        return lines_[lineIdx].phh > 0 ? lines_[lineIdx].phh : 90;
    uint8_t s = lineScale(lineIdx);
    return s >= 3 ? 42 : (s >= 2 ? 22 : 15);
}

// Recent sites: saved history; if empty, a small built-in starter list.
std::vector<std::string> Browser::recentList() const {
    auto urls = store::urls(6);
    if (!urls.empty()) return urls;
    return {
        "https://www.google.com",
        "https://duckduckgo.com",
        "https://www.bbc.com/news",
        "https://habr.com/ru/articles/",
        "https://ru.wikipedia.org",
    };
}

// Quick Start list: favorites first (marked '*'), then recent history.
constexpr const char* kDownloadsEntry = "Downloads";

std::vector<std::string> Browser::quickList() const {
    std::vector<std::string> out;
    out.reserve(10);
    out.push_back(kDownloadsEntry);   // first entry: saved files
    for (auto& f : store::favorites(6)) {
        out.push_back(f);
        if (out.size() >= 9) break;
    }
    for (auto& u : recentList()) {
        if (out.size() >= 9) break;
        bool dup = false;
        for (auto& e : out) if (e == u) dup = true;
        if (!dup) out.push_back(u);
    }
    // "Enter URL..." is always last (index == size()-1)
    out.push_back("");
    return out;
}

bool Browser::begin(Arduino_GFX* gfx) {
    gfx_ = gfx;
    font::begin(gfx);
    if (store::begin()) {
        int fs = atoi(store::get("font", "1").c_str());
        if (fs >= 1 && fs <= 3) fontScale_ = fs;
        compact_ = (store::get("compact", "0") == "1");
        net::loadCookieJar();
        qsCache_ = quickList();
        img::begin();   // background image worker (core 1) + /browser/img dir
    }
    history_.push_back("");  // home slot
    currentUrl_ = "";
    return true;
}

bool Browser::clickable(int lineIdx) const {
    if (lineIdx < 0 || lineIdx >= (int)lines_.size()) return false;
    const Line& l = lines_[lineIdx];
    if (!l.href.empty()) return true;   // table row with a link
    if (l.block < 0 || l.block >= (int)page_.blocks.size()) return false;
    auto k = page_.blocks[l.block].kind;
    return k == BlockKind::Link || k == BlockKind::Button ||
           k == BlockKind::Input || k == BlockKind::Select ||
           k == BlockKind::Checkbox;
}

// Boilerplate: short, generic nav/footer strings that waste screen space.
// Matched on short blocks only (<= 24 chars) so real content is never dropped.
namespace {
bool isBoilerplateText(const std::string& raw) {
    std::string s = raw;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n')) s.pop_back();
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    if (s.size() > 24) return false;
    static const char* k[] = {
        "skip to content", "jump to content", "main menu", "move to sidebar",
        "hide", "show", "show more", "show less", "read more", "continue",
        "sign in", "sign up", "log in", "login", "log out", "register",
        "account", "my account", "profile", "preferences",
        "privacy", "privacy policy", "privacy notice", "cookies", "cookie policy",
        "terms", "terms of use", "terms of service", "legal", "disclaimer",
        "feedback", "contact", "help", "support",
        "home", "about", "about us", "main", "top", "back to top", "navigation",
        "search", "random article", "donate", "give", "subscribe", "follow us",
        "tools", "more", "less", "expand", "collapse", "other projects",
        "in other languages", "appearance", "mobile view", "desktop view",
        "enable js", "javascript is disabled", "settings",
    };
    for (auto* e : k) if (s == e) return true;
    return false;
}
}  // namespace

bool Browser::isBoilerplate(const lilka::Block& b) const {
    if (!compact_) return false;
    // only non-interactive, short text-ish blocks are candidates
    if (b.kind != lilka::BlockKind::Text && b.kind != lilka::BlockKind::ListItem &&
        b.kind != lilka::BlockKind::Link) return false;
    return isBoilerplateText(b.text);
}

void Browser::runFind(const std::string& term) {
    findMatches_.clear();
    if (term.empty()) { findActive_ = false; return; }
    // case-insensitive substring match across the rendered lines
    auto fold = [](std::string s) {
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    std::string needle = fold(term);
    for (int i = 0; i < (int)lines_.size(); i++) {
        if (fold(lines_[i].text).find(needle) != std::string::npos) findMatches_.push_back(i);
    }
    findTerm_ = term;
    if (!findMatches_.empty()) {
        findPos_ = 0;
        lineCursor_ = findMatches_[0];
        findActive_ = true;
    } else {
        findActive_ = false;
    }
}

void Browser::findNext(bool forward) {
    if (!findActive_ || findMatches_.empty()) return;
    findPos_ = (findPos_ + (forward ? 1 : (int)findMatches_.size() - 1)) % (int)findMatches_.size();
    lineCursor_ = findMatches_[findPos_];
}

// Download queue derived from the parsed <img> blocks (in document order), so the
// on-page placeholder positions match what actually gets fetched.
std::vector<std::string> Browser::imageUrlsFromBlocks(const std::string& baseUrl) {
    std::vector<std::string> out;
    auto extOk = [](const std::string& u) {
        std::string low; for (char ch : u) low += (char)tolower((unsigned char)ch);
        size_t q = low.find_first_of("?#");
        std::string p = (q == std::string::npos) ? low : low.substr(0, q);
        return (p.size() > 4 && p.compare(p.size() - 4, 4, ".jpg") == 0)
            || (p.size() > 5 && p.compare(p.size() - 5, 5, ".jpeg") == 0)
            || (p.size() > 4 && p.compare(p.size() - 4, 4, ".png") == 0);
    };
    for (auto& b : page_.blocks) {
        if (b.kind != BlockKind::Image) continue;
        std::string low = b.href;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (low.rfind("data:", 0) == 0 || low.rfind("javascript:", 0) == 0) continue;
        if (!extOk(b.href)) continue;
        std::string resolved = b.href;
        { net::Url base; if (net::parseUrl(baseUrl, base)) resolved = net::resolve(base, b.href); }
        bool dup = false; for (auto& s : out) if (s == resolved) { dup = true; break; }
        if (!dup) out.push_back(resolved);
    }
    return out;
}

void Browser::renderImageLine(int i, int& y) {
    const Line& l = lines_[i];
    int W = gfx_->width();
    if (img::ready(l.href)) {
        std::vector<uint16_t> px; int iw = 0, ih = 0;
        if (img::loadBmp(l.href, px, iw, ih) && iw > 0 && ih > 0) {
            gfx_->draw16bitRGBBitmap((W - iw) / 2, y, px.data(), iw, ih);
            y += ih + 4;
            return;
        }
    }
    // placeholder: reserve the declared (scaled) height so the layout is stable
    int ph = l.phh > 0 ? l.phh : 90;
    bool queued = std::find(pageImages_.begin(), pageImages_.end(), l.href) != pageImages_.end();
    bool isFailed = img::failed(l.href);
    uint16_t border = isFailed ? 0xF800 : C_DIM;   // red frame for a failed image
    gfx_->fillRect(4, y, W - 8, ph, 0x1010);
    gfx_->drawRect(4, y, W - 8, ph, border);
    std::string cap = l.text.empty() ? "image" : l.text;
    font::text(gfx_, 10, y + 6, font::fit(cap.c_str(), W - 16, 1), C_DIM, 1);
    const char* st;
    if (isFailed)       st = "failed to load";
    else if (!queued)   st = "not supported";
    else if (img::busy()) st = "downloading...";
    else                 st = "queued...";
    // place the status line centered in the placeholder
    int sy = y + ph / 2 - 6;
    if (sy < y + 4) sy = y + 4;
    font::text(gfx_, 10, sy, st, border, 1);
    y += ph + 4;
}

void Browser::buildLines() {
    // any rebuild invalidates find match indices
    findActive_ = false; findMatches_.clear(); findTerm_.clear(); findPos_ = 0;
    lines_.clear();
    links_.clear();
    const int16_t maxW = gfx_ ? gfx_->width() - 10 : 230;
    for (int i = 0; i < (int)page_.blocks.size(); i++) {
        const auto& b = page_.blocks[i];
        if (isBoilerplate(b)) continue;
        char buf[64];
        switch (b.kind) {
            case BlockKind::Heading: {
                // at scale 3 the base font is already the big one — headings
                // just render in the base size (scale 2 would overflow)
                uint8_t hscale = (fontScale_ >= 3) ? 3 : ((fontScale_ >= 2 || b.level <= 4) ? 2 : 1);
                std::vector<std::string> ws;
                wrapWords(b.text, maxW, ws, hscale);
                for (auto& w : ws) lines_.push_back({i, w, false});
                break;
            }
            case BlockKind::Text:
            case BlockKind::ListItem: {
                // full text, no expand/collapse (A had no focusable target anyway)
                uint8_t tscale = fontScale_ >= 3 ? 3 : (fontScale_ >= 2 ? 2 : 1);
                std::string prefix = (b.kind == BlockKind::ListItem) ? std::string("- ") : "";
                std::vector<std::string> ws;
                wrapWords(prefix + b.text, maxW, ws, tscale);
                for (auto& w : ws) lines_.push_back({i, w, false});
                break;
            }
            case BlockKind::Link:
                lines_.push_back({i, b.text, false});
                links_.emplace_back(b.text, b.href);
                break;
            case BlockKind::Button: {
                Line l{i, b.text.empty() ? "OK" : b.text, false, false, true};
                lines_.push_back(std::move(l));
                break;
            }
            case BlockKind::Input: {
                // caption only; renderPage draws the field rectangle around it
                Line l{i, b.placeholder.empty() ? b.name : b.placeholder, false, true};
                lines_.push_back(std::move(l));
                break;
            }
            case BlockKind::Checkbox:
                lines_.push_back({i, b.text, false});  // glyph drawn in renderPage
                break;
            case BlockKind::Table:
                for (auto& r : b.rows) {
                    std::string t = r.k + (r.k.empty() ? "" : ": ") + r.v;
                    lines_.push_back({i, t, true, false, false, r.href});
                }
                break;
            case BlockKind::Select: {
                std::string v = b.options.empty() ? b.text :
                    b.options[b.selected < b.options.size() ? b.selected : 0];
                snprintf(buf, sizeof(buf), "[%s] (%d/%d)", v.c_str(),
                         (int)b.selected + 1, (int)b.options.size());
                lines_.push_back({i, buf, false});
                break;
            }
            case BlockKind::Image: {
                // inline, in document order; reserve height for the (<=240x180) image.
                // Resolve the href the SAME way as the download queue, so the cache
                // lookup (img::ready) matches the file that actually got saved.
                Line il;
                il.block = i; il.img = true;
                std::string resolved = b.href;
                { net::Url base; if (net::parseUrl(currentUrl_, base)) resolved = net::resolve(base, b.href); }
                il.href = resolved;
                il.text = b.text.empty() ? "image" : b.text;
                int phh = 90;  // default when the page doesn't declare a size
                if (b.width > 0 && b.height > 0) {
                    double sc = std::min(240.0 / b.width, 180.0 / b.height);
                    if (sc > 1.0) sc = 1.0;
                    int h = (int)(b.height * sc);
                    if (h < 24) h = 24;
                    phh = h;
                }
                il.phh = phh;
                lines_.push_back(std::move(il));
                break;
            }
        }
    }
    lineCursor_ = 0;
    sel_ = -1;
}



void Browser::frameClickables(std::vector<int>& out) const {
    out.clear();
    // same window math as renderPage so SELECT matches what is on screen
    int cap = (gfx_->height() - kHeaderH - kFooterH) / lineH();
    if (cap < 1) cap = 1;
    int top = lineCursor_;
    if (lineCursor_ >= cap) top = lineCursor_ - cap / 2;
    if (top + cap > (int)lines_.size()) top = (int)lines_.size() - cap;
    if (top < 0) top = 0;
    int y = kHeaderH + 4;
    for (int i = top; i < (int)lines_.size(); i++) {
        int lh = lineH(i);
        if (y + lh > gfx_->height() - kFooterH) break;
        if (clickable(i)) out.push_back(i);
        y += lh;
    }
}

namespace {
std::string urlenc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string formEncode(const std::vector<std::pair<std::string, std::string>>& fields,
                       const std::string& extraName, const std::string& extraValue) {
    std::string o;
    for (const auto& kv : fields) {
        if (!kv.first.empty()) {
            if (!o.empty()) o += '&';
            o += urlenc(kv.first) + "=" + urlenc(kv.second);
        }
    }
    if (!extraName.empty()) {
        if (!o.empty()) o += '&';
        o += urlenc(extraName) + "=" + urlenc(extraValue);
    }
    return o;
}
}  // namespace

// form fields + the form's checkboxes, only if they are currently checked
std::vector<std::pair<std::string, std::string>> Browser::withChecks(const lilka::Block& b) const {
    return withChecksFields(b.formFields, b.checkIds);
}

std::vector<std::pair<std::string, std::string>> Browser::withChecksFields(
        const std::vector<std::pair<std::string, std::string>>& base,
        const std::vector<size_t>& ids) const {
    std::vector<std::pair<std::string, std::string>> f = base;
    for (size_t id : ids) {
        if (id < page_.blocks.size() && page_.blocks[id].checked) {
            const auto& cb = page_.blocks[id];
            f.emplace_back(cb.name, cb.value);
        }
    }
    return f;
}

// Transcode a fetched body to UTF-8 in place if it came in a legacy
// single-byte encoding (cp1251/KOI8-R). No-op when already UTF-8.
// True when the last response is renderable text (page cache is for HTML only).
static bool isTextResponse() {
    std::string ct = net::lastContentType();
    for (auto& ch : ct) ch = (char)tolower((unsigned char)ch);
    return ct.empty() || ct.rfind("text/", 0) == 0 ||
           ct == "application/json" || ct == "image/svg+xml";
}

static void transcodeToUtf8(std::string& html) {
    std::string enc = lilka::charset::detect(html, net::lastCharset());
    if (enc != "utf8") {
        Serial.printf("[nav] transcode %s -> utf8\n", enc.c_str());
        std::string utf8;
        lilka::charset::toUtf8(html, enc.c_str(), utf8);
        html = std::move(utf8);
    }
}

void Browser::navigate(const std::string& urlIn, bool fromRedirect) {
    if (!fromRedirect) redirDepth_ = 0;  // user-initiated: reset hop budget
    std::string url = urlIn;
    while (!url.empty() && (unsigned char)url.back() < 32) url.pop_back();
    while (!url.empty() && (unsigned char)url.front() < 32) url.erase(url.begin());
    if (url.empty()) return;
    url = net::rewriteUrl(url);
    Serial.printf("[nav] navigate: %s\n", url.c_str());
    if (url.find("://") == std::string::npos && url.find('/') == std::string::npos &&
        url.find('.') == std::string::npos) {
        // bare word -> search. DuckDuckGo HTML endpoint: serves real results
        // to plain HTTP clients (Google's /search returns a JS/anti-bot page).
        url = "https://html.duckduckgo.com/html/?q=" + urlenc(url);
    } else if (url.find("://") == std::string::npos) {
        url = "https://" + url;
    }
    if (url == currentUrl_) { Serial.printf("[nav] same url, skip (cur=%s)\n", currentUrl_.c_str()); return; }

    net::setStageFn([](const char* s) { g_browser.onStage(s); });
    std::string lang = store::get("lang", "uk");
    net::setAcceptLanguage(lang == "uk" ? "uk,ru;q=0.8" : lang == "ru" ? "ru,en;q=0.8" : "en");
    img::clear();  // stop any in-flight background images from the previous page
    busyStage_ = "start";
    busyDownload_ = false;
    busyUrl_ = url;
    busyT0_ = millis();
    mode_ = BUSY;
    dirty_ = true;
    renderBusy();
    gfx_->flush();

    std::string err, html, finalUrl;
    std::string cachedHtml, cachedEtag, cachedLm;
    bool haveCache = false;
    if (store::get("cache", "1") == "1") {
        if (store::cacheGet(url, cachedHtml, &cachedEtag, &cachedLm)) haveCache = true;
    }
    bool canRevalidate = haveCache && (!cachedEtag.empty() || !cachedLm.empty());

    if (!canRevalidate && !haveCache) {
        // no cache at all: plain GET
        std::string ssid, pass;
        if (!net::wifiCreds(ssid, pass)) { err = "no wifi creds (set up in Keira)"; }
        else if (WiFi.status() != WL_CONNECTED && !net::connect(ssid, pass, 20000)) { err = "wifi connect failed"; }
        else {
            Serial.printf("[nav] GET %s ...\n", url.c_str());
            net::Client c;
            if (c.get(url, html, finalUrl, err)) {
                Serial.printf("[nav] got %u bytes\n", (unsigned)html.size());
                if (isTextResponse()) {
                    transcodeToUtf8(html);   // never touch binary bodies
                    store::cachePut(finalUrl, html, net::lastEtag(), net::lastModified());
                }
            }
        }
    } else if (canRevalidate) {
        // conditional GET: reuse cache on 304, refresh on 200
        std::string ssid, pass;
        if (!net::wifiCreds(ssid, pass)) { err = "no wifi creds (set up in Keira)"; }
        else if (WiFi.status() != WL_CONNECTED && !net::connect(ssid, pass, 20000)) { err = "wifi connect failed"; }
        else {
            Serial.printf("[nav] GET(cond) %s ...\n", url.c_str());
            net::Client c;
            if (c.getCond(url, html, finalUrl, err, cachedEtag, cachedLm)) {
                if (net::lastNotModified()) {
                    Serial.printf("[nav] 304 — reuse cache (%u bytes)\n", (unsigned)cachedHtml.size());
                    html = std::move(cachedHtml);
                } else {
                    Serial.printf("[nav] 200 refreshed %u bytes\n", (unsigned)html.size());
                    if (isTextResponse()) {
                        transcodeToUtf8(html);   // never touch binary bodies
                        store::cachePut(finalUrl, html, net::lastEtag(), net::lastModified());
                    }
                }
            } else if (haveCache) {
                // network/HTTP failure but we have a copy — serve it stale
                Serial.printf("[nav] revalidate failed (%s), serving stale cache\n", err.c_str());
                html = std::move(cachedHtml);
                err.clear();
            }
        }
    } else {
        // cache exists but has no validators: serve it directly
        html = std::move(cachedHtml);
    }
    if (!err.empty()) {
        errText_ = err;
        Serial.printf("[nav] ERROR: %s\n", err.c_str());
        mode_ = ERROR;
        dirty_ = true;
        return;
    }
    finishLoad(url, html, finalUrl);
}

// Find an auto-redirect target in the raw HTML (no-JS fallbacks we must
// honor ourselves):
//   1. <meta http-equiv="refresh" content="0; url=...">  (any delay)
//   2. JS stubs: location.replace("...") / location.href = '...'
// Returns "" when none found. Decodes &amp; etc. in the target.
static std::string findAutoRedirect(const std::string& html) {
    std::string h = html;
    for (auto& c : h) c = (char)tolower((unsigned char)c);

    auto extractQuoted = [&](size_t i, const std::string& s) {
        if (i >= s.size()) return std::string();
        if (s[i] == '"' || s[i] == '\'') i++;
        size_t e = s.find_first_of("\"'", i);
        std::string v = (e == std::string::npos) ? s.substr(i) : s.substr(i, e - i);
        // entity-decode &amp; (DDG hrefs carry &amp;rut=...)
        std::string out;
        for (size_t k = 0; k < v.size(); k++) {
            if (v.compare(k, 5, "&amp;") == 0) { out += '&'; k += 4; }
            else out += v[k];
        }
        return out;
    };

    // 1) meta refresh
    size_t m = h.find("http-equiv");
    while (m != std::string::npos) {
        size_t seg = h.find('>', m);
        size_t len = (seg == std::string::npos) ? 300 : std::min<size_t>(seg - m, 300);
        std::string tag = h.substr(m, len);
        if (tag.find("refresh") != std::string::npos) {
            size_t c = tag.find("content=");
            if (c != std::string::npos) {
                std::string content = extractQuoted(c + 8, tag);
                size_t u = content.find("url=");
                if (u != std::string::npos) {
                    std::string t = content.substr(u + 4);
                    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
                    if (t.rfind("http://", 0) == 0 || t.rfind("https://", 0) == 0) return t;
                }
            }
        }
        m = h.find("http-equiv", m + 1);
    }

    // 2) JS location stubs — only on tiny stub pages, so a large page with an
    //    incidental location.href in its scripts can't trigger a false redirect
    if (html.size() > 8192) return "";
    const char* pats[] = { "location.replace(", "location.href=", "location.href =" };
    for (auto* pat : pats) {
        size_t p = h.find(pat);
        while (p != std::string::npos) {
            size_t q = h.find_first_of("\"'", p + strlen(pat));
            if (q != std::string::npos && q - p < 60) {
                std::string t = extractQuoted(q, h);
                if (t.rfind("http://", 0) == 0 || t.rfind("https://", 0) == 0) return t;
            }
            p = h.find(pat, p + 1);
        }
    }
    return "";
}

void Browser::finishLoad(const std::string& url, std::string html, const std::string& finalUrl) {
    // server handed us a non-text payload (pdf/zip/image/…): save it, don't
    // try to parse binary as HTML
    {
        std::string ct = net::lastContentType();
        for (auto& ch : ct) ch = (char)tolower((unsigned char)ch);
        bool isText = ct.empty() || ct.rfind("text/", 0) == 0 ||
                      ct == "application/json" || ct == "image/svg+xml";
        if (!isText) {
            Serial.printf("[nav] non-text response (%s) -> save as download\n", ct.c_str());
            std::string name;
            {
                std::string u = finalUrl.empty() ? url : finalUrl;
                size_t q = u.find_first_of("?#");
                if (q != std::string::npos) u = u.substr(0, q);
                size_t slash = u.find_last_of('/');
                name = (slash == std::string::npos) ? u : u.substr(slash + 1);
            }
            if (name.empty() || name.find('.') == std::string::npos) name += ".bin";
            if (store::saveDownload(name, html)) {
                char sz[32];
                snprintf(sz, sizeof sz, html.size() >= 1048576 ? "%u.%uMB" : "%uKB",
                         (unsigned)(html.size() / (html.size() >= 1048576 ? 1048576 : 1024)),
                         html.size() >= 1048576 ? (unsigned)((html.size() / 102400) % 10) : 0u);
                doneText_ = std::string(name) + " (" + sz + ")\n" + "/browser/downloads/" + name;
                mode_ = DONE;
                dirty_ = true;
                return;
            }
            errText_ = "save failed (SD card?)";
            mode_ = ERROR;
            dirty_ = true;
            return;
        }
    }
    // a new page invalidates any in-page find
    findActive_ = false; findTerm_.clear(); findMatches_.clear();
    // no-JS redirect stubs (e.g. DuckDuckGo result links): follow the target
    {
        std::string redir = findAutoRedirect(html);
        if (!redir.empty()) {
            if (redirDepth_ < 5 && redir != url) {
                Serial.printf("[nav] auto-redirect hop %d -> %s\n", redirDepth_ + 1, redir.c_str());
                redirDepth_++;
                navigate(redir, true);   // keep the hop budget across stubs
                return;
            }
            Serial.printf("[nav] auto-redirect loop, stopping at %s\n", url.c_str());
        }
    }
    // parse
    Serial.printf("[nav] parsing...\n");
    lilka::html::Parser p;
    p.feed(html.data(), html.size());
    p.finish();
    page_ = std::move(p.page());
    // kick off background image downloads (core 1) before we build the lines so
    // the image placeholders are part of the page from the first paint.
    {
        std::string base = finalUrl.empty() ? url : finalUrl;
        pageImages_ = imageUrlsFromBlocks(base);  // inline <img> sources, in document order
        lastImgStatus_ = -1;
        img::queue(pageImages_);  // no-op if the list is empty
    }
    lastHtml_.swap(html);  // keep for the `dump` serial command
    Serial.printf("[nav] blocks=%u title=%s\n", (unsigned)page_.blocks.size(), page_.title.c_str());

    history_.push_back(url);
    store::addUrl(url);
    currentUrl_ = finalUrl.empty() ? url : finalUrl;
    mode_ = PAGE;
    buildLines();
    dirty_ = true;
}

void Browser::submitPost(const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& fields,
                         const std::string& extraName, const std::string& extraValue) {
    std::string form = formEncode(fields, extraName, extraValue);
    net::setStageFn([](const char* s) { g_browser.onStage(s); });
    busyStage_ = "start";
    busyUrl_ = url;
    busyT0_ = millis();
    mode_ = BUSY;
    dirty_ = true;
    renderBusy();
    gfx_->flush();

    std::string err, html, finalUrl;
    std::string ssid, pass;
    if (!net::wifiCreds(ssid, pass)) {
        err = "no wifi creds (set up in Keira)";
    } else if (WiFi.status() != WL_CONNECTED && !net::connect(ssid, pass, 20000)) {
        err = "wifi connect failed";
    } else {
        Serial.printf("[nav] POST %s (%u bytes)\n", url.c_str(), (unsigned)form.size());
        net::Client c;
        if (!c.post(url, form, html, finalUrl, err)) {
            Serial.printf("[nav] POST failed: %s\n", err.c_str());
        } else {
            Serial.printf("[nav] got %u bytes, final %s\n", (unsigned)html.size(), finalUrl.c_str());
            store::cachePut(finalUrl, html);
        }
    }
    if (!err.empty()) {
        errText_ = err;
        Serial.printf("[nav] ERROR: %s\n", err.c_str());
        mode_ = ERROR;
        dirty_ = true;
        return;
    }
    finishLoad(url, html, finalUrl);
}

void Browser::serialCmd(const std::string& in) {
    std::string line;
    size_t b = in.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return;
    size_t e = in.find_last_not_of(" \t\r\n");
    line = in.substr(b, e - b + 1);
    if (line.empty()) return;
    Serial.printf("[cmd] %s\n", line.c_str());
    if (line == "heap") {
        Serial.printf("[cmd] heap free=%u psram=%u\n", (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getFreePsram());
        return;
    }
    if (line == "imgls") {
        std::string ls; img::listDir(ls);
        Serial.printf("[cmd] /browser/img -> %s\n", ls.c_str());
        return;
    }
    if (line == "imgtest") {
        auto found = imageUrlsFromBlocks(currentUrl_.empty() ? "" : currentUrl_);
        Serial.printf("[cmd] imgtest: base=%s found=%d\n", currentUrl_.c_str(), (int)found.size());
        for (size_t i = 0; i < found.size() && i < 10; i++) Serial.printf("[cmd]   %s\n", found[i].c_str());
        return;
    }
    if (line == "img") {
        Serial.printf("[cmd] images: %d total, %d done, busy=%d (%s)\n",
                      img::total(), img::done(), (int)img::busy(), img::statusText());
        for (size_t i = 0; i < pageImages_.size(); i++) {
            Serial.printf("[cmd] img[%u]: %s ready=%d\n", (unsigned)i,
                          pageImages_[i].c_str(), (int)img::ready(pageImages_[i]));
        }
        return;
    }
    if (line.rfind("imgone", 0) == 0) {
        std::string u = line.substr(6);
        while (!u.empty() && u.front() == ' ') u.erase(u.begin());
        if (u.empty()) { Serial.printf("[cmd] usage: imgone <url>\n"); return; }
        img::clear();
        img::queue({u});
        Serial.printf("[cmd] imgone queued: %s\n", u.c_str());
        return;
    }
    if (line == "qlist") {
        auto l = quickList();
        Serial.printf("[cmd] sel=%d\n", linklistSel_);
        for (size_t i = 0; i < l.size(); i++)
            Serial.printf("[cmd] qs[%u]: %s\n", (unsigned)i, l[i].empty() ? "(Enter URL)" : l[i].c_str());
        return;
    }
    if (line == "dl") {
        auto l = store::downloads(20);
        Serial.printf("[cmd] downloads: %u\n", (unsigned)l.size());
        for (size_t i = 0; i < l.size(); i++)
            Serial.printf("[cmd] dl[%u]: %s %u bytes\n", (unsigned)i, l[i].name.c_str(), (unsigned)l[i].size);
        return;
    }
    if (line.rfind("dldel", 0) == 0) {
        std::string i = line.substr(5);
        while (!i.empty() && i.front() == ' ') i.erase(i.begin());
        int idx = atoi(i.c_str());
        auto l = store::downloads(20);
        if (idx >= 0 && idx < (int)l.size()) {
            bool ok = store::deleteDownload(l[idx].name);
            Serial.printf("[cmd] dldel %s %s\n", l[idx].name.c_str(), ok ? "ok" : "FAIL");
        } else Serial.printf("[cmd] dldel bad index\n");
        return;
    }
    if (line.rfind("dlfile", 0) == 0) {
        std::string url = line.substr(6);
        while (!url.empty() && url.front() == ' ') url.erase(url.begin());
        if (!url.empty()) downloadFile(url);
        return;
    }
    if (line.rfind("key", 0) == 0) {
        std::string k = line.substr(3);
        while (!k.empty() && k.front() == ' ') k.erase(k.begin());
        Buttons::Key key = Buttons::COUNT;
        if (k == "up") key = Buttons::UP;
        else if (k == "down") key = Buttons::DOWN;
        else if (k == "left") key = Buttons::LEFT;
        else if (k == "right") key = Buttons::RIGHT;
        else if (k == "a") key = Buttons::A;
        else if (k == "b") key = Buttons::B;
        else if (k == "c") key = Buttons::C;
        else if (k == "d") key = Buttons::D;
        else if (k == "select") key = Buttons::SELECT;
        else if (k == "start") key = Buttons::START;
        if (key < Buttons::COUNT) { btns_->inject(key); Serial.printf("[cmd] injected %s\n", k.c_str()); }
        return;
    }
    if (line.rfind("click", 0) == 0) {
        int n = atoi(line.c_str() + 6);
        if (n >= 0 && n < (int)lines_.size()) {
            lineCursor_ = n;
            activate(n);
        } else {
            Serial.printf("[cmd] bad line %d\n", n);
        }
        return;
    }
    if (line.rfind("cursor", 0) == 0) {
        lineCursor_ = atoi(line.c_str() + 7);
        dirty_ = true;
        Serial.printf("[cmd] cursor -> %d\n", lineCursor_);
        return;
    }
    if (line == "findnext") { findNext(true); Serial.printf("[cmd] findnext -> %d (pos %d/%d)\n", lineCursor_, findPos_ + 1, (int)findMatches_.size()); return; }
    if (line == "findclear") { findActive_ = false; findMatches_.clear(); findTerm_.clear(); dirty_ = true; Serial.printf("[cmd] find cleared\n"); return; }
    if (line.rfind("find", 0) == 0) {
        // find <text> -> run the in-page find (jumps to first match)
        std::string needle = line.substr(5);
        while (!needle.empty() && needle.front() == ' ') needle.erase(needle.begin());
        if (needle.empty()) { Serial.printf("[cmd] usage: find <text>\n"); return; }
        runFind(needle);
        mode_ = PAGE;
        dirty_ = true;
        if (findMatches_.empty()) {
            Serial.printf("[cmd] find: no matches\n");
            errText_ = "not found: " + needle;
            mode_ = ERROR;
        } else {
            Serial.printf("[cmd] find: %d match(es), cursor->%d\n",
                          (int)findMatches_.size(), lineCursor_);
        }
        return;
    }
    if (line == "compact" || line == "compact on" || line == "compact off") {
        bool on = (line == "compact") ? !compact_ : (line.find("on") != std::string::npos);
        if (on != compact_) { compact_ = on; store::set("compact", on ? "1" : "0"); buildLines(); }
        Serial.printf("[cmd] compact=%s lines=%d\n", compact_ ? "on" : "off", (int)lines_.size());
        return;
    }
    if (line.rfind("parse ", 0) == 0) {
        std::string s = line.substr(6);
        net::Url u;
        bool ok = net::parseUrl(s, u);
        Serial.printf("parse ok=%d scheme=%s host=%s port=%d path=%s\n",
                      ok, u.scheme.c_str(), u.host.c_str(), u.port, u.path.c_str());
        return;
    }
    if (line.rfind("hrefhex", 0) == 0) {
        int idx = atoi(line.c_str() + 7);
        if (idx >= 0 && idx < (int)lines_.size() && lines_[idx].block >= 0) {
            const auto& b = page_.blocks[lines_[idx].block];
            Serial.printf("len=%d ", (int)b.href.size());
            for (size_t i = 0; i < b.href.size(); i++) Serial.printf("%02x", (unsigned char)b.href[i]);
            Serial.println();
        }
        return;
    }
    if (line == "err") { Serial.printf("err: %s\n", errText_.c_str()); return; }
    if (line == "mkt") {
        Serial.printf("[sd] /browser exists=%d\n", (int)SD.exists("/browser"));
        bool m1 = SD.mkdir("/browser/cache");
        Serial.printf("[sd] mkdir /browser/cache -> %d, exists=%d\n", (int)m1, (int)SD.exists("/browser/cache"));
        File t = SD.open("/browser/cache/_t.txt", FILE_WRITE);
        Serial.printf("[sd] open write -> %d\n", (int)t);
        if (t) { t.print("hi"); t.close(); }
        Serial.printf("[sd] _t.txt exists=%d\n", (int)SD.exists("/browser/cache/_t.txt"));
        return;
    }
    if (line == "validators") {
        if (currentUrl_.empty()) { Serial.printf("[cmd] no current url\n"); return; }
        std::string h, e, lm;
        if (store::cacheGet(currentUrl_, h, &e, &lm))
            Serial.printf("[cmd] cache: body=%u etag='%s' lm='%s'\n", (unsigned)h.size(), e.c_str(), lm.c_str());
        else Serial.printf("[cmd] cache: miss for %s\n", currentUrl_.c_str());
        return;
    }
    if (line == "cookies") {
        Serial.printf("COOKIES (%u):\n%s", (unsigned)net::cookieCount(), net::dumpCookies().c_str());
        return;
    }
    if (line == "clearcookies") {
        net::clearCookies();
        return;
    }
    if (line == "lines") {
        for (int i = 0; i < (int)lines_.size(); i++) {
            const auto& L = lines_[i];
            if (L.block >= 0 && L.block < (int)page_.blocks.size()) {
                const auto& b = page_.blocks[L.block];
                Serial.printf("line %d: block=%d kind=%d sel=%d '%.*s' href='%s'\n", i, L.block, (int)b.kind, sel_, 30, L.text.c_str(), b.href.c_str());
            }
        }
        return;
    }
    if (line.rfind("goto", 0) == 0) {
        std::string url = line.substr(4);
        while (!url.empty() && url.front() == ' ') url.erase(url.begin());
        // A space means it's a search query — pass through so navigate()
        // routes it to DuckDuckGo. Otherwise treat as a bare host and add https.
        bool isSearch = url.find(' ') != std::string::npos && url.rfind("http", 0) != 0;
        if (!isSearch && url.rfind("http", 0) != 0 && url.size() > 4) url = "https://" + url;
        if (!url.empty()) navigate(url);
    } else if (line == "back") {
        goBack();
    } else if (line == "quickstart") {
        mode_ = QUICKSTART; linklistSel_ = 0; dirty_ = true;
    } else if (line == "dump") {
        Serial.printf("DUMPBEGIN %u\n", (unsigned)lastHtml_.size());
        Serial.write((const uint8_t*)lastHtml_.data(), lastHtml_.size());
        Serial.println("\nDUMPEND");
    } else if (line == "saveraw") {
        fs::File f = SD.open("/browser/raw.gz", FILE_WRITE);
        if (f) {
            f.write((const uint8_t*)net::lastRawBody().data(), net::lastRawBody().size());
            f.close();
            Serial.printf("saved %u bytes -> /browser/raw.gz\n", (unsigned)net::lastRawBody().size());
        } else {
            Serial.println("open failed");
        }
    } else if (line == "reinflate") {
        fs::File f = SD.open("/browser/raw.gz", FILE_READ);
        if (!f) { Serial.println("no /browser/raw.gz (saveraw first)"); return; }
        std::string raw;
        raw.reserve(f.size());
        uint8_t b[1024];
        while (f.available()) {
            int n = f.read(b, sizeof b);
            if (n <= 0) break;
            raw.append((char*)b, (size_t)n);
        }
        f.close();
        for (int run = 1; run <= 3; run++) {
            std::string out, err;
            bool ok = net::inflateBody(raw, "gzip", out, err);
            uint32_t crc = 2166136261u;
            for (unsigned char c : out) { crc ^= c; crc *= 16777619u; }
            Serial.printf("reinflate %d: ok=%d out=%u fnv=%08x %s\n", run, ok, (unsigned)out.size(), crc, err.c_str());
            if (run == 1 && ok) {
                fs::File of = SD.open("/browser/out.html", FILE_WRITE);
                if (of) { of.write((const uint8_t*)out.data(), out.size()); of.close(); }
            }
        }
    } else if (line.rfind("dumpfile", 0) == 0) {
        std::string path = line.substr(9);
        while (!path.empty() && path.front() == ' ') path.erase(0, 1);
        fs::File f = SD.open(path.c_str(), FILE_READ);
        if (!f) { Serial.println("open fail"); return; }
        Serial.printf("DUMPBEGIN %u\n", (unsigned)f.size());
        while (f.available()) {
            uint8_t b[512];
            int n = f.read(b, sizeof b);
            if (n <= 0) break;
            Serial.write(b, n);
        }
        f.close();
        Serial.println("\nDUMPEND");
    }
}

void Browser::goBack() {
    if (history_.size() <= 1) {
        Serial.printf("[nav] back: at home\n");
        mode_ = QUICKSTART; linklistSel_ = 0;
        dirty_ = true;
        return;
    }
    history_.pop_back();
    currentUrl_ = history_.back();
    Serial.printf("[nav] back -> %s (hist=%u)\n", currentUrl_.c_str(), (unsigned)history_.size());
    if (currentUrl_.empty()) {
        page_.blocks.clear();
        buildLines();
        mode_ = PAGE;        // leave ERROR/wherever we were — B must visibly work
        dirty_ = true;
        return;
    }
    std::string html;
    if (store::cacheGet(currentUrl_, html)) {
        lilka::html::Parser p;
        p.feed(html.data(), html.size());
        p.finish();
        page_ = std::move(p.page());
        buildLines();
        mode_ = PAGE;
        lineCursor_ = 0;
        dirty_ = true;
    } else {
        navigate(currentUrl_);   // sets BUSY -> PAGE/ERROR itself
    }
}

void Browser::activate(int lineIdx) {
    Serial.printf("[ui] activate line=%d\n", lineIdx);
    if (lineIdx < 0) return;
    // table row carrying a link (e.g. datasheet tables)
    if (!lines_[lineIdx].href.empty()) {
        std::string target = lines_[lineIdx].href;
        if (target[0] == '/' || (target[0] == '.' && target[1] == '/')) {
            net::Url u;
            if (net::parseUrl(currentUrl_, u)) target = net::resolve(u, target);
        }
        if (isFileUrl(target)) downloadFile(target);
        else navigate(target);
        return;
    }
    const auto& b = page_.blocks[lines_[lineIdx].block];
    if (b.kind == BlockKind::Link && !b.href.empty()) {
        std::string target = b.href;
        if (target[0] == '/' || (target[0] == '.' && target[1] == '/')) {
            net::Url u;
            if (net::parseUrl(currentUrl_, u)) target = net::resolve(u, target);
        }
        if (isFileUrl(target)) downloadFile(target);
        else navigate(target);
    } else if (b.kind == BlockKind::Button) {
        std::string target = b.href.empty() ? b.formAction : b.href;
        if (!target.empty()) {
            if (target[0] == '/' || (target[0] == '.' && target[1] == '/')) {
                net::Url u;
                if (net::parseUrl(currentUrl_, u)) target = net::resolve(u, target);
            }
            auto fields = withChecks(b);
            if (b.formMethod == "POST") submitPost(target, fields, b.name, b.text);
            else navigate(target);
        }
    } else if (b.kind == BlockKind::Input) {
        inputPurpose_ = b.name.empty() ? b.placeholder : b.name;
        inputFormAction_ = b.formAction;
        inputFormMethod_ = b.formMethod;
        inputFormFields_ = b.formFields;
        inputCheckIds_ = b.checkIds;
        kb_.reset();
        inputFrom_ = PAGE;
        mode_ = INPUTMODE;
        dirty_ = true;
    } else if (b.kind == BlockKind::Select) {
        if (!b.options.empty()) {
            // cycle: mutate a working copy kept in expanded_? simplest: rebuild page
            int i = lines_[lineIdx].block;
            page_.blocks[i].selected = (page_.blocks[i].selected + 1) % page_.blocks[i].options.size();
            buildLines();
        }
    }
}

bool Browser::loop() {
    static int lastMode = -1;
    if ((int)mode_ != lastMode) {
        Serial.printf("[ui] mode -> %d (PAGE=0 LINKLIST=1 SETTINGS=2 QUICKSTART=3 INPUT=4 BUSY=5 ERROR=6)\n", (int)mode_);
        lastMode = (int)mode_;
    }
    btns_->poll();
    bool drew = false;
    bool act = false;
    auto just = [&](Buttons::Key k) { return btns_->get(k).justPressed; };

    switch (mode_) {
        case PAGE: {
            // lineCursor_ is the selected line (highlighted). The visible
            // window follows it (renderPage), so A always acts on exactly
            // the line you see highlighted.
            int vis = (gfx_->height() - kHeaderH - kFooterH) / lineH();
            if (just(Buttons::UP)) { lineCursor_ = std::max(0, lineCursor_ - 1); sel_ = -1; act = true; }
            else if (just(Buttons::DOWN)) {
                lineCursor_ = std::min((int)lines_.size() - 1, lineCursor_ + 1); sel_ = -1; act = true;
            } else if (just(Buttons::LEFT)) {
                lineCursor_ = std::max(0, lineCursor_ - vis); sel_ = -1; act = true;
            } else if (just(Buttons::RIGHT)) {
                lineCursor_ = std::min((int)lines_.size() - 1, lineCursor_ + vis); sel_ = -1; act = true;
            } else if (just(Buttons::SELECT)) {
                if (findActive_ && !findMatches_.empty()) {
                    findNext(true);   // cycle to the next match
                } else {
                    // jump selection to the next interactive element (wraps)
                    std::vector<int> cl;
                    frameClickables(cl);
                    if (!cl.empty()) {
                        size_t pos = 0;
                        for (size_t i = 0; i < cl.size(); i++) if (cl[i] == lineCursor_) pos = i;
                        lineCursor_ = cl[(pos + 1) % cl.size()];
                    }
                }
                sel_ = -1;
                act = true;
            } else if (just(Buttons::START)) {
                if (findActive_ && !findMatches_.empty()) findNext(false);  // prev match
                else activate(lineCursor_);
            } else if (just(Buttons::A)) {
                // A = primary action on the element under the cursor:
                // link/button -> activate, input -> keyboard, select -> cycle.
                int bi = lines_[lineCursor_].block;
                if (bi >= 0 && bi < (int)page_.blocks.size()) {
                    auto& blk = page_.blocks[bi];
                    if (blk.kind == BlockKind::Link || blk.kind == BlockKind::Button) {
                        activate(lineCursor_);
                    } else if (blk.kind == BlockKind::Input) {
                        activate(lineCursor_);
                    } else if (blk.kind == BlockKind::Checkbox) {
                        blk.checked = !blk.checked;
                        dirty_ = true;
                    } else if (blk.kind == BlockKind::Select && !blk.options.empty()) {
                        blk.selected = (blk.selected + 1) % blk.options.size();
                        buildLines();
                    }
                }
                act = true;
            } else if (just(Buttons::B)) {
                if (findActive_) {  // B first clears an active find, not navigate back
                    findActive_ = false; findTerm_.clear(); findMatches_.clear();
                    act = true;
                    break;
                }
                if (!currentUrl_.empty() && history_.size() > 1) goBack();
                else { history_.clear(); history_.push_back(""); currentUrl_.clear(); page_.blocks.clear(); buildLines(); }
                act = true;
            } else if (just(Buttons::C)) {
                mode_ = QUICKSTART;
                linklistSel_ = 0;
                act = true;
            } else if (just(Buttons::D)) {
                mode_ = SETTINGS;
                linklistSel_ = 0;
                act = true;
            }
            break;
        }
        case LINKLIST: {
            if (just(Buttons::UP)) linklistSel_ = (linklistSel_ + (int)links_.size() - 1) % links_.size();
            else if (just(Buttons::DOWN)) linklistSel_ = (linklistSel_ + 1) % links_.size();
            else if (just(Buttons::A) || just(Buttons::START)) {
                navigate(links_[linklistSel_].second);
            } else if (just(Buttons::B) || just(Buttons::SELECT)) {
                mode_ = PAGE;
            }
            act = true;
            break;
        }
        case SETTINGS: {
            constexpr int nItems = 7;
            bool touched = false;
            if (just(Buttons::UP)) { linklistSel_ = (linklistSel_ + nItems - 1) % nItems; touched = true; }
            else if (just(Buttons::DOWN)) { linklistSel_ = (linklistSel_ + 1) % nItems; touched = true; }
            else if (just(Buttons::A)) {
                touched = true;
                if (linklistSel_ == 0) {
                    // cycle small -> large -> extra large
                    fontScale_ = (fontScale_ % 3) + 1;
                    store::set("font", std::to_string(fontScale_));
                    buildLines();
                } else if (linklistSel_ == 1) {
                    store::set("cache", store::get("cache", "1") == "1" ? "0" : "1");
                } else if (linklistSel_ == 2) {
                    // Lilka is a Ukrainian project: uk is the default.
                    std::string cur = store::get("lang", "uk");
                    std::string nx = cur == "uk" ? "en" : (cur == "en" ? "ru" : "uk");
                    store::set("lang", nx);
                    net::setAcceptLanguage(nx == "uk" ? "uk,ru;q=0.8" :
                                           nx == "ru" ? "ru,en;q=0.8" : "en");
                } else if (linklistSel_ == 3) {
                    // add current page to favorites (shown in Quick Start)
                    if (currentUrl_.rfind("http", 0) == 0) {
                        store::addFavorite(currentUrl_);
                        Serial.printf("[fav] added %s\n", currentUrl_.c_str());
                    }
                } else if (linklistSel_ == 4) {
                    // compact mode: drop boilerplate nav/footer blocks
                    compact_ = !compact_;
                    store::set("compact", compact_ ? "1" : "0");
                    buildLines();
                } else if (linklistSel_ == 5) {
                    // in-page find: ask for a term, then jump to first match
                    inputPurpose_ = "find";
                    kb_.reset();
                    inputFrom_ = SETTINGS;
                    mode_ = INPUTMODE;
                } else {
                    mode_ = LINKLIST;
                    linklistSel_ = 0;
                }
            } else if (just(Buttons::B)) {
                mode_ = PAGE;
                touched = true;
            }
            if (touched) { dirty_ = true; act = true; }
            break;
        }
        case QUICKSTART: {
            // list size without touching SD: render path recomputes when dirty
            int n = qsSize();
            if (just(Buttons::UP)) { linklistSel_ = (linklistSel_ + n) % (n + 1); dirty_ = true; act = true; }
            else if (just(Buttons::DOWN)) { linklistSel_ = (linklistSel_ + 1) % (n + 1); dirty_ = true; act = true; }
            else if (just(Buttons::A) || just(Buttons::START)) {
                auto list = quickList();
                bool last = linklistSel_ >= (int)list.size() - 1;
                if (last) {
                    // "Enter URL..." entry
                    inputPurpose_ = "url";
                    inputFormAction_.clear();
                    kb_.reset();
                    inputFrom_ = QUICKSTART;
                    mode_ = INPUTMODE;
                } else if (linklistSel_ < n && list[linklistSel_] == kDownloadsEntry) {
                    mode_ = DOWNLOADS; dlSel_ = 0; dlConfirm_ = false;
                } else if (linklistSel_ < n) {
                    navigate(list[linklistSel_]);
                }
                dirty_ = true;
                act = true;
            } else if (just(Buttons::B)) {
                mode_ = PAGE;
                dirty_ = true;
                act = true;
            }
            break;
        }
        case INPUTMODE: {
            // B = close/cancel (global convention). Erasing is the on-screen
            // backspace key (or D-layer BS), not B.
            if (just(Buttons::B)) {
                mode_ = inputFrom_;
                dirty_ = true;
                act = true;
                break;
            }
            if (kb_.update(*btns_)) { dirty_ = true; act = true; }
            if (kb_.isFinished()) {
                std::string v = kb_.value();
                if (inputPurpose_ == "url") {
                    navigate(v);
                } else if (inputPurpose_ == "find") {
                    // in-page find: jump to first match (SELECT cycles the rest)
                    runFind(v);
                    mode_ = PAGE;
                    Serial.printf("[find] '%s' -> %d match(es)\n", v.c_str(), (int)findMatches_.size());
                    if (findMatches_.empty()) {
                        errText_ = "not found: " + v;
                        mode_ = ERROR;
                    }
                } else if (!inputFormAction_.empty()) {
                    std::string action = inputFormAction_;
                    // relative action (e.g. Google's "/search") -> resolve against page host
                    if (action.find("://") == std::string::npos) {
                        net::Url u;
                        if (net::parseUrl(currentUrl_, u)) action = net::resolve(u, action);
                    }
                    if (inputFormMethod_ == "POST") {
                        submitPost(action, withChecksFields(inputFormFields_, inputCheckIds_), inputPurpose_, v);
                    } else {
                        std::string sep = action.find('?') == std::string::npos ? "?" : "&";
                        navigate(action + sep + urlenc(inputPurpose_) + "=" + urlenc(v));
                    }
                }
            }
            break;
        }
        case BUSY:
        case ERROR: {
            // Static screens: redraw ONLY on a state change. Redrawing (and
            // SPI-flushing) every loop made the loop period far exceed the
            // 20ms debounce window, so short B clicks were missed — the user
            // had to mash the key. onStage() already redraws the busy screen
            // inline during the blocking fetch.
            if (just(Buttons::B)) {
                if (mode_ == ERROR && history_.size() > 1) goBack();
                else { mode_ = PAGE; }
                act = true;
            } else if (just(Buttons::C) && mode_ == ERROR) {
                mode_ = QUICKSTART; linklistSel_ = 0; act = true;
            } else if (just(Buttons::D) && mode_ == ERROR) {
                mode_ = SETTINGS; linklistSel_ = 0; act = true;
            }
            break;
        }
        case DONE: {
            if (just(Buttons::B)) { mode_ = PAGE; act = true; }
            else if (just(Buttons::C)) { mode_ = QUICKSTART; linklistSel_ = 0; act = true; }
            else if (just(Buttons::D)) { mode_ = DOWNLOADS; dlSel_ = 0; dlConfirm_ = false; act = true; }
            break;
        }
        case DOWNLOADS: {
            int n = (int)store::downloads(10).size();
            if (just(Buttons::UP)) { dlSel_ = (dlSel_ + n) % (n ? n : 1); dlConfirm_ = false; act = true; }
            else if (just(Buttons::DOWN)) { dlSel_ = (dlSel_ + 1) % (n ? n : 1); dlConfirm_ = false; act = true; }
            else if (just(Buttons::B)) { mode_ = PAGE; dlConfirm_ = false; act = true; }
            else if (just(Buttons::A)) {
                if (n == 0) break;
                auto list = store::downloads(10);
                if (!dlConfirm_) { dlConfirm_ = true; act = true; }
                else {
                    if (store::deleteDownload(list[dlSel_].name))
                        Serial.printf("[dl] deleted %s\n", list[dlSel_].name.c_str());
                    dlConfirm_ = false;
                    dlSel_ = 0;
                    act = true;
                }
            }
            break;
        }
    }

    if (act) dirty_ = true;

    // Live refresh: when a background image lands (done-count changes) while we
    // are viewing the page, repaint so the placeholder turns into the image and
    // the top-bar "(done/total)" counter advances.
    if (mode_ == PAGE && img::total() > 0) {
        int d = img::done();
        if (d != lastImgStatus_) { lastImgStatus_ = d; dirty_ = true; }
    }

    if (dirty_) {
        switch (mode_) {
            case PAGE: renderPage(); break;
            case LINKLIST: renderLinkList(); break;
            case SETTINGS: {
                std::vector<std::string> items = {
                    std::string("Font: ") + (fontScale_ == 1 ? "small" : fontScale_ == 2 ? "large" : "xl"),
                    std::string("Cache: ") + (store::get("cache", "1") == "1" ? "on" : "off"),
                    std::string("Language: ") + store::get("lang", "uk"),
                    std::string("Favorite this page"),
                    std::string("Compact: ") + (compact_ ? "on" : "off"),
                    std::string("Find on page..."),
                    std::string("Link list: enter"),
                };
                renderPopup("Settings (D)", items, linklistSel_);
                break;
            }
            case QUICKSTART: {
                qsCache_ = quickList();  // includes trailing "Enter URL..." slot
                for (auto& e : qsCache_) if (e.empty()) e = "Enter URL...";
                renderPopup("Go (C)", qsCache_, linklistSel_);
                break;
            }
            case INPUTMODE:
                kb_.draw(gfx_, inputPurpose_ == "url" ? "Enter URL" : "Enter: " + inputPurpose_);
                break;
            case BUSY: renderBusy(); break;
            case ERROR: renderError(); break;
            case DONE: renderDone(); break;
            case DOWNLOADS: renderDownloads(); break;
        }
        dirty_ = false;
        drew = true;
    }
    return drew;
}

void Browser::renderPopup(const std::string& title, const std::vector<std::string>& items, int sel) {
    gfx_->fillScreen(0x0000);
    topBar(gfx_, C_ACCENT);
    headerTitle(gfx_, title.c_str(), 0x0000);
    int y = kHeaderH + 6;
    for (size_t i = 0; i < items.size() && y + lineH() < gfx_->height() - kFooterH; i++, y += lineH()) {
        uint16_t c = (int)i == sel ? C_ACCENT : 0xFFFF;
        if ((int)i == sel) {
            gfx_->fillRect(0, y - 1, gfx_->width(), lineH(), 0x1B37);
            c = 0xFFFF;
        }
        font::text(gfx_, (int)i == sel ? 6 : 14, y,
                   font::fit(items[i].c_str(), gfx_->width() - 12, 1), c, 1);
    }
    footer(gfx_, "A=ok B=back", C_DIM);
}

void Browser::renderPage() {
    gfx_->fillScreen(0x0000);
    // header: short url + image-download progress on the right, e.g. "(3/8)"
    std::string h = currentUrl_.empty() ? "Lilka Browser" : currentUrl_;
    if (h.rfind("https://", 0) == 0) h = h.substr(8);
    std::string prog;
    {
        int it = img::total();
        if (it > 0) {
            char b[16]; snprintf(b, sizeof b, "%d/%d", img::done(), it);
            prog = std::string("(") + b + ")";
        }
    }
    topBar(gfx_, C_ACCENT);
    {
        int reserve = prog.empty() ? 0 : (int)font::width(prog.c_str(), 1) + 10;  // counter + gap
        int w = gfx_->width() - 2 * kBarInset - reserve;
        if (w < 20) w = 20;
        font::text(gfx_, kBarInset, 3, font::fit(h.c_str(), w, 1), 0x0000, 1);
    }
    if (!prog.empty()) {
        // Solid black on the bright cyan bar (boombox C_SEL/C_SEL_TEXT convention).
        int tw = font::width(prog.c_str(), 1);
        font::text(gfx_, gfx_->width() - tw - kBarInset, 3, prog.c_str(), 0x0000, 1);
    }

    int y = kHeaderH + 4;
    int vis = 0;
    int top = lineCursor_;
    {  // keep the selection visible, roughly centered in the lower half
        int cap = (gfx_->height() - kHeaderH - kFooterH) / lineH();
        if (cap < 1) cap = 1;
        if (lineCursor_ >= cap) top = lineCursor_ - cap / 2;
        if (top + cap > (int)lines_.size()) top = (int)lines_.size() - cap;
        if (top < 0) top = 0;
    }
    for (int i = top; i < (int)lines_.size(); i++) {
        int lh = lineH(i);
        if (y + lh > gfx_->height() - kFooterH) break;
        uint8_t scale = lineScale(i);
        const Line& l = lines_[i];
        if (l.img) {
            renderImageLine(i, y);
            vis++;
            continue;
        }
        uint16_t c = 0xFFFF;
        if (l.block >= 0 && l.block < (int)page_.blocks.size()) {
            auto k = page_.blocks[l.block].kind;
            if (k == BlockKind::Link) c = C_LINK;
            if (k == BlockKind::Heading) c = 0xFFE0;
            if (k == BlockKind::Input || k == BlockKind::Select || k == BlockKind::Checkbox) c = 0xF800;
            if (k == BlockKind::Text && page_.blocks[l.block].bold) c = 0xFFFF;
        }
        if (i == lineCursor_) {
            gfx_->fillRect(0, y - 1, gfx_->width(), lh, 0x1B37);
            gfx_->fillRect(0, y - 1, 3, lh, C_ACCENT);
        }
        std::string t = l.text;
        if (l.pseudo) c = C_DIM;
        bool isCheck = l.block >= 0 && l.block < (int)page_.blocks.size() &&
                       page_.blocks[l.block].kind == BlockKind::Checkbox;
        if (isCheck) {
            // checkbox glyph + label
            int16_t bx = (i == lineCursor_) ? 8 : 4;
            int16_t by = y + (lh - 12) / 2;
            gfx_->fillRect(bx, by, 12, 12, 0x0000);
            gfx_->drawRect(bx, by, 12, 12, 0xC63F);
            if (page_.blocks[l.block].checked) {
                gfx_->drawLine(bx + 2, by + 6, bx + 5, by + 10, 0xFFFF);
                gfx_->drawLine(bx + 5, by + 10, bx + 10, by + 2, 0xFFFF);
            }
            font::text(gfx_, bx + 18, y, font::fit(t.c_str(), gfx_->width() - bx - 22, scale), c, scale);
        } else if (l.btn) {
            // button: filled box, centered label, beveled bottom edge
            int16_t x0 = (i == lineCursor_) ? 8 : 4;
            int16_t x1 = gfx_->width() - 4;
            int16_t bh = (lh > 18) ? lh - 2 : 16;
            int16_t by = y + (lh - bh) / 2;
            gfx_->fillRect(x0, by, x1 - x0, bh, 0x2458);
            gfx_->fillRect(x0, by + bh - 2, x1 - x0, 2, 0x1224);  // bottom bevel
            gfx_->drawRect(x0, by, x1 - x0, bh, C_ACCENT);
            int16_t tw = font::width(t.c_str(), scale);
            int16_t tx = x0 + std::max<int16_t>(2, ((x1 - x0) - tw) / 2);
            font::text(gfx_, tx, by + (bh - 13 * scale) / 2 + (scale > 1 ? 2 : 0),
                       font::fit(t.c_str(), (int16_t)(x1 - x0 - 6), scale), 0xFFFF, scale);
        } else if (l.field) {
            // input field: rectangle outline with the caption inside
            int16_t x0 = (i == lineCursor_) ? 8 : 4;
            int16_t x1 = gfx_->width() - 4;
            gfx_->drawRect(x0, y - 1, x1 - x0, lh, C_DIM);
            font::text(gfx_, x0 + 4, y + (lh > 16 ? 3 : 0),
                       font::fit(t.c_str(), (int16_t)(x1 - x0 - 8), scale), c, scale);
        } else {
            font::text(gfx_, (i == lineCursor_) ? 8 : 4, y, font::fit(t.c_str(), gfx_->width() - 10, scale), c, scale);
        }
        y += lh;
        vis++;
    }
    if (page_.blocks.empty()) {
        font::text(gfx_, 10, 60, "Press C to open a page", C_DIM, 1);
    }
    // footer
    if (findActive_ && !findMatches_.empty()) {
        char fbuf[80];
        snprintf(fbuf, sizeof(fbuf), "find '%s' %d/%d  SEL=next B=done",
                 findTerm_.c_str(), findPos_ + 1, (int)findMatches_.size());
        footer(gfx_, fbuf, C_LINK);
    } else {
        footer(gfx_, "UD=move SEL=jump A=act B=back", C_DIM);
    }
}

void Browser::renderLinkList() {
    int n = (int)links_.size();
    if (!n) { mode_ = PAGE; renderPage(); return; }
    int vis = (gfx_->height() - kHeaderH - kFooterH) / lineH();
    int top = std::max(0, linklistSel_ - vis / 2);
    gfx_->fillScreen(0x0000);
    topBar(gfx_, C_ACCENT);
    headerTitle(gfx_, ("Links: " + std::to_string(n)).c_str(), 0x0000);
    int y = kHeaderH + 4;
    for (int i = 0; i < vis && top + i < n; i++, y += lineH()) {
        int idx = top + i;
        char num[8];
        snprintf(num, sizeof(num), "%2d ", idx + 1);
        if (idx == linklistSel_) {
            gfx_->fillRect(0, y - 1, gfx_->width(), lineH(), 0x1B37);
        }
        font::text(gfx_, 4, y, num, C_DIM, 1);
        font::text(gfx_, 26, y,
                   font::fit(links_[idx].first.c_str(), gfx_->width() - 30, 1),
                   idx == linklistSel_ ? 0xFFFF : C_LINK, 1);
    }
    footer(gfx_, "A=open B=back", C_DIM);
}

void Browser::renderBusy() {
    gfx_->fillScreen(0x0000);
    if (busyDownload_) {
        // ---- Download screen: progress bar + received bytes ----
        font::text(gfx_, 10, 44, "Downloading...", 0xFFFF, 2);
        // filename (last path segment) if we can pull one out of the URL
        std::string url = busyUrl_;
        size_t q = url.find_first_of("?#");
        if (q != std::string::npos) url = url.substr(0, q);
        size_t slash = url.find_last_of('/');
        std::string fname = (slash == std::string::npos) ? url : url.substr(slash + 1);
        if (fname.empty() || fname.find('.') == std::string::npos) fname += ".bin";
        font::text(gfx_, 10, 76, font::fit(fname.c_str(), gfx_->width() - 20, 1), 0xFFFF, 1);

        int bx = 10, by = 104, bw = gfx_->width() - 20, bh = 14;
        gfx_->drawRect(bx, by, bw, bh, C_DIM);
        int fillw = 0;
        if (busyTotal_ > 0 && busyBytes_ > 0) {
            fillw = (int)((long)bw * busyBytes_ / busyTotal_);
            if (fillw > bw) fillw = bw;
        }
        if (fillw > 0) gfx_->fillRect(bx + 1, by + 1, fillw - 2 > 0 ? fillw - 2 : 1, bh - 2, C_ACCENT);

        char s[40];
        if (busyTotal_ > 0) snprintf(s, sizeof s, "%lu.%luKB / %luKB",
                          (unsigned long)(busyBytes_ / 1024), (unsigned long)((busyBytes_ / 100) % 10),
                          (unsigned long)(busyTotal_ / 1024));
        else snprintf(s, sizeof s, "%lu.%luKB received",
                          (unsigned long)(busyBytes_ / 1024), (unsigned long)((busyBytes_ / 100) % 10));
        font::text(gfx_, 10, by + 20, s, 0xFFFF, 1);
        if (busyT0_) {
            char t[16];
            snprintf(t, sizeof t, "%lus", (unsigned long)((millis() - busyT0_) / 1000));
            font::text(gfx_, 10, by + 38, t, C_DIM, 1);
        }
        footer(gfx_, "please wait...", C_DIM);
        return;
    }

    font::text(gfx_, 10, 60, "Loading...", 0xFFFF, 2);
    // target address (wrapped, max 4 lines)
    std::vector<std::string> ws;
    if (!busyUrl_.empty()) wrapWords(busyUrl_, gfx_->width() - 20, ws);
    int y = 84;
    for (size_t i = 0; i < ws.size() && i < 4; i++) {
        font::text(gfx_, 10, y, ws[i].c_str(), C_DIM, 1);
        y += 11;
    }
    if (!busyStage_.empty()) font::text(gfx_, 10, 160, busyStage_.c_str(), C_DIM, 1);
    if (busyT0_) {
        char t[16];
        snprintf(t, sizeof(t), "%lus", (unsigned long)((millis() - busyT0_) / 1000));
        font::text(gfx_, 10, 180, t, C_DIM, 1);
    }
}

void Browser::onStage(const char* s) {
    busyStage_ = s ? s : "";
    // Track download progress from "dl 123KB" / "dl 123KB/4041KB" stage strings.
    if (busyDownload_ && busyStage_.rfind("dl ", 0) == 0) {
        const char* p = busyStage_.c_str() + 3;
        long done = atol(p);
        busyBytes_ = done * 1024;
        size_t slash = busyStage_.find('/', 3);
        if (slash != std::string::npos) {
            long tot = atol(busyStage_.c_str() + slash + 1);
            busyTotal_ = tot * 1024;
        }
    }
    if (mode_ == BUSY) {
        renderBusy();
        gfx_->flush();  // fetch blocks the main loop — flush inline
    }
    dirty_ = true;
}

void Browser::renderError() {
    gfx_->fillScreen(0x0000);
    topBar(gfx_, 0x7B00);
    headerTitle(gfx_, "Error", 0xFFFF);
    // The bare "http NNN" is cryptic; add a short, actionable note for the
    // common "the server refused us" statuses.
    std::string shown = errText_;
    if (shown.rfind("http 403", 0) == 0)
        shown += "\n\nThe site refused this client. Cloudflare's bot-detection fingerprints the ESP32's TLS stack and blocks it — it loads in a normal browser, not here (not a fixable bug).";
    else if (shown.rfind("http 429", 0) == 0)
        shown += "\n\nThe site is rate-limiting you. Wait a bit, then retry.";
    else if (shown.rfind("http 503", 0) == 0)
        shown += "\n\nThe site is temporarily unavailable.";
    std::vector<std::string> ws;
    wrapWords(shown, gfx_->width() - 12, ws);
    int y = kHeaderH + 10;
    for (auto& w : ws) {
        font::text(gfx_, 8, y, font::fit(w.c_str(), gfx_->width() - 12, 1), 0xFFFF, 1);
        y += lineH();
    }
    footer(gfx_, "B=back C=home D=settings", C_DIM);
}

void Browser::renderDone() {
    gfx_->fillScreen(0x0000);
    topBar(gfx_, 0x1D6);
    headerTitle(gfx_, "Done", 0xFFFF);
    std::vector<std::string> ws;
    wrapWords(doneText_, gfx_->width() - 12, ws);
    int y = kHeaderH + 10;
    for (auto& w : ws) {
        font::text(gfx_, 8, y, font::fit(w.c_str(), gfx_->width() - 12, 1), 0xFFFF, 1);
        y += lineH();
    }
    footer(gfx_, "B=back C=home", C_DIM);
}

void Browser::renderDownloads() {
    gfx_->fillScreen(0x0000);
    topBar(gfx_, C_ACCENT);
    headerTitle(gfx_, "Downloads", 0xFFFF);
    auto list = store::downloads(10);
    if (list.empty()) {
        font::text(gfx_, 8, kHeaderH + 10, "(empty)", C_DIM, 1);
    } else {
        if (dlSel_ >= (int)list.size()) dlSel_ = 0;
        int y = kHeaderH + 2;
        int h = gfx_->height() - kHeaderH - kFooterH;
        int per = h / lineH();
        int start = dlSel_ > per / 2 ? dlSel_ - per / 2 : 0;
        if (start + per > (int)list.size()) start = (int)list.size() - per;
        if (start < 0) start = 0;
        for (int i = start; i < start + per && i < (int)list.size(); i++) {
            const auto& e = list[i];
            char sz[24];
            snprintf(sz, sizeof sz, e.size >= 1048576 ? "%u.%uMB" : "%uKB",
                     (unsigned)(e.size / (e.size >= 1048576 ? 1048576 : 1024)),
                     e.size >= 1048576 ? (unsigned)((e.size / 102400) % 10) : 0u);
            std::string label = font::fit(e.name.c_str(), gfx_->width() - 56, 1);
            label += "  ";
            label += sz;
            uint16_t bg = (i == dlSel_) ? C_ACCENT : 0x0000;
            uint16_t fg = (i == dlSel_) ? 0x0000 : 0xDDDD;  // black on the cyan highlight = readable
            if (bg) gfx_->fillRect(0, y, gfx_->width(), lineH(), bg);
            font::text(gfx_, 6, y, label.c_str(), fg, 1);
            y += lineH();
        }
    }
    std::string hint = dlConfirm_ ? std::string("A=confirm delete") :
        ((int)store::downloads(1).size() ? "UD=move A=del B=back" : "B=back");
    footer(gfx_, hint.c_str(), C_DIM);
}

bool Browser::isFileUrl(const std::string& url) {
    // strip query/fragment
    std::string u = url;
    for (char ch : "?#") {
        size_t p = u.find(ch);
        if (p != std::string::npos) u = u.substr(0, p);
    }
    // last path segment
    size_t slash = u.find_last_of('/');
    std::string name = (slash == std::string::npos) ? u : u.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == name.size() - 1) return false;
    std::string ext = name.substr(dot + 1);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    // txt / md are NOT here on purpose: text files open inline as plain text
    static const char* kExt[] = {
        "zip", "tar", "gz", "tgz", "7z", "rar", "bz2", "xz", "pdf",
        "csv", "json", "xml", "bin", "hex", "elf", "uf2", "mp3", "wav", "ogg",
        "png", "jpg", "jpeg", "gif", "webp", "svg", "ttf", "otf", "woff", "woff2",
        "exe", "dmg", "apk", "deb", "rpm", "iso", "webm", "mp4",
    };
    for (auto* e : kExt) if (ext == e) return true;
    return false;
}

void Browser::downloadFile(const std::string& urlIn) {
    std::string url = net::rewriteUrl(urlIn);
    Serial.printf("[dl] download: %s\n", url.c_str());

    net::setStageFn([](const char* s) { g_browser.onStage(s); });
    busyStage_ = "start";
    busyDownload_ = true;
    busyBytes_ = 0;
    busyTotal_ = -1;
    busyUrl_ = url;
    busyT0_ = millis();
    mode_ = BUSY;
    dirty_ = true;
    renderBusy();
    gfx_->flush();

    std::string err, finalUrl, ct;
    bool got = false;
    long sz = -1;
    std::string ssid, pass;

    // filename from the URL's last path segment (used as the SD destination).
    std::string name;
    {
        std::string u = url;
        size_t q = u.find_first_of("?#");
        if (q != std::string::npos) u = u.substr(0, q);
        size_t slash = u.find_last_of('/');
        name = (slash == std::string::npos) ? u : u.substr(slash + 1);
    }
    if (name.empty() || name.find('.') == std::string::npos) name += ".bin";
    if (!SD.exists("/browser/downloads")) SD.mkdir("/browser/downloads");
    std::string sdPath = std::string("/browser/downloads/") + name;

    if (!net::wifiCreds(ssid, pass)) err = "no wifi creds (set up in Keira)";
    else if (WiFi.status() != WL_CONNECTED && !net::connect(ssid, pass, 20000)) err = "wifi connect failed";
    else {
        net::Client c;
        sz = c.download(url, sdPath, err, finalUrl, ct, 180000);  // 3 min for large files
        Serial.printf("[dl] download rc=%ld ct=%s\n", sz, ct.c_str());
        if (sz == -2) {
            // Server actually served text (HTML/plain) — render it as a page.
            Serial.printf("[dl] response is text (%s) -> render as page\n", ct.c_str());
            std::string html, pageErr;
            if (c.get(url, html, finalUrl, pageErr, 30000)) {
                finishLoad(url, std::move(html), finalUrl);
            } else {
                errText_ = pageErr.empty() ? "render failed" : pageErr;
                mode_ = ERROR;
                dirty_ = true;
            }
            return;
        }
        if (sz >= 0) got = true;
    }

    if (!got) {
        errText_ = err.empty() ? std::string("download failed") : err;
        mode_ = ERROR;
        dirty_ = true;
        return;
    }

    char human[24];
    if (sz >= 1048576) snprintf(human, sizeof human, "%.1fMB", (double)sz / 1048576.0);
    else snprintf(human, sizeof human, "%uKB", (unsigned)(sz / 1024));
    doneText_ = std::string(name) + " (" + human + ")\n" + sdPath;
    Serial.printf("[dl] saved %s (%ld bytes)\n", name.c_str(), sz);
    mode_ = DONE;
    dirty_ = true;
}

}  // namespace ui
