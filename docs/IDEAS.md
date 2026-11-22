# Requirement improvement ideas (Opera Mini & friends)

Opera Mini's essence: **transcode on the fly into a compact binary markup (OBML),
progressive display, link-centric navigation, data savings**. We can't run a proxy,
but the same UX wins apply locally.

## ✅ Done
- [x] **On-device in-page search** — Settings → "Find on page…"; SELECT/START cycle matches, B clears; footer shows `find 'term' n/m`.
- [x] **Compact / boilerplate mode** — Settings toggle; drops short generic nav/footer blocks ("Sign in", "Skip to content", …); e.g. Wikipedia 463→451 lines.
- [x] **Charset sniffing (cp1251 + KOI8-R → UTF-8)** — honors `Content-Type` charset, then `<meta charset>`, then a UTF-8 validity heuristic; transcoded before parse; unit-tested.
- [x] **ETag / Last-Modified revalidation** — cache stores validators; `If-None-Match`/`If-Modified-Since` on revisit; 304 reuses cache (verified live). Also fixed SD init (was never bound to the shared SPI bus — settings/cache/cookies were silently not persisting).
- [x] **File downloads** — file links (by extension) and non-text responses (by Content-Type) are saved to `/browser/downloads/` instead of parsed; progress via busy screen; DONE screen; **Downloads** entry in Quick Start (UD=move, A=delete with confirm, B=back); never clobbers (name_1.ext); binary bodies are no longer charset-mangled (PNG verified byte-identical); table cells with links are clickable (datasheet tables). txt/md open inline as plain text (not downloaded); download fetch timeout is 3 min.

## Navigation / UX
- **Link list mode** (Opera Mini's "link list"): press a key to switch the frame to a
  numbered list of all links on the page (`1. Home  2. News …`) — great for power users
  and for pages with heavy nav (habr: 435 blocks → a flat list is more usable).
- **Search within page** (START+SELECT or a settings entry): highlight matches, jump between.
- **Auto-skip boilerplate**: drop blocks matching common nav/footer text (Home, About,
  Privacy, Sign in, Skip to content) behind a "compact mode" toggle — this is the single
  biggest win for small screens.
- **Sticky status line**: current URL + progress while loading (we buffer in PSRAM, so we
  can show % fetched and block count as it parses).
- **Favicon / page title in header** — title is already parsed.

## Data / performance
- **Gzip/deflate support**: `Accept-Encoding: gzip` + a tiny inflate (miniz is ~10 KB of
  code) — most pages are 3–10x smaller on the wire. Biggest "Opera Mini-like" win available.
- **Connection reuse** (keep-alive) for pages with relative links we fetch next.
- **Timeouts + progress**: show "…" per block; abort with B.
- **Cache**: revalidate `ETag`/`Last-Modified` on history revisits; store IR (not HTML)
  for the last N pages in the SD cache — re-render after font-size change without refetch.

## Rendering
- **Tables**: currently flattened; detect 2-column key/value tables (very common in
  settings/profile pages) and render as aligned rows.
- **`<select>`/dropdowns**: render as a cycle-able block (A = next value) instead of
  dropping options — covers many filter UIs.
- **Checkbox/radio**: render state, A toggles, value included in GET form submit.
  (v1 is GET-only; checkboxes work with GET.)
- **Readability mode**: extract only the main article (heuristic: largest text block
  cluster) — Opera Mini had "reading view".

## Robustness
- **Charset sniffing**: honor `<meta charset>` / HTTP `Content-Type` before UTF-8 assume
  (Windows-1251 Russian sites are common; U8g2 cyrillic fonts assume UTF-8 in —
  add a cp1251→utf8 converter if meta says so).
- **Relative URL resolution** + `http→https` upgrade option.
- **HTML5 `<details>`/`<summary>`** map naturally onto our A=expand/collapse.
- **`<noscript>` content**: currently skipped; these tags often hold the no-JS fallback
  (e.g., Google's old no-JS search) — *consider un-skipping* behind a setting.

## Safety
- **Phishing guard**: show exact host in status line, never truncate to a pretty name;
  warn on http:// pages with forms (password input over cleartext).
- **Cookie policy**: v1 ignores cookies entirely — document it; settings could allow
  a flat name=value cookie jar for GET-authenticated sites later.
