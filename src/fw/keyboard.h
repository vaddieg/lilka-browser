// On-screen keyboard (adapted from lilka-sdk inputdialog.cpp).
// 3 languages (en/ru/uk), 3 layers (base/shift/symbols), 4 rows x 10 cols.
// Controls: A = type, B = backspace, C = language, D = layer,
//           arrows = move, START = done.
#pragma once
#include "hw.h"
#include <Arduino_GFX_Library.h>
#include <string>

namespace kb {

// special key codes
constexpr int16_t K_L0 = -1, K_L1 = -2, K_L2 = -3, K_BS = -10, K_SP = -11, K_LANG = -12;

constexpr int ROWS = 4, COLS = 10;

class Keyboard {
public:
    void begin() { reset(); }
    void reset() {
        value_.clear();
        cx = 1; cy = 1;
        layer = 0; lang = 0;
        lastBlink = millis(); blink = true;
        done = false;
    }
    void setValue(const std::string& v) { value_ = v; }
    const std::string& value() const { return value_; }
    bool isFinished();

    /// Returns true if anything changed (cursor, text, layer, blink) and
    /// the caller should redraw.
    bool update(Buttons& btns);
    void draw(Arduino_GFX* gfx, const std::string& title) const;

private:
    int16_t keyAt(int x, int y) const;
    void move(int dx, int dy);
    void backspace();

    std::string value_;
    int cx = 0, cy = 0;
    int layer = 0, lang = 0;
    bool done = false;
    uint32_t lastBlink = 0;
    bool blink = true;
};

}  // namespace kb
