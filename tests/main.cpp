// Mac test target for the HTML parser: unit tests + CLI for the
// autonomous test loop.
//
// Build & test:  make test
// CLI:           ./build/parser-test <fixture.html | http(s)://url>
//                (URLs are fetched with curl; output is the IR dump)
#include "../src/html_parser.h"
#include "../src/charset.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace lilka;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static Page parse(const std::string& html) {
    html::Parser p;
    p.feed(html.data(), html.size());
    p.finish();
    return p.page();
}

static size_t countKind(const Page& pg, BlockKind k) {
    size_t n = 0;
    for (auto& b : pg.blocks) if (b.kind == k) n++;
    return n;
}

// ---------------- unit tests ----------------

void test_basics() {
    Page pg = parse("<html><body>"
                    "<h1>Title</h1>"
                    "<p>Hello <b>world</b> &amp; friends</p>"
                    "<a href=\"https://example.com/x\">Click me</a>"
                    "</body></html>");
    CHECK(countKind(pg, BlockKind::Heading) == 1);
    CHECK(pg.blocks[0].text == "Title");
    CHECK(pg.blocks[0].level == 1);
    CHECK(countKind(pg, BlockKind::Text) == 1);
    CHECK(pg.blocks[1].text == "Hello world & friends");
    CHECK(countKind(pg, BlockKind::Link) == 1);
    CHECK(pg.blocks[2].href == "https://example.com/x");
    CHECK(pg.blocks[2].text == "Click me");
}

void test_headings() {
    Page pg = parse("<h1>Heading 1</h1>"
                    "<h2>Heading 2</h2>"
                    "<h3>Heading 3</h3>"
                    "<h4>Heading 4</h4>"
                    "<h5>Heading 5</h5>"
                    "<h6>Heading 6</h6>");
    CHECK(countKind(pg, BlockKind::Heading) == 6);
    CHECK(pg.blocks[0].level == 1 && pg.blocks[0].text == "Heading 1");
    CHECK(pg.blocks[1].level == 2 && pg.blocks[1].text == "Heading 2");
    CHECK(pg.blocks[2].level == 3 && pg.blocks[2].text == "Heading 3");
    CHECK(pg.blocks[3].level == 4 && pg.blocks[3].text == "Heading 4");
    CHECK(pg.blocks[4].level == 5 && pg.blocks[4].text == "Heading 5");
    CHECK(pg.blocks[5].level == 6 && pg.blocks[5].text == "Heading 6");
}

void test_script_style_stripped() {
    Page pg = parse("<html><head>"
                    "<script>var x = '<div>fake</div> <a href=x>bad</a>';</script>"
                    "<style>.a { color: red; }</style>"
                    "</head><body>Real text</body></html>");
    for (auto& b : pg.blocks) {
        CHECK(b.text.find("fake") == std::string::npos);
        CHECK(b.text.find("color") == std::string::npos);
    }
    CHECK(countKind(pg, BlockKind::Text) >= 1);
}

void test_noscript_rendered() {
    // We are a no-JS client: <noscript> fallback content must be rendered,
    // while <script> content stays stripped.
    Page pg = parse("<body>"
                    "<noscript><a href=\"/fallback\">JS-free link</a> "
                    "<p>noscript text</p></noscript>"
                    "<script>var s = '<a href=\"/bad\">no</a>';</script>"
                    "</body>");
    bool foundLink = false, foundText = false;
    for (auto& b : pg.blocks) {
        if (b.kind == BlockKind::Link && b.href == "/fallback") foundLink = true;
        if (b.text == "noscript text") foundText = true;
        CHECK(b.text.find("var s") == std::string::npos);
    }
    CHECK(foundLink);
    CHECK(foundText);
}

void test_entities() {
    Page pg = parse("<p>a&#65;b &#x42; &mdash; &nbsp; &middot; &unknown; end</p>");
    CHECK(pg.blocks.size() == 1);
    CHECK(pg.blocks[0].text.find("aAb B") != std::string::npos);
    CHECK(pg.blocks[0].text.find("\xC2\xB7") != std::string::npos); // middot
    CHECK(pg.blocks[0].text.find("end") != std::string::npos);
}

void test_cyrillic() {
    Page pg = parse("<h2>\xD0\x9A\xD0\xBE\xD1\x88</h2><p>\xD0\xA5\xD0\xBE\xD1\x80\xD0\xBE\xD1\x88</p>");
    CHECK(pg.blocks[0].text == "\xD0\x9A\xD0\xBE\xD1\x88");
    CHECK(pg.blocks[1].text == "\xD0\xA5\xD0\xBE\xD1\x80\xD0\xBE\xD1\x88");
}

void test_inputs_forms() {
    Page pg = parse("<form action=\"/search\"><input name=\"q\" placeholder=\"Search\">"
                    "<button>Go</button></form>"
                    "<input type=\"hidden\" name=\"x\">");
    CHECK(countKind(pg, BlockKind::Input) == 1);
    CHECK(pg.blocks[0].name == "q");
    CHECK(pg.blocks[0].formAction == "/search");
    CHECK(countKind(pg, BlockKind::Button) == 1);
    CHECK(pg.blocks[1].href == "/search");

    // Submit inputs (like "Accept all" / "Google Search")
    Page pg2 = parse("<form action=\"/consent\"><input type=\"submit\" value=\"Accept all\" name=\"agree\"></form>");
    CHECK(countKind(pg2, BlockKind::Button) == 1);
    CHECK(pg2.blocks[0].text == "Accept all");
    CHECK(pg2.blocks[0].href == "/consent");
}

void test_submit_button_no_text() {
    // DuckDuckGo's submit button has no text, only an aria-label; it must be
    // kept (not dropped) and carry the form action.
    Page pg = parse("<form action=\"/\" method=\"GET\">"
                    "<textarea name=\"q\"></textarea>"
                    "<button type=\"submit\" aria-label=\"Search\"></button></form>");
    CHECK(countKind(pg, BlockKind::Button) == 1);
    CHECK(pg.blocks[pg.blocks.size() - 1].text == "Search");
    CHECK(pg.blocks[pg.blocks.size() - 1].formAction == "/");
}

void test_textarea() {
    // DuckDuckGo's search box is a <textarea>, not an <input>.
    Page pg = parse("<form action=\"/\" method=\"GET\">"
                    "<textarea name=\"q\" placeholder=\"Search privately\"></textarea>"
                    "<button type=\"submit\" aria-label=\"Search\">Search</button></form>");
    CHECK(countKind(pg, BlockKind::Input) == 1);
    CHECK(pg.blocks[0].name == "q");
    CHECK(pg.blocks[0].formAction == "/");
    CHECK(pg.blocks[0].formMethod == "GET");
}

void test_checkboxes() {
    Page pg = parse("<form action=\"/consent\">"
                    "<input type=\"checkbox\" name=\"consent\" value=\"yes\">"
                    "<input type=\"checkbox\" name=\"stats\" checked>"
                    "<input type=\"submit\" value=\"Save\"></form>");
    CHECK(countKind(pg, BlockKind::Checkbox) == 2);
    const auto& c0 = pg.blocks[0];
    CHECK(c0.name == "consent" && c0.value == "yes" && !c0.checked);
    const auto& c1 = pg.blocks[1];
    CHECK(c1.name == "stats" && c1.value == "on" && c1.checked);
    // the submit button knows about both checkboxes (for submission)
    const auto& b = pg.blocks[2];
    CHECK(b.kind == BlockKind::Button);
    CHECK(b.checkIds.size() == 2);
    CHECK(b.checkIds[0] == 0 && b.checkIds[1] == 1);

    // a later form resets the checkbox association
    Page pg2 = parse("<form><input type=\"checkbox\" name=\"a\"></form>"
                     "<form action=\"/x\"><input type=\"submit\" value=\"Go\"></form>");
    CHECK(pg2.blocks[1].kind == BlockKind::Button);
    CHECK(pg2.blocks[1].checkIds.empty());

    // <label for> becomes the caption (label after the input)
    Page pg3 = parse("<form><input type=\"checkbox\" id=\"keep\" name=\"k\">"
                     "<label for=\"keep\">Keep me logged in</label></form>");
    CHECK(pg3.blocks[0].kind == BlockKind::Checkbox);
    CHECK(pg3.blocks[0].text == "Keep me logged in");
    // aria-label also works
    Page pg4 = parse("<input type=\"checkbox\" aria-label=\"Agree to terms\">");
    CHECK(pg4.blocks[0].text == "Agree to terms");
}

void test_disclosure_toggle() {
    // A checkbox/radio with role="button" is a CSS-only disclosure toggle
    // (Wikipedia mobile sidebar) and must be skipped, not shown as a checkbox.
    Page pg = parse("<input type=\"checkbox\" id=\"main-menu-input\" "
                    "class=\"toggle-list__checkbox\" role=\"button\" "
                    "aria-expanded=\"false\">"
                    "<input type=\"checkbox\" name=\"real\" value=\"yes\">");
    CHECK(countKind(pg, BlockKind::Checkbox) == 1);          // only the real one
    CHECK(pg.blocks[0].name == "real");                        // the toggle is gone
    // radio with role=button is also skipped
    Page pg2 = parse("<input type=\"radio\" role=\"BUTTON\" name=\"r\">"
                     "<input type=\"radio\" name=\"r2\">");
    CHECK(countKind(pg2, BlockKind::Checkbox) == 1);
    CHECK(pg2.blocks[0].name == "r2");
}

void test_tables() {
    Page pg = parse("<table>"
                    "<tr><th>Key</th><th>Value</th></tr>"
                    "<tr><td>Player</td><td>Lilka</td></tr>"
                    "<tr><td>RAM</td><td>8 MB</td><td>PSRAM</td></tr>"
                    "</table>");
    CHECK(countKind(pg, BlockKind::Table) == 1);
    const auto& t = pg.blocks[0];
    CHECK(t.rows.size() == 3);
    CHECK(t.rows[1].k == "Player" && t.rows[1].v == "Lilka");
    CHECK(t.rows[2].v.find("8 MB") != std::string::npos);
}

void test_table_links() {
    // markdown-flavored tables with a link in one cell: the row keeps the href
    Page pg = parse("<table>"
                    "<tr><td>ESP32</td><td>Yes</td><td><a href=\"https://ex.com/ds.pdf\">ESP32</a></td></tr>"
                    "<tr><td>ESP32-C3</td><td>Yes</td><td>plain</td></tr>"
                    "</table>");
    CHECK(countKind(pg, BlockKind::Table) == 1);
    const auto& t = pg.blocks[0];
    CHECK(t.rows.size() == 2);
    CHECK(t.rows[0].href == "https://ex.com/ds.pdf");
    CHECK(t.rows[1].href.empty());
}

void test_select() {
    // design: options carry submit *values* (option value attr; falls back to text)
    Page pg = parse("<form><select name=\"sort\">"
                    "<option value=\"new\">New</option>"
                    "<option value=\"best\" selected>Best</option>"
                    "<option>Text</option>"
                    "</select></form>");
    CHECK(countKind(pg, BlockKind::Select) == 1);
    const auto& s = pg.blocks[0];
    CHECK(s.options.size() == 3);
    CHECK(s.options[0] == "new" && s.options[2] == "Text");
    CHECK(s.selected == 1);
    CHECK(s.text == "best");
    CHECK(s.name == "sort");
}

void test_images() {
    Page pg = parse("<p>before</p>"
                    "<img src=\"/img/cat.jpg\" alt=\"A cat\" width=\"480\" height=\"360\">"
                    "<p>after</p>");
    // image is a block, in document order (between the two paragraphs)
    CHECK(pg.blocks.size() == 3);
    CHECK(pg.blocks[1].kind == BlockKind::Image);
    CHECK(pg.blocks[1].href == "/img/cat.jpg");
    CHECK(pg.blocks[1].text == "A cat");
    CHECK(pg.blocks[1].width == 480 && pg.blocks[1].height == 360);

    // entity-encoded src is decoded; no src -> no block
    Page pg2 = parse("<img src=\"/a&amp;b.png\">");
    CHECK(countKind(pg2, BlockKind::Image) == 1);
    CHECK(pg2.blocks[0].href == "/a&b.png");
    Page pg3 = parse("<img alt=\"no source\">");
    CHECK(countKind(pg3, BlockKind::Image) == 0);
}

void test_charset() {
    using namespace lilka::charset;
    // canonicalization
    CHECK(canonical("windows-1251") == "cp1251");
    CHECK(canonical("CP1251") == "cp1251");
    CHECK(canonical("koi8-r") == "koi8r");
    CHECK(canonical("utf-8") == "utf8");
    CHECK(canonical("big5").empty());

    // cp1251 -> UTF-8: "Прив" in cp1251 = CF F0 E8 E2
    std::string src;
    src += (char)0xCF; src += (char)0xF0; src += (char)0xE8; src += (char)0xE2;
    std::string out;
    toUtf8(src, "cp1251", out);
    // expected UTF-8: П U+0410 -> D0 90 ; р U+0440 -> D1 80
    //                и U+0438 -> D0 B8 ; в U+0432 -> D0 B2
    std::string expect;
    expect += (char)0xD0; expect += (char)0x9F;
    expect += (char)0xD1; expect += (char)0x80;
    expect += (char)0xD0; expect += (char)0xB8;
    expect += (char)0xD0; expect += (char)0xB2;
    CHECK(out == expect);

    // KOI8-R: "Прив" -> П=0xF0 р=0xD2 и=0xC9 в=0xD7
    std::string koi;
    koi += (char)0xF0; koi += (char)0xD2; koi += (char)0xC9; koi += (char)0xD7;
    toUtf8(koi, "koi8-r", out);
    CHECK(out == expect);

    // detection: http charset wins
    CHECK(detect("<html>x</html>", "windows-1251") == "cp1251");
    // meta charset sniffed (case-insensitive, quoted)
    CHECK(detect("<html><head><meta charset=\"KOI8-R\"></head></html>", "") == "koi8r");
    // valid utf-8 with no declaration -> utf8
    CHECK(detect("<p>hello</p>", "") == "utf8");
}

void test_lists() {
    Page pg = parse("<ul><li>one</li><li>two</li></ul>");
    CHECK(countKind(pg, BlockKind::ListItem) == 2);
}

void test_comments_and_malformed() {
    Page pg = parse("before <!-- a <b> comment --> after <div>unterminated");
    CHECK(pg.blocks.size() == 2);
    CHECK(pg.blocks[0].text == "before after");
    CHECK(!pg.blocks[0].bold);
}

void test_theme_color() {
    Page pg = parse("<html><head><meta name=\"theme-color\" content=\"#123456\"></head>"
                    "<body bgcolor=\"#654321\">x</body></html>");
    CHECK(pg.bg == 0x654321u); // body processed after meta, overwrites
}

void test_chunked_feed() {
    std::string html = "<div><a href=\"/a\">link <b>bold</b> tail</a><p>next</p></div>";
    html::Parser p;
    // feed in awkward 3-byte chunks, splitting mid-entity/mid-tag
    std::string html2 = html + "<p>&amp;&#97;</p>";
    p.feed(html2.data(), html2.size());
    p.finish();
    const Page& pg = p.page();
    CHECK(countKind(pg, BlockKind::Link) == 1);
    CHECK(pg.blocks[0].text == "link bold tail");
    CHECK(pg.blocks.back().text == "&a");
}

void test_fixture(const std::string& path) {
    std::string html = slurp(path);
    if (html.empty()) { printf("FAIL cannot read %s\n", path.c_str()); failures++; return; }
    html::Parser p;
    p.feed(html.data(), html.size());
    p.finish();
    CHECK(p.page().blocks.size() > 0);
}

// ---------------- CLI dump ----------------

static void dump(const Page& pg) {
    printf("title: %s\nbg: #%06x\nblocks: %zu\n", pg.title.c_str(), pg.bg, pg.blocks.size());
    for (size_t i = 0; i < pg.blocks.size(); i++) {
        const Block& b = pg.blocks[i];
        const char* k = "?";
        switch (b.kind) {
            case BlockKind::Text: k = "text"; break;
            case BlockKind::Heading: k = "heading"; break;
            case BlockKind::Link: k = "link"; break;
            case BlockKind::Button: k = "button"; break;
            case BlockKind::Input: k = "input"; break;
            case BlockKind::ListItem: k = "list"; break;
            case BlockKind::Table: k = "table"; break;
            case BlockKind::Select: k = "select"; break;
            case BlockKind::Checkbox: k = "checkbox"; break;
            case BlockKind::Image: k = "image"; break;
        }
        printf("[%zu] %s", i, k);
        if (b.kind == BlockKind::Heading) printf(" h%d", b.level);
        if (!b.href.empty()) printf(" href=%s", b.href.c_str());
        if (!b.name.empty()) printf(" name=%s", b.name.c_str());
        if (b.bold) printf(" bold");
        if (b.italic) printf(" italic");
        if (b.kind == BlockKind::Table) {
            printf(" rows=%zu", b.rows.size());
            for (size_t r = 0; r < b.rows.size() && r < 4; r++)
                printf("\n      %s = %.80s", b.rows[r].k.c_str(), b.rows[r].v.c_str());
            printf("\n");
            continue;
        }
        if (b.kind == BlockKind::Select) {
            printf(" sel=%zu opts=", b.selected);
            for (auto& o : b.options) printf("[%s] ", o.c_str());
            printf("\n");
            continue;
        }
        printf(" | %.120s\n", b.text.c_str());
    }
}

int main(int argc, char** argv) {
    if (argc >= 2) {
        const char* arg = argv[1];
        std::string html;
        if (strncmp(arg, "http", 4) == 0) {
            // fetch via curl with a mobile-ish UA
            char cmd[512];
            snprintf(cmd, sizeof(cmd),
                     "curl -sL --max-time 20 -A \"Mozilla/5.0 (Linux; Android 10; Lilka) "
                     "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Mobile Safari/537.36\" "
                     "\"%s\" | head -c 400000", arg);
            FILE* f = popen(cmd, "r");
            if (!f) { printf("curl failed for %s\n", arg); return 2; }
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) html.append(buf, n);
            int rc = pclose(f);
            if (rc != 0) { printf("curl failed for %s\n", arg); return 2; }
        } else {
            html = slurp(arg);
        }
        if (html.empty()) { printf("empty input\n"); return 2; }
        html::Parser p;
        p.feed(html.data(), html.size());
        p.finish();
        dump(p.page());
        return 0;
    }

    test_basics();
    test_headings();
    test_script_style_stripped();
    test_noscript_rendered();
    test_entities();
    test_cyrillic();
    test_charset();
    test_inputs_forms();
    test_textarea();
    test_submit_button_no_text();
    test_checkboxes();
    test_disclosure_toggle();
    test_tables();
    test_table_links();
    test_select();
    test_images();
    test_lists();
    test_comments_and_malformed();
    test_theme_color();
    test_chunked_feed();

    // fixtures
    const char* fx[] = {
        "tests/fixtures/simple.html",
        "tests/fixtures/forms.html",
        "tests/fixtures/messy.html",
        "tests/fixtures/cyrillic.html",
        "tests/fixtures/tables.html",
    };
    for (auto p : fx) test_fixture(p);

    if (failures) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("\nAll parser tests passed.\n");
    return 0;
}
