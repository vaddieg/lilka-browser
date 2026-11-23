# Lilka browser

Basic HTML browser firmware for the DIY Lilka console (ESP32-S3, ST7789 280x240).
Streams a page, strips JS/CSS, renders a vertical list of links / text / inputs.
Think Opera Mini for a 240x280 screen. See `TASK.md` for requirements.

## Layout
```
src/
  ir.h              page IR (blocks: text/link/button/input/table/select/...)
  html_parser.*     tolerant HTML->IR parser (platform independent)
  fw/
    hw.h            pins/display/SD/buttons (from lilka-boombox)
    font.*          U8g2 cyrillic fonts on the GFX canvas (from lilka-boombox)
    net.*           HTTP(S) GET, redirects, gzip (miniz), Wi-Fi creds from Keira NVS
    store.*         SD /browser: settings, history, page cache
    keyboard.*      on-screen keyboard, en+ru (from lilka-sdk inputdialog)
    ui.*            page view, link list, settings, quick start
tests/              mac unit tests + live-URL CLI loop
lib/miniz           vendored gzip inflate (miniz 2.2.0 — master branch had an
                      out-of-bounds write in tinfl; do not "upgrade" to master)
tools/serial_test.py drive the device over serial: "goto <url>", "back", "quickstart",
                      "key <up|down|left|right|a|b|c|d|select|start>" (button inject for UI tests)
tools/local_https_server.py local TLS server (gzip + chunked) for reproducible net tests
```

## Known-good debugging notes (read before touching net.cpp)
- **Chunked body read: never pre-resize the destination string before
  `client.read()`.** `read()` returns 0/-1 on TLS WANT_READ timeouts; a prior
  `raw.resize(size+want)` then leaves null padding inside the body and corrupts
  the gzip stream (this was the Google "blocks=0" bug). Read into a temp buffer
  and `append()` only on success.
- `Client::available()` is unreliable for `WiFiClientSecure` (reports the raw
  socket, not the decrypted buffer) — line framing must use single-byte `read()`
  with its own timeout, not an `available()` gate.
- The parser caps output at 400 blocks (`Parser::kMaxBlocks`) to protect the
  internal heap; huge pages are truncated, not fatal.
- Verify any change with `goto https://www.google.com` → expect
  `FRAMING-OK` + `gzip crc ... OK` + `blocks=16`.
```

## Build & test
```
# parser tests (mac)
make test
# live loop (fixtures + a couple of real sites)
make loop

# firmware
~/.platformio/penv/bin/pio run            # build
~/.platformio/penv/bin/pio run -t upload  # flash (Keira multiboot app slot)
```

## Controls
| key | action |
|---|---|
| UP/DOWN | scroll lines |
| LEFT/RIGHT | page up / down |
| SELECT | cycle selection over clickable elements in the frame |
| START | activate selection (or first clickable) |
| A | expand/collapse text, cycle `<select>` option |
| B | back (history) / close popup |
| C | quick start: history or type URL |
| D | settings: font size, cache, link list mode |

## Notes
- Serial console @115200 doubles as a command channel: `goto <url>`, `back`,
  `quickstart` (line-terminated) — used for autonomous testing without buttons.
- Runs in "guest" OTA mode: `verifyRollbackLater()` returns true (src/main.cpp) so
  ESP-IDF auto-rollback never reverts the boot slot away from Keira OS.
- Wi-Fi credentials come from Keira NVS (`network` namespace, as in lilka-boombox).
- TLS cert validation is off (`setInsecure`) — no CA store on device; document the risk.
- Cookies are ignored in v1 (GET-only navigation).
- UA: see `docs/UA-RESEARCH.md`; ideas for later: `docs/IDEAS.md`;
  serial debug commands: `docs/DEBUG-CLI.md`.
