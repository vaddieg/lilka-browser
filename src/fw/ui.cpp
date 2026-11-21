#include "ui.h"
#include "font.h"
#include "net.h"
#include "store.h"
#include "html_parser.h"
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
const uint16_t C_ACCENT = 0x7E6E;
const uint16_t C_LINK = 0x7CFF;      // cyan-ish for links
const uint16_t C_DIM = 0x841C;


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

int Browser::lineH() const { return fontScale_ >= 2 ? 22 : 15; }

uint8_t Browser::lineScale(int lineIdx) const {
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
    return lineScale(lineIdx) >= 2 ? 22 : 15;
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

bool Browser::begin(Arduino_GFX* gfx) {
    gfx_ = gfx;
    font::begin(gfx);
    if (store::begin()) {
        if (store::get("font", "1") == "2") fontScale_ = 2;
    }
    history_.push_back("");  // home slot
    currentUrl_ = "";
    return true;
}

bool Browser::clickable(int lineIdx) const {
    if (lineIdx < 0 || lineIdx >= (int)lines_.size()) return false;
    const Line& l = lines_[lineIdx];
    if (l.block < 0 || l.block >= (int)page_.blocks.size()) return false;
    auto k = page_.blocks[l.block].kind;
    return k == BlockKind::Link || k == BlockKind::Button ||
           k == BlockKind::Input || k == BlockKind::Select;
}

void Browser::buildLines() {
    lines_.clear();
    links_.clear();
    const int16_t maxW = gfx_ ? gfx_->width() - 10 : 230;
    for (int i = 0; i < (int)page_.blocks.size(); i++) {
        const auto& b = page_.blocks[i];
        char buf[64];
        switch (b.kind) {
            case BlockKind::Heading: {
                uint8_t hscale = (fontScale_ >= 2 || b.level <= 4) ? 2 : 1;
                std::vector<std::string> ws;
                wrapWords(b.text, maxW, ws, hscale);
                for (auto& w : ws) lines_.push_back({i, w, false});
                break;
            }
            case BlockKind::Text:
            case BlockKind::ListItem: {
                // full text, no expand/collapse (A had no focusable target anyway)
                uint8_t tscale = fontScale_ >= 2 ? 2 : 1;
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
            case BlockKind::Button:
                lines_.push_back({i, b.text.empty() ? "OK" : b.text, false});
                break;
            case BlockKind::Input:
                snprintf(buf, sizeof(buf), "[input] %s",
                         b.placeholder.empty() ? b.name.c_str() : b.placeholder.c_str());
                lines_.push_back({i, buf, false});
                break;
            case BlockKind::Table:
                for (auto& r : b.rows) {
                    std::string t = r.k + (r.k.empty() ? "" : ": ") + r.v;
                    lines_.push_back({i, t, true});
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
        }
    }
    lineCursor_ = 0;
    sel_ = -1;
}

void Browser::frameClickables(std::vector<int>& out) const {
    out.clear();
    int y = kHeaderH + 4;
    for (int i = lineCursor_; i < (int)lines_.size(); i++) {
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

void Browser::navigate(const std::string& urlIn) {
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
    if (url == currentUrl_) return;

    net::setStageFn([](const char* s) { g_browser.onStage(s); });
    std::string lang = store::get("lang", "uk");
    net::setAcceptLanguage(lang == "uk" ? "uk,ru;q=0.8" : lang == "ru" ? "ru,en;q=0.8" : "en");
    busyStage_ = "start";
    busyUrl_ = url;
    busyT0_ = millis();
    mode_ = BUSY;
    dirty_ = true;
    renderBusy();
    gfx_->flush();

    std::string err, html, finalUrl;
    bool fromCache = false;
    if (store::get("cache", "1") == "1") {
        if (store::cacheGet(url, html)) fromCache = true;
    }
    if (!fromCache) {
        std::string ssid, pass;
        Serial.printf("[nav] wifiCreds...\n");
        if (!net::wifiCreds(ssid, pass)) {
            Serial.printf("[nav] no creds\n");
            err = "no wifi creds (set up in Keira)";
        } else if (WiFi.status() != WL_CONNECTED && !net::connect(ssid, pass, 20000)) {
            Serial.printf("[nav] wifi connect FAILED\n");
            err = "wifi connect failed";
        } else {
            Serial.printf("[nav] GET %s ...\n", url.c_str());
            net::Client c;
            if (!c.get(url, html, finalUrl, err)) {
                Serial.printf("[nav] GET failed: %s\n", err.c_str());
            } else {
                Serial.printf("[nav] got %u bytes, final %s\n", (unsigned)html.size(), finalUrl.c_str());
                store::cachePut(finalUrl, html);
            }
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

void Browser::finishLoad(const std::string& url, const std::string& html, const std::string& finalUrl) {
    // parse
    Serial.printf("[nav] parsing...\n");
    lilka::html::Parser p;
    p.feed(html.data(), html.size());
    p.finish();
    page_ = p.page();
    lastHtml_ = html;  // keep for the `dump` serial command
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
    if (line.rfind("find", 0) == 0) {
        // find <text> -> list matching line numbers (case-insensitive)
        std::string needle = line.substr(5);
        while (!needle.empty() && needle.front() == ' ') needle.erase(needle.begin());
        bool found = false;
        auto fold = [](std::string& s) {  // ASCII-only: tolower() corrupts UTF-8
            for (auto& ch : s) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        };
        for (int i = 0; i < (int)lines_.size(); i++) {
            std::string t = lines_[i].text;
            std::string n2 = needle;
            fold(t); fold(n2);
            if (t.find(n2) != std::string::npos) {
                Serial.printf("[cmd] found line %d: '%s'\n", i, lines_[i].text.c_str());
                found = true;
            }
        }
        if (!found) Serial.printf("[cmd] not found: %s\n", needle.c_str());
        else Serial.println("unknown key");
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
        if (url.size() > 4 && url.rfind("http", 0) != 0) url = "https://" + url;
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
    if (history_.size() > 1) {
        history_.pop_back();
        currentUrl_ = history_.back();
        if (currentUrl_.empty()) {
            page_.blocks.clear();
            buildLines();
        } else {
            std::string html;
            if (store::cacheGet(currentUrl_, html)) {
                lilka::html::Parser p;
                p.feed(html.data(), html.size());
                p.finish();
                page_ = p.page();
                buildLines();
            } else {
                navigate(currentUrl_);
            }
        }
    }
}

void Browser::activate(int lineIdx) {
    Serial.printf("[ui] activate line=%d\n", lineIdx);
    if (lineIdx < 0) return;
    const auto& b = page_.blocks[lines_[lineIdx].block];
    if (b.kind == BlockKind::Link && !b.href.empty()) {
        std::string target = b.href;
        if (target[0] == '/' || (target[0] == '.' && target[1] == '/')) {
            net::Url u;
            if (net::parseUrl(currentUrl_, u)) target = net::resolve(u, target);
        }
        navigate(target);
    } else if (b.kind == BlockKind::Button) {
        std::string target = b.href.empty() ? b.formAction : b.href;
        if (!target.empty()) {
            if (target[0] == '/' || (target[0] == '.' && target[1] == '/')) {
                net::Url u;
                if (net::parseUrl(currentUrl_, u)) target = net::resolve(u, target);
            }
            if (b.formMethod == "POST") submitPost(target, b.formFields, b.name, b.text);
            else navigate(target);
        }
    } else if (b.kind == BlockKind::Input) {
        inputPurpose_ = b.name.empty() ? b.placeholder : b.name;
        inputFormAction_ = b.formAction;
        inputFormMethod_ = b.formMethod;
        inputFormFields_ = b.formFields;
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
            int vis = (gfx_->height() - kHeaderH - kFooterH) / lineH();
            if (just(Buttons::UP)) { lineCursor_ = std::max(0, lineCursor_ - 1); act = true; }
            else if (just(Buttons::DOWN)) {
                lineCursor_ = std::min((int)lines_.size() - 1, lineCursor_ + 1); act = true;
            } else if (just(Buttons::LEFT)) {
                lineCursor_ = std::max(0, lineCursor_ - vis); act = true;
            } else if (just(Buttons::RIGHT)) {
                lineCursor_ = std::min((int)lines_.size() - 1, lineCursor_ + vis); act = true;
            } else if (just(Buttons::SELECT)) {
                std::vector<int> cl;
                frameClickables(cl);
                if (cl.empty()) act = true;
                else {
                    size_t pos = 0;
                    for (size_t i = 0; i < cl.size(); i++) if (cl[i] == sel_) pos = i;
                    sel_ = cl[(pos + 1) % cl.size()];
                    act = true;
                }
            } else if (just(Buttons::START)) {
                if (sel_ >= 0) activate(sel_);
                else {
                    std::vector<int> cl;
                    frameClickables(cl);
                    if (!cl.empty()) { sel_ = cl[0]; activate(sel_); }
                }
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
                    } else if (blk.kind == BlockKind::Select && !blk.options.empty()) {
                        blk.selected = (blk.selected + 1) % blk.options.size();
                        buildLines();
                    }
                }
                act = true;
            } else if (just(Buttons::B)) {
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
            static const char* items[] = {
                "Font size", "Page cache", "Language", "Link list",
            };
            if (just(Buttons::UP)) linklistSel_ = (linklistSel_ + 3) % 4;
            else if (just(Buttons::DOWN)) linklistSel_ = (linklistSel_ + 1) % 4;
            else if (just(Buttons::A)) {
                if (linklistSel_ == 0) {
                    fontScale_ = fontScale_ == 1 ? 2 : 1;
                    store::set("font", fontScale_ == 1 ? "1" : "2");
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
                } else {
                    mode_ = LINKLIST;
                    linklistSel_ = 0;
                }
            } else if (just(Buttons::B)) {
                mode_ = PAGE;
            }
            act = true;
            break;
        }
        case QUICKSTART: {
            auto recent = recentList();
            int n = (int)recent.size();
            if (just(Buttons::UP)) { linklistSel_ = (linklistSel_ + n) % (n + 1); Serial.printf("[qs] sel=%d/%d\n", linklistSel_, n); }
            else if (just(Buttons::DOWN)) { linklistSel_ = (linklistSel_ + 1) % (n + 1); Serial.printf("[qs] sel=%d/%d\n", linklistSel_, n); }
            else if (just(Buttons::A) || just(Buttons::START)) {
                Serial.printf("[qs] activate sel=%d n=%d\n", linklistSel_, n);
                if (linklistSel_ < n) {
                    navigate(recent[linklistSel_]);
                } else {
                    inputPurpose_ = "url";
                    inputFormAction_.clear();
                    kb_.reset();
                    inputFrom_ = QUICKSTART;
                    mode_ = INPUTMODE;
                }
            } else if (just(Buttons::B)) {
                mode_ = PAGE;
            }
            act = true;
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
            kb_.update(*btns_);
            if (kb_.isFinished()) {
                std::string v = kb_.value();
                if (inputPurpose_ == "url") {
                    navigate(v);
                } else if (!inputFormAction_.empty()) {
                    std::string action = inputFormAction_;
                    // relative action (e.g. Google's "/search") -> resolve against page host
                    if (action.find("://") == std::string::npos) {
                        net::Url u;
                        if (net::parseUrl(currentUrl_, u)) action = net::resolve(u, action);
                    }
                    if (inputFormMethod_ == "POST") {
                        submitPost(action, inputFormFields_, inputPurpose_, v);
                    } else {
                        std::string sep = action.find('?') == std::string::npos ? "?" : "&";
                        navigate(action + sep + urlenc(inputPurpose_) + "=" + urlenc(v));
                    }
                }
            }
            act = true;
            break;
        }
        case BUSY:
        case ERROR: {
            if (just(Buttons::B)) {
                if (mode_ == ERROR && history_.size() > 1) goBack();
                else { mode_ = PAGE; }
            }
            act = true;
            break;
        }
    }

    if (act) dirty_ = true;

    if (dirty_) {
        switch (mode_) {
            case PAGE: renderPage(); break;
            case LINKLIST: renderLinkList(); break;
            case SETTINGS: {
                std::vector<std::string> items = {
                    std::string("Font: ") + (fontScale_ == 1 ? "small" : "large"),
                    std::string("Cache: ") + (store::get("cache", "1") == "1" ? "on" : "off"),
                    std::string("Language: ") + store::get("lang", "uk"),
                    std::string("Link list: enter"),
                };
                renderPopup("Settings (D)", items, linklistSel_);
                break;
            }
            case QUICKSTART: {
                auto items = recentList();
                items.push_back("Enter URL...");
                renderPopup("Go (C)", items, linklistSel_);
                break;
            }
            case INPUTMODE:
                kb_.draw(gfx_, inputPurpose_ == "url" ? "Enter URL" : "Enter: " + inputPurpose_);
                break;
            case BUSY: renderBusy(); break;
            case ERROR: renderError(); break;
        }
        dirty_ = false;
        drew = true;
    }
    return drew;
}

void Browser::renderPopup(const std::string& title, const std::vector<std::string>& items, int sel) {
    gfx_->fillScreen(0x0000);
    gfx_->fillRect(0, 0, gfx_->width(), kHeaderH, C_ACCENT);
    font::text(gfx_, 6, 3, title.c_str(), 0x0000, 1);
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
    font::text(gfx_, 4, gfx_->height() - kFooterH + 1, "A=ok B=back", C_DIM, 1);
}

void Browser::renderPage() {
    gfx_->fillScreen(0x0000);
    // header: short url
    std::string h = currentUrl_.empty() ? "Lilka Browser" : currentUrl_;
    if (h.rfind("https://", 0) == 0) h = h.substr(8);
    gfx_->fillRect(0, 0, gfx_->width(), kHeaderH, C_ACCENT);
    font::text(gfx_, 6, 3, font::fit(h.c_str(), gfx_->width() - 10, 1), 0x0000, 1);

    int y = kHeaderH + 4;
    int vis = 0;
    for (int i = lineCursor_; i < (int)lines_.size(); i++) {
        int lh = lineH(i);
        if (y + lh > gfx_->height() - kFooterH) break;
        uint8_t scale = lineScale(i);
        const Line& l = lines_[i];
        uint16_t c = 0xFFFF;
        if (l.block >= 0 && l.block < (int)page_.blocks.size()) {
            auto k = page_.blocks[l.block].kind;
            if (k == BlockKind::Link) c = C_LINK;
            if (k == BlockKind::Heading) c = 0xFFE0;
            if (k == BlockKind::Input || k == BlockKind::Select) c = 0xF800;
            if (k == BlockKind::Text && page_.blocks[l.block].bold) c = 0xFFFF;
        }
        if (i == sel_) {
            gfx_->fillRect(0, y - 1, gfx_->width(), lh, 0x1B37);
            gfx_->fillRect(0, y - 1, 3, lh, C_ACCENT);
        }
        std::string t = l.text;
        if (l.pseudo) c = C_DIM;
        font::text(gfx_, (i == sel_ ? 8 : 4), y, font::fit(t.c_str(), gfx_->width() - 10, scale), c, scale);
        y += lh;
        vis++;
    }
    if (page_.blocks.empty()) {
        font::text(gfx_, 10, 60, "Press C to open a page", C_DIM, 1);
    }
    // footer
    font::text(gfx_, 4, gfx_->height() - kFooterH + 1,
               "UD=scroll SEL=pick A=act B=back", C_DIM, 1);
}

void Browser::renderLinkList() {
    int n = (int)links_.size();
    if (!n) { mode_ = PAGE; renderPage(); return; }
    int vis = (gfx_->height() - kHeaderH - kFooterH) / lineH();
    int top = std::max(0, linklistSel_ - vis / 2);
    gfx_->fillScreen(0x0000);
    gfx_->fillRect(0, 0, gfx_->width(), kHeaderH, C_ACCENT);
    font::text(gfx_, 6, 3, font::fit(("Links: " + std::to_string(n)).c_str(), gfx_->width() - 10, 1),
               0x0000, 1);
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
    font::text(gfx_, 4, gfx_->height() - kFooterH + 1, "A=open B=back", C_DIM, 1);
}

void Browser::renderBusy() {
    gfx_->fillScreen(0x0000);
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
    if (mode_ == BUSY) {
        renderBusy();
        gfx_->flush();  // fetch blocks the main loop — flush inline
    }
    dirty_ = true;
}

void Browser::renderError() {
    gfx_->fillScreen(0x0000);
    gfx_->fillRect(0, 0, gfx_->width(), kHeaderH, 0x7B00);
    font::text(gfx_, 6, 3, "Error", 0xFFFF, 1);
    std::vector<std::string> ws;
    wrapWords(errText_, gfx_->width() - 12, ws);
    int y = kHeaderH + 10;
    for (auto& w : ws) {
        font::text(gfx_, 8, y, font::fit(w.c_str(), gfx_->width() - 12, 1), 0xFFFF, 1);
        y += lineH();
    }
    font::text(gfx_, 4, gfx_->height() - kFooterH + 1, "B=back", C_DIM, 1);
}

}  // namespace ui
