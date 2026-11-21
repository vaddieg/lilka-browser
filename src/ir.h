// IR (intermediate representation) of a parsed web page.
// A flat, vertically-ordered list of blocks. Platform independent.
#pragma once
#include <string>
#include <vector>

namespace lilka {

enum class BlockKind {
    Text,     // plain paragraph/text run (flags: bold/italic)
    Heading,  // h1-h6, level in `level` (1..6)
    Link,     // clickable, href set
    Button,   // form button/submit, href optional (form action)
    Input,    // text/password input; name/placeholder set
    ListItem, // list item (rendered with a small bullet)
    Table,    // table rows in `rows` (2-column key/value preferred)
    Select,   // dropdown: options in `options`, current index `selected`
};

struct TableRow {
    std::string k;
    std::string v;
};

struct Block {
    BlockKind kind = BlockKind::Text;
    std::string text;
    std::string href;        // Link/Button
    std::string name;        // Input
    std::string placeholder; // Input
    std::string formAction;  // Button/Input submit target
    std::string formMethod;  // "GET" or "POST"
    int level = 0;           // Heading: 1..6
    bool bold = false;
    bool italic = false;
    std::vector<TableRow> rows;   // Table
    std::vector<std::string> options; // Select
    size_t selected = 0;           // Select: current option index
};

// Parsed page: blocks + optional background color (nice-to-have).
struct Page {
    std::vector<Block> blocks;
    unsigned bg = 0xFFFFFFu; // from <meta name="theme-color"> or body bgcolor, if trivial
    std::string title;       // <title>
};

}  // namespace lilka
