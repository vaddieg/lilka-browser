// Streaming HTML -> IR parser (see ir.h).
// Deliberately small: no DOM, no JS/CSS, tolerant of malformed markup.
// Platform independent; tested on macOS and used on ESP32-S3.
#pragma once
#include "ir.h"
#include <cctype>
#include <cstring>
#include <map>
#include <string>

namespace lilka {
namespace html {

class Parser {
public:
    static constexpr size_t kMaxBlocks = 250;  // cap: memory + menu usability (13px: 6 rows/screen)
    // Feed chunks of HTML; call finish() then page().
    void feed(const char* data, size_t len);
    void finish();
    const Page& page() const { return page_; }

private:
    struct Ctx {           // open element state (a/button/h*/li/select/label)
        enum { None, Link, Button, Heading, ListItem, Select, Label } kind;
        std::string href;
        std::string formMethod;
        std::vector<std::pair<std::string, std::string>> formFields;
        std::string name;  // Select: form field name
        int level = 0;
        std::string text;  // accumulated inner text
        std::vector<std::string> options; // Select
        size_t selected = 0;
        bool optionPending = false;       // <option> without value= : use its text
    };

    void flushText();      // emit pending inline text as a block
    void openCtx(const Ctx& c);
    void closeCtx();
    void emitTable();
    void entity(const char* src, size_t len, size_t& i);
    void pushChar(unsigned char c);
    static bool eqi(const char* s, size_t n, const char* lit);
    static std::string tagLower(const char* s, size_t n);

    Page page_;
    std::string text_;          // pending inline text
    bool bold_ = false, italic_ = false;
    int skipDepth_ = 0;         // inside <script>/<style>/<head>-like content
    std::string skipTag_;       // which closing tag to wait for
    Ctx ctx_ = Ctx{};
    bool inTitle_ = false;
    bool inComment_ = false;
    bool titleDone_ = false;  // only the first <title> counts
    std::string formAction_;
    std::string formMethod_ = "GET";
    std::vector<std::pair<std::string, std::string>> formHidden_;  // <input type=hidden> inside current form
    std::vector<size_t> formChecks_;  // block indexes of checkboxes in current form
    std::map<std::string, std::string> labelFor_;  // <label for=id> -> text
    // table state
    bool inTable_ = false;
    std::vector<std::vector<std::string>> tableRows_;
    std::vector<std::string> tableRow_;
    std::string cell_;
    bool inCell_ = false;    // current <form action>, for buttons/inputs
};

}  // namespace html
}  // namespace lilka

#include "html_parser.ipp"
