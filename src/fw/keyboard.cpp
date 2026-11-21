#include "keyboard.h"
#include "font.h"
#include <string.h>

namespace kb {

// [lang][layer][ROWS*COLS]
static const int16_t keys[3][3][ROWS * COLS] = {
    // ---------------- English ----------------
    {
        { // base
            '1','2','3','4','5','6','7','8','9','0',
            'q','w','e','r','t','y','u','i','o','p',
            'a','s','d','f','g','h','j','k','l',K_BS,
            K_L0,'z','x','c','v','b','n','m','.',K_SP,
        },
        { // shift
            '!','@','#','$','%','^','&','*','(',')',
            'Q','W','E','R','T','Y','U','I','O','P',
            'A','S','D','F','G','H','J','K','L',K_BS,
            K_L0,'Z','X','C','V','B','N','M','?',K_SP,
        },
        { // symbols
            '-','_','=','+','<','>','/',0x27,0x22,'\\',
            '{','}','[',']','~',';',':',',','!','.',
            K_BS,0,0,0,0,0,0,0,0,K_BS,
            K_L0,0,0,0,0,0,0,'@',0,K_SP,
        },
    },
    { // ---------------- Russian (ЙЦУКЕН) ----------------
        { // base
            '1','2','3','4','5','6','7','8','9','0',
            0x0439,0x0446,0x0443,0x043A,0x0435,0x043D,0x0433,0x0448,0x0449,0x0437, // йцукенгшщз
            0x0444,0x0456,0x0432,0x0430,0x043F,0x0440,0x043E,0x043B,0x0434,K_BS,   // фывапролд
            K_L0,0x044F,0x0447,0x0441,0x043C,0x0438,0x0442,0x044C,0x0431,K_SP,    // ячсмитьбю
        },
        { // shift
            '1','2','3','4','5','6','7','8','9','0',
            0x0419,0x0426,0x0423,0x041A,0x0415,0x041D,0x0413,0x0428,0x0429,0x0417, // ЙЦУКЕНГШЩЗ
            0x0424,0x042B,0x0412,0x0410,0x041F,0x0420,0x041E,0x041B,0x0414,K_BS,   // ФЫВАПРОЛД
            K_L0,0x042F,0x0427,0x0421,0x041C,0x0422,0x041C,0x0411,0x042E,K_SP,    // ЯЧСМИТЬБЮ
        },
        { // symbols: same as English symbols
            '-','_','=','+','<','>','/',0x27,0x22,'\\',
            '{','}','[',']','~',';',':',',','!','.',
            K_BS,0,0,0,0,0,0,0,0,K_BS,
            K_L0,0,0,0,0,0,0,'@',0,K_SP,
        },
    },
    { // ---------------- Ukrainian (ЙЦУКЕН: і, ї, є, ґ) ----------------
        { // base
            '1','2','3','4','5','6','7','8','9','0',
            0x0439,0x0446,0x0443,0x043A,0x0435,0x043D,0x0433,0x0448,0x0449,0x0437, // йцукенгшщз
            0x0444,0x0457,0x0432,0x0430,0x043F,0x0440,0x043E,0x043B,0x0434,K_BS,   // фівапролд
            K_L0,0x0454,0x0447,0x0441,0x043C,0x0456,0x0442,0x044C,0x0431,K_SP,    // єчсмитьбю
        },
        { // shift
            '1','2','3','4','5','6','7','8','9','0',
            0x0419,0x0426,0x0423,0x041A,0x0415,0x041D,0x0413,0x0428,0x0429,0x0417, // ЙЦУКЕНГШЩЗ
            0x0424,0x0406,0x0412,0x0410,0x041F,0x0420,0x041E,0x041B,0x0414,K_BS,   // ФІВАПРОЛД
            K_L0,0x0404,0x0427,0x0421,0x041C,0x0418,0x0422,0x0407,0x0411,K_SP,    // ЄЧСМИТЇБЮ
        },
        { // symbols: same as English symbols
            '-','_','=','+','<','>','/',0x27,0x22,'\\',
            '{','}','[',']','~',';',':',',','!','.',
            K_BS,0,0,0,0,0,0,0,0,K_BS,
            K_L0,0,0,0,0,0,0,'@',0,K_SP,
        },
    },
};

int16_t Keyboard::keyAt(int x, int y) const {
    return keys[lang][layer][y * COLS + x];
}

bool Keyboard::isFinished() {
    bool d = done;
    done = false;
    return d;
}

void Keyboard::backspace() {
    if (value_.empty()) return;
    size_t i = value_.size();
    while (i > 0 && (value_[i] & 0xC0) == 0x80) i--;
    i--;  // the codepoint start
    value_.erase(i);
}

void Keyboard::move(int dx, int dy) {
    for (int step = 0; step < ROWS * COLS; step++) {
        cx += dx; cy += dy;
        if (cx < 0) cx = COLS - 1;
        if (cx >= COLS) cx = 0;
        if (cy < 0) cy = ROWS - 1;
        if (cy >= ROWS) cy = 0;
        int16_t k = keyAt(cx, cy);
        if (k) return;
    }
}

void Keyboard::update(Buttons& btns) {
    if (millis() - lastBlink > 300) { lastBlink = millis(); blink = !blink; }
    if (btns.get(Buttons::A).justPressed) {
        int16_t k = keyAt(cx, cy);
        if (k == K_L0) layer = 0;
        else if (k == K_L1) layer = 1;
        else if (k == K_L2) layer = 2;
        else if (k == K_BS) backspace();
        else if (k == K_LANG) { lang = (lang + 1) % 3; layer = 0; }
        else if (k == K_SP) value_ += ' ';
        else if (k >= 0x80 && k < 0x800) {
            char b[3] = {(char)(0xC0 | (k >> 6)), (char)(0x80 | (k & 63)), 0};
            value_ += b;
        } else if (k >= 0x800) {
            char b[4] = {(char)(0xE0 | (k >> 12)), (char)(0x80 | ((k >> 6) & 63)),
                         (char)(0x80 | (k & 63)), 0};
            value_ += b;
        } else if (k) value_ += (char)k;
        lastBlink = millis(); blink = true;
    } else if (btns.get(Buttons::B).justPressed) {
        backspace();
    } else if (btns.get(Buttons::C).justPressed) {
        lang = (lang + 1) % 3;
        layer = 0;
    } else if (btns.get(Buttons::D).justPressed) {
        layer = (layer + 1) % 3;
    } else if (btns.get(Buttons::START).justPressed) {
        done = true;
    } else if (btns.get(Buttons::UP).justPressed) move(0, -1);
    else if (btns.get(Buttons::DOWN).justPressed) move(0, 1);
    else if (btns.get(Buttons::LEFT).justPressed) move(-1, 0);
    else if (btns.get(Buttons::RIGHT).justPressed) move(1, 0);
}

void Keyboard::draw(Arduino_GFX* gfx, const std::string& title) const {
    int16_t W = gfx->width(), H = gfx->height();
    gfx->fillScreen(0x0000);

    // header
    font::text(gfx, 8, 4, title.c_str(), 0xFFFF, 1);

    // value line (last chars that fit)
    std::string v = value_;
    const char* show = v.c_str();
    int16_t maxW = W - 16;
    // shrink from the front until it fits
    while (*show && font::width(show, 1) > maxW) {
        size_t n = font::charLen((const unsigned char*)show);
        show += n;
    }
    if (v.c_str() != show) {
        static char buf[80];
        snprintf(buf, sizeof(buf), "%s%s", "\xE2\x80\xA6", show);
        show = buf;
    }
    font::text(gfx, 8, 22, show, 0xFFFF, 1);
    if (blink) {
        int16_t w = font::width(show, 1);
        gfx->fillRect(8 + w + 2, 22, 2, 13, 0xFFFF);
    }

    // key grid
    int16_t top = 48;
    int16_t bw = W / COLS;
    int16_t bh = (H - top - 4) / ROWS;
    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            int16_t k = keyAt(x, y);
            if (!k) continue;
            int16_t px = x * bw, py = top + y * bh;
            bool sel = (x == cx && y == cy);
            if (sel) gfx->fillRect(px, py, bw, bh, 0x7E6E);  // orange-red-ish
            const char* cap = nullptr;
            char tmp[4] = {0};
            if (k == K_L0) cap = "ab";
            else if (k == K_LANG) cap = lang == 0 ? "en" : (lang == 1 ? "ru" : "uk");
            else if (k == K_BS) cap = "<";
            else if (k == K_SP) cap = "sp";
            else if (k < 0x80) { tmp[0] = (char)k; cap = tmp; }
            else if (k < 0x800) {
                tmp[0] = (char)(0xC0 | (k >> 6)); tmp[1] = (char)(0x80 | (k & 63)); cap = tmp;
            } else {
                tmp[0] = (char)(0xE0 | (k >> 12));
                tmp[1] = (char)(0x80 | ((k >> 6) & 63));
                tmp[2] = (char)(0x80 | (k & 63)); cap = tmp;
            }
            if (cap) {
                int16_t w = font::width(cap, 1);
                if (w > bw - 2) continue;
                font::text(gfx, px + (bw - w) / 2, py + (bh - 13) / 2, cap, 0xFFFF, 1);
            }
        }
    }
    // hint line
    font::text(gfx, 4, H - 12, "A=type C=lang D=layer B=close", 0x7FFF7F, 1);
}

}  // namespace kb
