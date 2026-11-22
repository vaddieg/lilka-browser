#include "charset.h"
#include <cctype>

namespace lilka {
namespace charset {

namespace {

// ---- cp1251 (windows-1251) byte -> Unicode codepoint ----
// Bytes 0x00-0x7F are ASCII. 0x80-0x9F are C1-ish punctuation, 0xA0-0xFF
// hold the Cyrillic block.
const unsigned int kCp1251[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
};

const unsigned int kKoi8r[128] = {
    0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524,
    0x252C, 0x2534, 0x253C, 0x2580, 0x2584, 0x2588, 0x258C, 0x2590,
    0x2591, 0x2592, 0x2593, 0x2320, 0x25A0, 0x2219, 0x221A, 0x2248,
    0x2264, 0x2265, 0x00A0, 0x2321, 0x00B0, 0x00B2, 0x00B7, 0x00F7,
    0x2550, 0x2551, 0x2552, 0x0451, 0x2553, 0x2554, 0x2555, 0x2556,
    0x2557, 0x2558, 0x2559, 0x255A, 0x255B, 0x255C, 0x255D, 0x255E,
    0x255F, 0x2560, 0x2561, 0x0401, 0x2562, 0x2563, 0x2564, 0x2565,
    0x2566, 0x2567, 0x2568, 0x2569, 0x256A, 0x256B, 0x256C, 0x00A9,
    0x044E, 0x0430, 0x0431, 0x0446, 0x0434, 0x0435, 0x0444, 0x0433,
    0x0445, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E,
    0x043F, 0x044F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0436, 0x0432,
    0x044C, 0x044B, 0x0437, 0x0448, 0x044D, 0x0449, 0x0447, 0x044A,
    0x042E, 0x0410, 0x0411, 0x0426, 0x0414, 0x0415, 0x0424, 0x0413,
    0x0425, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E,
    0x041F, 0x042F, 0x0420, 0x0421, 0x0422, 0x0423, 0x0416, 0x0412,
    0x042C, 0x042B, 0x0417, 0x0428, 0x042D, 0x0429, 0x0427, 0x042A,
};

void appendCp(std::string& out, unsigned int cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 63));
    } else {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 63));
        out += (char)(0x80 | (cp & 63));
    }
}

bool validUtf8(const char* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)p[i];
        int extra;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;
        for (int k = 1; k <= extra; k++) {
            if (i + k >= n) return false;
            if (((unsigned char)p[i + k] & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

}  // namespace

std::string canonical(const std::string& in) {
    std::string s;
    for (char ch : in) {
        if (ch == '-' || ch == '_' || ch == '.') continue;
        s += (char)tolower((unsigned char)ch);
    }
    if (s == "utf8" || s == "utf-8" || s == "unicode61-0" || s == "ucs2" ||
        s == "utf16")
        return "utf8";
    if (s == "cp1251" || s == "windows1251" || s == "win1251" ||
        s == "ansi1251" || s == "1251" || s == "ms1251" || s == "xcp1251")
        return "cp1251";
    if (s == "koi8r" || s == "koi8ru" || s == "koi8")
        return "koi8r";
    return "";
}

void stripBom(const std::string& src, std::string& out) {
    if (src.size() >= 3 && (unsigned char)src[0] == 0xEF &&
        (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) {
        out = src.substr(3);
    } else {
        out = src;
    }
}

void toUtf8(const std::string& src, const char* enc, std::string& out) {
    out.clear();
    std::string id = canonical(enc ? enc : "");
    if (id.empty() || id == "utf8") {
        out = src;
        return;
    }
    out.reserve(src.size() * 2);
    const unsigned int* tbl = nullptr;
    if (id == "cp1251") tbl = kCp1251;
    else if (id == "koi8r") tbl = kKoi8r;
    if (!tbl) { out = src; return; }
    for (unsigned char b : src) {
        if (b < 0x80) { out += (char)b; continue; }
        unsigned int cp = tbl[b - 0x80];
        if (cp == 0) cp = 0x00A0;  // undefined slot -> NBSP, keeps layout
        appendCp(out, cp);
    }
}

std::string detect(const std::string& html, const std::string& httpCharset) {
    std::string http = canonical(httpCharset);
    if (!http.empty()) return http;

    // sniff a meta charset declaration in the first ~2 KB
    size_t scan = html.size() < 2048 ? html.size() : 2048;
    std::string head;
    for (size_t i = 0; i < scan; i++)
        head += (char)tolower((unsigned char)html[i]);

    // <meta charset="X">  or  <meta charset=X>
    size_t m = head.find("charset=");
    if (m != std::string::npos) {
        size_t i = m + 8;
        if (i < head.size() && (head[i] == '"' || head[i] == '\'')) i++;
        size_t s = i;
        while (i < head.size() && head[i] != '"' && head[i] != '\'' &&
               head[i] != ';' && head[i] != ' ' && head[i] != '>')
            i++;
        std::string c = canonical(head.substr(s, i - s));
        if (!c.empty()) return c;
    }

    // default: trust UTF-8 unless the bytes clearly aren't valid UTF-8 and
    // look like legacy Cyrillic (high bytes in the 0xC0-0xFF range).
    if (validUtf8(html.data(), html.size())) return "utf8";
    bool high = false;
    for (size_t i = 0; i < scan; i++)
        if ((unsigned char)html[i] >= 0xC0) { high = true; break; }
    return high ? "cp1251" : "utf8";
}

}  // namespace charset
}  // namespace lilka
