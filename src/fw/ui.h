// Browser UI: page view, link list mode, popups (settings / quick start),
// on-screen input. See TASK.md for the control scheme.
#pragma once
#include "hw.h"
#include "ir.h"
#include "keyboard.h"
#include <Arduino_GFX_Library.h>
#include <string>
#include <vector>

namespace ui {

struct Line {
    int block = -1;     // source block index (-1 for pseudo lines)
    std::string text;
    bool pseudo = false; // ellipsis / table / select line
    bool field = false;  // input field: draw rectangle outline around caption
    bool btn = false;    // button: draw button box
};

class Browser {
public:
    void setButtons(Buttons* b);
    bool begin(Arduino_GFX* gfx);
    /// Poll buttons, advance state, redraw on change.
    /// Returns true if a frame was drawn (caller must flush the canvas).
    bool loop();
    void serialCmd(const std::string& line);  // debug/autotest: "goto <url>", "back", "quickstart"

private:
    // state
    enum Mode { PAGE, LINKLIST, SETTINGS, QUICKSTART, INPUTMODE, BUSY, ERROR };
    Mode mode_ = PAGE;
    Mode inputFrom_ = PAGE;  // where to return when the keyboard is cancelled

    lilka::Page page_;
    std::string lastHtml_;  // for the `dump` serial command
    std::vector<Line> lines_;
    std::vector<std::pair<std::string, std::string>> links_; // linklist mode: label,url

    int lineCursor_ = 0;
    int sel_ = -1;            // selected clickable line (into lines_)
    int linklistSel_ = 0;

    std::vector<std::string> history_;   // navigation stack
    std::string currentUrl_;
    std::string errText_;

    // input
    kb::Keyboard kb_;
    std::vector<std::string> qsCache_;  // Quick Access list (cached; avoids SD read per key)
    int qsSize() const { return (int)qsCache_.size(); }
    std::vector<std::string> quickList() const;
    std::string inputPurpose_;   // "url" | field name
    std::string inputFormAction_;
    std::string inputFormMethod_;
    std::vector<std::pair<std::string, std::string>> inputFormFields_;
    std::vector<size_t> inputCheckIds_;  // checkbox blocks in the input's form

    // settings
    int fontScale_ = 1;         // 1 = 13px, 2 = 20px, 3 = 40px

    // derived
    int lineH() const;
    int lineH(int lineIdx) const;
    uint8_t lineScale(int lineIdx) const;
    std::vector<std::string> recentList() const;
    bool clickable(int lineIdx) const;
    void buildLines();
    void navigate(const std::string& url);
    std::vector<std::pair<std::string, std::string>> withChecks(const lilka::Block& b) const;
    std::vector<std::pair<std::string, std::string>> withChecksFields(
            const std::vector<std::pair<std::string, std::string>>& base,
            const std::vector<size_t>& ids) const;
    void submitPost(const std::string& url, const std::vector<std::pair<std::string, std::string>>& fields,
                    const std::string& extraName, const std::string& extraValue);
    void finishLoad(const std::string& url, std::string html, const std::string& finalUrl);

    /// net::StageFn target — redraws the busy screen inline (fetch blocks the loop).
    void onStage(const char* s);
    void goBack();
    void activate(int lineIdx);
    void renderPage();
    void renderLinkList();
    void renderPopup(const std::string& title, const std::vector<std::string>& items, int sel);
    void renderError();
    void renderBusy();
    void frameClickables(std::vector<int>& out) const;

    Arduino_GFX* gfx_ = nullptr;
    Buttons* btns_ = nullptr;
    bool dirty_ = true;
    std::string busyStage_;
    std::string busyUrl_;
    uint32_t busyT0_ = 0;
};

}  // namespace ui
