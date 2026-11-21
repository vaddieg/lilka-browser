// Implementation of html_parser.h (included from it).
namespace lilka {
namespace html {

namespace {

const char* namedEntity(const char* s, size_t n) {
    struct E { const char* name; size_t len; const char* utf8; };
    static const E tbl[] = {
        {"amp", 3, "&"},    {"lt", 2, "<"},    {"gt", 2, ">"},
        {"quot", 4, "\""},  {"apos", 4, "'"},  {"nbsp", 4, "\xC2\xA0"},
        {"rsquo", 5, "\xE2\x80\x99"}, {"lsquo", 5, "\xE2\x80\x98"},
        {"ldquo", 5, "\xE2\x80\x9C"}, {"rdquo", 5, "\xE2\x80\x9D"},
        {"mdash", 5, "\xE2\x80\x94"}, {"ndash", 5, "\xE2\x80\x93"},
        {"hellip", 6, "\xE2\x80\xA6"}, {"euro", 4, "\xE2\x82\xAC"},
        {"copy", 4, "\xC2\xA9"},
        {"middot", 6, "\xC2\xB7"}, {"bull", 4, "\xE2\x80\xA2"},
        {"trade", 5, "\xE2\x84\xA2"}, {"reg", 3, "\xC2\xAE"},
        {"sect", 4, "\xC2\xA7"}, {"deg", 3, "\xC2\xB0"},
        {"plusmn", 6, "\xC2\xB1"}, {"times", 5, "\xC3\x97"},
        {"divide", 6, "\xC3\xB7"},
    };
    for (const auto& e : tbl)
        if (e.len == n && !std::memcmp(s, e.name, n)) return e.utf8;
    return nullptr;
}

void appendUtf8(std::string& out, unsigned code) {
    char buf[5];
    size_t n = 0;
    if (code < 0x80) buf[n++] = (char)code;
    else if (code < 0x800) {
        buf[n++] = (char)(0xC0 | (code >> 6));
        buf[n++] = (char)(0x80 | (code & 63));
    } else if (code < 0x10000) {
        buf[n++] = (char)(0xE0 | (code >> 12));
        buf[n++] = (char)(0x80 | ((code >> 6) & 63));
        buf[n++] = (char)(0x80 | (code & 63));
    } else {
        buf[n++] = (char)(0xF0 | (code >> 18));
        buf[n++] = (char)(0x80 | ((code >> 12) & 63));
        buf[n++] = (char)(0x80 | ((code >> 6) & 63));
        buf[n++] = (char)(0x80 | (code & 63));
    }
    out.append(buf, n);
}

// extract value of attribute `name` from lowercased raw tag body
// decode &amp;/&#..; in an extracted attribute value (used for hrefs)
std::string unescapeAttr(std::string v) {
    std::string out;
    out.reserve(v.size());
    size_t i = 0;
    while (i < v.size()) {
        if (v[i] == '&') {
            size_t j = i + 1;
            while (j < v.size() && v[j] != ';' && j - i < 12) j++;
            if (j < v.size() && v[j] == ';') {
                std::string e = v.substr(i + 1, j - i - 1);
                if (e == "amp") { out += '&'; i = j + 1; continue; } }
        }
        out += v[i++];
    }
    return out;
}

std::string attrValue(const std::string& tag, const char* name) {
    size_t nlen = std::strlen(name);
    size_t i = 0;
    while (i + nlen < tag.size()) {
        bool match = true;
        for (size_t k = 0; k < nlen; k++) {
            if (std::tolower((unsigned char)tag[i + k]) != std::tolower((unsigned char)name[k])) {
                match = false;
                break;
            }
        }
        if (match && (i == 0 || std::isspace((unsigned char)tag[i - 1]) || tag[i - 1] == '/')) {
            size_t p = i + nlen;
            while (p < tag.size() && std::isspace((unsigned char)tag[p])) p++;
            if (p < tag.size() && tag[p] == '=') {
                p++;
                while (p < tag.size() && std::isspace((unsigned char)tag[p])) p++;
                if (p >= tag.size()) return "";
                char q = tag[p];
                if (q == '"' || q == '\'') {
                    size_t e = tag.find(q, p + 1);
                    if (e == std::string::npos) return tag.substr(p + 1);
                    return tag.substr(p + 1, e - p - 1);
                }
                size_t e = p;
                while (e < tag.size() && !std::isspace((unsigned char)tag[e]) && tag[e] != '>') e++;
                return tag.substr(p, e - p);
            }
        }
        i++;
    }
    return "";
}

bool hasAttr(const std::string& tag, const char* name) {
    size_t nlen = std::strlen(name);
    size_t i = 0;
    while (i + nlen <= tag.size()) {
        bool match = true;
        for (size_t k = 0; k < nlen; k++) {
            if (std::tolower((unsigned char)tag[i + k]) != std::tolower((unsigned char)name[k])) {
                match = false;
                break;
            }
        }
        if (match && (i == 0 || std::isspace((unsigned char)tag[i - 1]) || tag[i - 1] == '/')) {
            size_t p = i + nlen;
            if (p == tag.size() || std::isspace((unsigned char)tag[p]) || tag[p] == '=' || tag[p] == '>')
                return true;
        }
        i++;
    }
    return false;
}

std::string trimCollapse(const std::string& in) {
    size_t a = in.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = in.find_last_not_of(" \t\r\n");
    std::string s = in.substr(a, b - a + 1);
    for (auto& ch : s) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
    return s;
}

bool hexColor(const std::string& v, unsigned& out) {
    std::string s = v;
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (s.size() != 3 && s.size() != 6) return false;
    for (char c : s) if (!std::isxdigit((unsigned char)c)) return false;
    out = (unsigned)strtoul(s.c_str(), nullptr, 16);
    return true;
}

}  // namespace

bool Parser::eqi(const char* s, size_t n, const char* lit) {
    size_t l = std::strlen(lit);
    if (n != l) return false;
    for (size_t k = 0; k < n; k++)
        if (std::tolower((unsigned char)s[k]) != lit[k]) return false;
    return true;
}

std::string Parser::tagLower(const char* s, size_t n) {
    std::string r;
    r.reserve(n);
    for (size_t k = 0; k < n; k++)
        r += (char)std::tolower((unsigned char)s[k]);
    return r;
}

void Parser::pushChar(unsigned char c) {
    if (inTitle_) {
        if (page_.title.size() < 200) page_.title += (char)c;
        return;
    }
    if (inTable_) {
        if (inCell_) cell_ += (char)c;
        return;  // table inter-cell text is dropped
    }
    if (ctx_.kind != Ctx::None) ctx_.text += (char)c;
    else text_ += (char)c;
}

void Parser::emitTable() {
    inTable_ = false;
    inCell_ = false;
    cell_.clear();
    tableRow_.clear();
    if (tableRows_.empty()) return;
    Block blk;
    blk.kind = BlockKind::Table;
    for (auto& r : tableRows_) {
        TableRow tr;
        if (r.empty()) continue;
        if (r.size() == 1) {
            tr.v = trimCollapse(r[0]);
        } else {
            tr.k = trimCollapse(r[0]);
            std::string v;
            for (size_t i = 1; i < r.size(); i++) {
                if (i > 1) v += " ";
                v += r[i];
            }
            tr.v = trimCollapse(v);
        }
        if (!tr.k.empty() || !tr.v.empty()) blk.rows.push_back(std::move(tr));
    }
    tableRows_.clear();
    if (!blk.rows.empty() && page_.blocks.size() < kMaxBlocks) page_.blocks.push_back(std::move(blk));
}

void Parser::entity(const char* src, size_t len, size_t& i) {
    // src[i] == '&'
    if (i + 1 >= len) { pushChar('&'); i++; return; }
    if (src[i + 1] == '#') {
        unsigned code = 0;
        size_t j = i + 2;
        bool hex = (src[j] == 'x' || src[j] == 'X');
        if (hex) j++;
        size_t digits = 0;
        while (j < len && digits < 8 && std::isxdigit((unsigned char)src[j])) {
            char c = src[j];
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else v = c - 'A' + 10;
            code = code * (hex ? 16 : 10) + (unsigned)v;
            j++;
            digits++;
        }
        if (digits == 0) { pushChar('&'); i++; return; }
        if (code >= 0x20) {
            if (ctx_.kind != Ctx::None) appendUtf8(ctx_.text, code);
            else appendUtf8(text_, code);
        }
        i = (j < len && src[j] == ';') ? j + 1 : j;
        return;
    }
    size_t j = i + 1;
    while (j < len && src[j] != ';' && j - i < 16) j++;
    if (j < len && src[j] == ';') {
        const char* r = namedEntity(src + i + 1, j - i - 1);
        if (r) {
            if (ctx_.kind != Ctx::None) ctx_.text.append(r);
            else text_.append(r);
        } else {
            pushChar('&');
        }
        i = j + 1;
        return;
    }
    pushChar('&');
    i++;
}

void Parser::flushText() {
    std::string s = trimCollapse(text_);
    text_.clear();
    if (s.empty()) return;
    Block blk;
    blk.kind = BlockKind::Text;
    blk.text = std::move(s);
    blk.bold = bold_;
    blk.italic = italic_;
    if (page_.blocks.size() < kMaxBlocks) page_.blocks.push_back(std::move(blk));
}

void Parser::openCtx(const Ctx& c) {
    flushText();
    ctx_ = c;
}

void Parser::closeCtx() {
    if (ctx_.kind == Ctx::None) return;
    std::string t = trimCollapse(ctx_.text);
    if (ctx_.kind == Ctx::Select) {
        if (!ctx_.options.empty())
            t = ctx_.options[ctx_.selected < ctx_.options.size() ? ctx_.selected : 0];
    }
    if (!t.empty()) {
        Block blk;
        switch (ctx_.kind) {
            case Ctx::Link: blk.kind = BlockKind::Link; break;
            case Ctx::Button: blk.kind = BlockKind::Button; break;
            case Ctx::Heading: blk.kind = BlockKind::Heading; break;
            case Ctx::ListItem: blk.kind = BlockKind::ListItem; break;
            case Ctx::Select: blk.kind = BlockKind::Select; break;
            default: blk.kind = BlockKind::Text; break;
        }
        blk.text = std::move(t);
        blk.href = ctx_.href;
        blk.formAction = ctx_.href;
        blk.formMethod = ctx_.formMethod;
        blk.formFields = ctx_.formFields;
        blk.level = ctx_.level;
        blk.name = ctx_.name;
        blk.options = ctx_.options;
        blk.selected = ctx_.selected;
        if (page_.blocks.size() < kMaxBlocks) page_.blocks.push_back(std::move(blk));
    }
    ctx_ = Ctx{}; ctx_.kind = Ctx::None;
}

void Parser::feed(const char* data, size_t len) {
    size_t i = 0;
    while (i < len) {
        // ---- skip mode (script/style/template content) ----
        if (skipDepth_ > 0) {
            if (data[i] == '<' && i + 1 < len && data[i + 1] == '/') {
                size_t k = i + 2;
                while (k < len && (std::isalnum((unsigned char)data[k]) || data[k] == '-')) k++;
                if (eqi(data + i + 2, k - (i + 2), skipTag_.c_str())) {
                    skipDepth_--;
                    i = k;
                    while (i < len && data[i] != '>') i++;
                    if (i < len) i++;
                    continue;
                }
            }
            i++;
            continue;
        }
        // ---- comment mode ----
        if (inComment_) {
            if (i + 3 <= len && data[i] == '-' && data[i + 1] == '-' && data[i + 2] == '>') {
                i += 3;
                inComment_ = false;
                continue;
            }
            i++;
            continue;
        }
        char c = data[i];
        if (c == '&') {
            // locate end of entity within this chunk; if it runs past the
            // chunk end, emit '&' now and continue (entity may re-encode on
            // next chunk — acceptable, entities rarely split at 64 KB)
            size_t j = i + 1;
            if (j < len && data[j] == '#') {
                j++;
                if (j < len && (data[j] == 'x' || data[j] == 'X')) j++;
                int d = 0;
                while (j < len && d++ < 8 && std::isxdigit((unsigned char)data[j])) j++;
                if (j >= len) { pushChar('&'); i++; continue; }
            } else {
                while (j < len && data[j] != ';' && j - i < 16) j++;
                if (j >= len) { pushChar('&'); i++; continue; }
            }
            entity(data, len, i);
            continue;
        }
        if (c == '<') {
            if (i + 4 <= len && eqi(data + i + 1, 3, "!--")) {
                i += 4;
                inComment_ = true;
                continue;
            }
            // doctype / declarations: skip to '>'
            if (i + 1 < len && data[i + 1] == '!') {
                size_t k2 = i + 1;
                while (k2 < len && data[k2] != '>') k2++;
                i = (k2 < len) ? k2 + 1 : len;
                continue;
            }
            bool closing = (i + 1 < len && data[i + 1] == '/');
            size_t j = i + 1 + (closing ? 1 : 0);
            while (j < len && (std::isalnum((unsigned char)data[j]) || data[j] == '-')) j++;
            if (j == i + 1 + (closing ? 1 : 0)) { i++; continue; }  // '<' in text
            std::string name = tagLower(data + i + 1 + (closing ? 1 : 0),
                                        j - (i + 1 + (closing ? 1 : 0)));

            // scan tag body honoring quoted attrs
            size_t k = j;
            bool quoted = false;
            char quote = 0;
            while (k < len) {
                char t = data[k];
                if (quoted) {
                    if (t == quote) quoted = false;
                } else if (t == '"' || t == '\'') {
                    quoted = true;
                    quote = t;
                } else if (t == '>') break;
                k++;
            }
            if (k >= len) { pushChar('<'); i++; continue; }  // tag spans chunk
            std::string body(data + j, k - j);
            i = k + 1;

            if (!closing) {
                // NOTE: <noscript> is NOT skipped — we are a no-JS client, so
                // its content is exactly the fallback we should render.
                if (name == "script" || name == "style" || name == "template") {
                    skipDepth_++;
                    skipTag_ = name;
                    continue;
                }
                if (name == "title" && !titleDone_) {
                    inTitle_ = true;
                    page_.title.clear();
                    continue;
                }
                if (name == "meta") {
                    if (attrValue(body, "name") == "theme-color") {
                        unsigned v = 0;
                        if (hexColor(attrValue(body, "content"), v)) page_.bg = v & 0xFFFFFFu;
                    }
                    continue;
                }
                if (name == "body") {
                    unsigned v = 0;
                    if (hexColor(attrValue(body, "bgcolor"), v)) page_.bg = v & 0xFFFFFFu;
                    flushText();
                    continue;
                }
                if (name == "a") {
                    Ctx cx;
                    cx.kind = Ctx::Link;
                    cx.href = unescapeAttr(attrValue(body, "href"));
                    openCtx(cx);
                    continue;
                }
                if (name == "button") {
                    Ctx cx;
                    cx.kind = Ctx::Button;
                    cx.href = formAction_;
                    cx.formMethod = formMethod_;
                    cx.formFields = formHidden_;
                    openCtx(cx);
                    continue;
                }
                if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
                    Ctx cx;
                    cx.kind = Ctx::Heading;
                    cx.level = name[1] - '0';
                    openCtx(cx);
                    continue;
                }
                if (name == "li") {
                    Ctx cx;
                    cx.kind = Ctx::ListItem;
                    openCtx(cx);
                    continue;
                }
                if (name == "input") {
                    std::string type = attrValue(body, "type");
                    for (auto& ch : type) ch = (char)tolower((unsigned char)ch);
                    if (type.empty()) type = "text";
                    if (type == "hidden") {
                        // keep for form submission (POST bodies, consent walls)
                        std::string hn = unescapeAttr(attrValue(body, "name"));
                        if (!hn.empty())
                            formHidden_.emplace_back(hn, unescapeAttr(attrValue(body, "value")));
                        continue;
                    }
                    if (type == "file" || type == "reset") continue;
                    if (type == "submit" || type == "button") {
                        flushText();
                        Block blk;
                        blk.kind = BlockKind::Button;
                        std::string val = attrValue(body, "value");
                        blk.text = val.empty() ? (type == "submit" ? "Submit" : "Button") : val;
                        blk.name = attrValue(body, "name");
                        blk.href = formAction_;
                        blk.formAction = formAction_;
                        blk.formMethod = formMethod_;
                        blk.formFields = formHidden_;
                        if (page_.blocks.size() < kMaxBlocks) page_.blocks.push_back(std::move(blk));
                        continue;
                    }
                    flushText();
                    Block blk;
                    blk.kind = BlockKind::Input;
                    blk.name = attrValue(body, "name");
                    blk.placeholder = attrValue(body, "placeholder");
                    if (blk.placeholder.empty()) blk.placeholder = blk.name;
                    blk.formAction = formAction_;
                    blk.formMethod = formMethod_;
                    blk.formFields = formHidden_;
                    if (page_.blocks.size() < kMaxBlocks) page_.blocks.push_back(std::move(blk));
                    continue;
                }
                if (name == "form") {
                    formAction_ = unescapeAttr(attrValue(body, "action"));
                    std::string m = attrValue(body, "method");
                    for (auto& c : m) c = (char)toupper((unsigned char)c);
                    formMethod_ = (m == "POST") ? "POST" : "GET";
                    continue;
                }
                if (name == "table") {
                    if (ctx_.kind == Ctx::Select) closeCtx();
                    flushText();
                    inTable_ = true;
                    tableRows_.clear();
                    continue;
                }
                if (inTable_) {
                    if (name == "tr") { tableRow_.clear(); continue; }
                    if (name == "td" || name == "th") { cell_.clear(); inCell_ = true; continue; }
                    continue;  // thead/tbody/etc: transparent
                }
                if (name == "select") {
                    Ctx cx;
                    cx.kind = Ctx::Select;
                    cx.name = attrValue(body, "name");
                    openCtx(cx);
                    continue;
                }
                if (name == "option" && ctx_.kind == Ctx::Select) {
                    ctx_.text.clear();  // restart per-option text accumulation
                    std::string v = attrValue(body, "value");
                    bool sel = hasAttr(body, "selected");
                    if (!v.empty()) {
                        ctx_.options.push_back(v);
                        if (sel) ctx_.selected = ctx_.options.size() - 1;
                    } else {
                        ctx_.optionPending = true;
                        if (sel) ctx_.selected = ctx_.options.size();
                    }
                    continue;
                }
                if (name == "b" || name == "strong") { bold_ = true; continue; }
                if (name == "i" || name == "em") { italic_ = true; continue; }
                if (name == "br" || name == "p" || name == "div" || name == "section" ||
                    name == "article" || name == "header" || name == "footer" ||
                    name == "main" || name == "nav" || name == "table" || name == "tr" ||
                    name == "ul" || name == "ol" || name == "pre") {
                    flushText();
                    continue;
                }
                continue;  // other elements: transparent
            }

            // closing tags
            if (name == "td" || name == "th") {
                if (inTable_ && inCell_) {
                    inCell_ = false;
                    std::string c = trimCollapse(cell_);
                    if (!c.empty()) tableRow_.push_back(std::move(c));
                }
                continue;
            }
            if (name == "tr" && inTable_) {
                if (!tableRow_.empty()) tableRows_.push_back(std::move(tableRow_));
                tableRow_.clear();
                continue;
            }
            if (name == "table") { emitTable(); continue; }
            if (name == "option" && ctx_.kind == Ctx::Select) {
                if (ctx_.optionPending) {
                    std::string t = trimCollapse(ctx_.text);
                    if (!t.empty()) {
                        ctx_.options.push_back(t);
                        if (ctx_.selected >= ctx_.options.size())
                            ctx_.selected = ctx_.options.size() - 1;
                    }
                    ctx_.optionPending = false;
                    ctx_.text.clear();
                }
                continue;
            }
            if (name == "a" || name == "button" || name == "li" ||
                (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6')) {
                closeCtx();
                continue;
            }
            if (name == "title") { inTitle_ = false; titleDone_ = true; continue; }
            if (name == "form") { formAction_ = ""; formMethod_ = "GET"; formHidden_.clear(); continue; }
            if (name == "b" || name == "strong") { bold_ = false; continue; }
            if (name == "i" || name == "em") { italic_ = false; continue; }
            if (name == "p" || name == "div" || name == "section" || name == "article" ||
                name == "header" || name == "footer" || name == "main" || name == "nav" ||
                name == "table" || name == "ul" || name == "ol" || name == "pre") {
                flushText();
                continue;
            }
            continue;
        }
        if (c == '\n' || c == '\t' || c == '\r') { i++; continue; }
        if (c == ' ') {
            // collapse: only push a space if both sides are non-space
            std::string& cur = inCell_ ? cell_ : (ctx_.kind != Ctx::None ? ctx_.text : text_);
            bool prev = !cur.empty() && !std::isspace((unsigned char)cur.back());
            if (prev) {
                size_t j = i + 1;
                while (j < len && data[j] == ' ') j++;
                if (j < len && data[j] != ' ' && data[j] != '\n' && data[j] != '\r' &&
                    data[j] != '\t')
                    pushChar(' ');
            }
            i++;
            continue;
        }
        pushChar((unsigned char)c);
        i++;
    }
}

void Parser::finish() {
    if (inTitle_) inTitle_ = false;
    if (inTable_) emitTable();
    if (ctx_.kind != Ctx::None) closeCtx();
    flushText();
}

}  // namespace html
}  // namespace lilka
