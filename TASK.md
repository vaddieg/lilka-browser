# Basic HTML browser for DIY Lilka console

## Hardware
MCU: ESP32-S3 with 8 MB PSRAM
TFT: 280x240 ST7789
SD Card
SNES-like controls: D-pad, SELECT, START; Action buttons: A B C D
https://github.com/lilka-dev/lilka

## Environment
Keira/FreeRTOS with multiboot, capable of loading and running firmwares from OTA partition
Keira stores wi-fi credentials into NVS, check ../lilka-boombox
PlatformIO + Arduino, ESP-IDF
Apple dev tools for sketching and quick tests

## The idea
Very basic HTML browser for a memory-constrained device, similar to Opera Mini. 
Streams a web page, strips out all the JS and CSS on the fly, preserving only critical elements like
input, buttons, links and plain texts.

## Requirements
- HTTP and HTTPS
- basic things like HTTP redirect
- Big elements laid out vertically, large text blocks are shown in collapsed to 3 lines state by default
  (Press A to expand/collapse the current section)
- UP/DOWN buttons to scroll the page, LEFT/RIGHT act like PgUp/PgDown
- SELECT switch selection rect over clickable elements in the visible frame, START - click on the selected element
- Button B - go to previous page or close popups
- Button D - settings popup: font size, etc.
- Button C - Quick start popup: enter address or pick from few last history items listed


## Requirements (added)
- Link list mode: toggleable flat numbered list of all links on the page (Opera Mini style)
- gzip/deflate response decoding (Accept-Encoding: gzip)
- 2-column (key/value) tables rendered as aligned rows
- <select> dropdowns rendered as cycle-able blocks (A = next option)

## Nice to have
- page background color preserved if trivial to obtain
- jpg and png images cached to SD card if small enough
- minimal text styles like <b> or <i>
- <h>-kind elements rendered with slightly larger font


## Additional requirements
1. Investigate with few common sites like google.com which User-Agent (or other form of identification) 
   gives simpler HTML or less or no JS pages
2. Cyrillic font support, we nailed it in the past, check ../lilka-boombox
3. Start with developing the stream parser using this Mac as a test target
4. Copy on-screen keyboard/input implementation from the lilka-sdk
5. Search for suitable requirement improvement ideas, e.g. from Opera Mini


## Status

### Requirements — almost all done
- ✅ HTTP and HTTPS (WiFiClientSecure, `setInsecure`)
- ✅ Redirects (301/302/303/307/308, with mangled-URL workaround for Google)
- ✅ Vertical block layout
- 🟡 Text collapse to 3 lines / A to expand — **not implemented** (buildLines emits all lines; comment says "full text, no expand/collapse")
- ✅ UP/DOWN scroll, LEFT/RIGHT page up/down
- ✅ SELECT cycles clickable elements in the visible frame, START activates
- ✅ B = back/close, D = settings popup, C = quick start popup
- ✅ Link list mode (Opera Mini style, in settings/D → "Link list")
- ✅ gzip/deflate decoding (miniz, `Accept-Encoding: gzip`)
- ✅ 2-column table rendering (key: value rows)
- ✅ `<select>` dropdowns (A = cycle option)

### Nice to have — partial
- 🟡 Page background color — **parsed** (theme-color meta, body bgcolor → `Page::bg`) but **not applied** in `renderPage()` (always `fillScreen(0x0000)`)
- ✅ Bold/italic style flags tracked in IR (italic has no visual distinction — no italic bitmap font in U8g2)
- ✅ `<h>` with larger font — headings are word-wrapped and rendered with scale=2 (20px font) and yellow color
- ❌ jpg/png images cached to SD

### Additional requirements — all done ✅
1. ✅ UA research → docs/UA-RESEARCH.md (Lilka/1.0 is the best default)
2. ✅ Cyrillic fonts (U8g2 `u8g2_font_6x13_t_cyrillic` / `10x20_t_cyrillic`)
3. ✅ Mac parser test target (Makefile + tests/main.cpp, 12 unit tests + fixture loop)
4. ✅ On-screen keyboard (en/ru/uk, 3 layers, adapted from lilka-sdk)
5. ✅ IDEAS.md with Opera Mini-inspired improvements


## Expert opinion

What follows is a prioritized roadmap of changes that would make the browser
genuinely more useful and compatible on real-world sites, ordered by
impact-per-effort. Everything here is realistic on the ESP32-S3 with 8 MB PSRAM.

### Tier 1 — High impact, low effort (do next)

**1. Charset detection and Windows-1251 → UTF-8 conversion.**
Most Ukrainian and Russian sites that aren't UTF-8 are Windows-1251 (cp1251).
The parser currently assumes UTF-8; feeding cp1251 bytes through U8g2 produces
garbage. Fix: sniff `<meta charset="windows-1251">` and the HTTP
`Content-Type: text/html; charset=windows-1251` header; if detected, run a
single-pass byte→multibyte converter before parsing. The conversion table is
~128 entries × 3 bytes ≈ 400 bytes of flash. This unblocks a large portion of
the Ukrainian/Russian web (older news sites, forums, government pages).

**2. Cookies (session-only, flat jar).** ✅ *Implemented in net.cpp*
Without cookies the browser can't stay logged in, can't pass CSRF checks, and
many sites (DuckDuckGo settings, Google consent) redirect in a loop or show a
cookie-wall. A minimal `name=value` jar per host, stored in RAM (no persistence
needed), capped at ~20 cookies, re-sent on every request to the same host, is
enough. It doesn't need `Path`/`Domain`/`Secure` matching for v1 — just echo
back what the server set. This single change makes Google Search, Wikipedia
mobile, and most login-gated sites work.

**3. POST form submission (at least `application/x-www-form-urlencoded`).**
Currently only GET forms work (append `?key=val` to the action URL). Many
search forms and all login forms use POST. Collect all visible `<input>` values
(name=value pairs) + the submit button's name, url-encode them, and send as a
POST body. The net::Client already has the socket — add a `post()` method next
to `get()`. This + cookies = login capability.

**4. Render headings with the larger font.** ✅ *Implemented in ui.cpp*
The IR already tracks `level` (1–6). `renderPage()` just needs to call
`font::text()` with `scale=2` for headings (or at least `h1`/`h2`). One-line
change, big visual improvement on article pages.

### Tier 2 — High impact, medium effort

**5. `<details>`/`<summary>` as native expand/collapse blocks.**
These HTML5 elements map perfectly onto the existing A=expand/collapse mechanic.
The parser should emit a new `BlockKind::Details` with the summary text
collapsed by default. This is free UX because the control scheme already
exists — just wire the IR. Many modern sites (FAQ pages, settings panels,
GitHub issue descriptions) use `<details>` heavily.

**6. Checkbox/radio inputs: render state, A toggles, include in form submit.**
Currently `<input type="checkbox">` and `<input type="radio">` are silently
dropped. Render `[x]`/`[ ]` and `(•)`/`( )` text prefixes; A toggles the
state; on form submit, include checked values in the query/body. This covers
filter UIs, settings pages, and consent forms. Works naturally with GET forms
(already done) and POST (Tier 1 item 3).

**7. `<noscript>` content preference + Google `&gbv=1` trick.** ✅ *Implemented in net.cpp/ui.cpp*
The parser already renders `<noscript>` content (good!). To go further: when
navigating to `google.com/search?q=...`, automatically append `&gbv=1` (Google
Basic Version) which returns a clean no-JS search results page. This one
heuristic makes Google Search fully usable. Similarly, append `?lite=true` to
Reddit, `?action=render` to MediaWiki, and `m.` prefix to Facebook/Twitter
when the host matches — a small table of "site-specific URL rewrites" in
settings or hardcoded.

**8. Connection keep-alive / reuse.**
Opening a new TLS handshake for every page load on the same host costs 2–4
seconds on ESP32-S3. Keep the `WiFiClientSecure` alive between requests to the
same host (store it in `net::Client`, reuse if `connected()` returns true).
Navigating within a site (e.g., clicking links on Wikipedia) becomes 2–3×
faster.

### Tier 3 — Medium impact, worth doing

**9. Abort loading with B.**
Currently the fetch blocks the main loop (the `BUSY` mode renders inline via
`onStage` but doesn't poll buttons). Run the fetch in a FreeRTOS task or check
`btns_->poll()` inside `onStage()`; if B is pressed, close the socket and
return an error. Users get stuck on slow/unresponsive sites with no way out.

**10. ETag / Last-Modified conditional requests.**
Store the `ETag` and `Last-Modified` headers alongside the cached HTML on SD.
On re-visit, send `If-None-Match` / `If-Modified-Since`; if the server returns
304, use the cache. Saves bandwidth and time, especially on Wikipedia and news
sites where the structure rarely changes even if content updates.

**11. Search within page.**
A simple in-page text search (START+SELECT → keyboard → highlight matching
lines, UP/DOWN to jump between matches). On a 240px screen with 400 blocks,
scrolling manually is painful. The line array is already flat; a `strstr` scan
is trivial.

**12. Auto-skip boilerplate ("compact mode").**
Drop blocks whose text matches common nav/footer strings ("Home", "About",
"Privacy Policy", "Skip to content", "Sign in", "Cookie settings") behind a
toggle in settings. This is the single biggest readability win on sites like
BBC, Habr, Wikipedia that have 20–40 nav blocks before the actual content.
Heuristic: skip blocks inside the first `<nav>` or `<header>`, and all blocks
after the last `<article>` or `<main>`.

**13. Thumbnail images (JPEG only, SD-cached).**
Detect `<img>` tags with `src` pointing to `.jpg`/`.jpeg` (skip `.png` for now
— JPEG decode is simpler on ESP32 with the hardware JPEG decoder or TJpgDec
library). Download if `Content-Length` < 32 KB, decode to a 60×60 or smaller
thumbnail, cache the raw RGB565 bitmap to SD. Display inline as a colored
rectangle placeholder with a small decoded preview. Not critical, but makes
image-heavy pages (news, Wikipedia) much more recognizable.

### Tier 4 — Robustness and polish

**14. HTTP→HTTPS upgrade and mixed-content warning.**
Default to `https://` when the user types a bare domain (already done).
Additionally, if a link points to `http://` on a page loaded over `https://`,
auto-upgrade to `https://` (most servers support it). Show a subtle indicator
(lock icon or color change in the header bar) for HTTPS vs HTTP pages.

**15. Better error recovery: timeout, DNS failure, TLS errors.**
The current error screen shows a single string. Differentiate: "DNS lookup
failed" (check host), "Connection refused" (server down), "TLS handshake
failed" (clock wrong? try http://), "Timeout" (slow network). Each with a
suggested action. Also: if `setInsecure()` is in use, at least log the
server's certificate CN so the user can spot MITM.

**16. Persistent cookie jar (SD).**
Extend the RAM cookie jar (Tier 1) to persist to SD (`/browser/cookies.txt`).
Format: one `host\tname\tvalue\texpires` per line. This lets logins survive
reboots. Guard with a setting ("Save cookies: on/off").

**17. `<textarea>` multi-line input.**
The on-screen keyboard currently returns a single-line string. For `<textarea>`
(common in forums, comment forms), allow line breaks (a special key on the
symbols layer). The IR already has `BlockKind::Input`; add a `multiline` flag.

**18. Page title in the header bar.**
`Page::title` is already parsed. Show it instead of the URL in the header when
available (keep the URL in a tooltip/long-press or in the status footer).
Makes tab-like navigation between pages more intuitive.

### Architecture notes

The current architecture is solid for what it does: a single-threaded
fetch→parse→render loop with a flat IR. The main structural weakness is that
**the fetch is synchronous and blocks the UI loop** — this means no abort, no
progress bar based on content-length, and the UI freezes during DNS/TLS. The
cleanest fix is to run `net::Client::get()` in a separate FreeRTOS task with
a shared state struct (URL, body, error, stage, `aborted` flag) protected by
a mutex. The UI loop polls the state and can set `aborted=true` at any time.
This is a moderate refactor but unlocks abort (Tier 3), progress bars, and
background prefetching of the next likely link.

The parser's 400-block cap (`kMaxBlocks`) is pragmatic but means the bottom of
long pages is silently lost. Consider emitting a final synthetic
`BlockKind::Text` block saying "⚠ Page truncated (400 blocks)" so the user
knows, and potentially a "load more" action that re-parses with a higher cap
(PSRAM can hold thousands of small blocks).
