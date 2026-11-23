# Debug CLI (serial command channel)

The firmware reads newline-terminated commands on **UART0 @ 115200 baud** and
prints results back on the same port. Every command is echoed as `[cmd] …`.
This is the main way to drive and inspect the browser without the console
buttons — it's what the test harness uses.

Drive it with the helper (it waits for the reply and, for `dump`, writes the
bytes to a file):

```bash
~/.platformio/penv/bin/python tools/serial_test.py "goto https://example.com" "heap"
```

or a bare `screen` / `picocom` on `/dev/cu.usbmodem101` at 115200 8N1.

## Navigation
| Command | Effect |
|---|---|
| `goto <url>` | Load a URL. A space-separated word (no `http`) is sent to DuckDuckGo as a search. Bare host gets `https://` prepended. |
| `goto esp32 wifi` | Example: search query → DDG HTML results. |
| `back` | History back (or to Quick Start at the root). |
| `quickstart` | Jump to the Quick Start screen. |
| `click <n>` | Activate (click) line *n* of the current page. |
| `cursor <n>` | Move the page cursor to line *n* (no activation). |
| `key <up\|down\|left\|right\|a\|b\|c\|d\|select\|start>` | Inject one hardware-button press. |

## Inspect
| Command | Effect |
|---|---|
| `lines` | Dump the current page's line list (block index, kind, text, href). |
| `qlist` | Dump the Quick Start list and current selection. |
| `dl` | List saved downloads (name + size, up to 20). |
| `heap` | Free internal heap and PSRAM (`[cmd] heap free=… psram=…`). |
| `err` | Print the last error text. |
| `cookies` | Dump the cookie jar. |
| `validators` | Show the cache entry (ETag / Last-Modified) for the current URL. |
| `mkt` | SD sanity probe: `/browser` existence, mkdir, test file write. |
| `hrefhex <n>` | Print the raw bytes (hex) of line *n*'s href — for debugging bad URLs. |

## Downloads
| Command | Effect |
|---|---|
| `dlfile <url>` | Download a file to `/browser/downloads/` (streams to SD, no PSRAM buffer). |
| `dldel <idx>` | Delete the *idx*-th entry from the download list. |

## Images (background pipeline)
| Command | Effect |
|---|---|
| `img` | Status of the current page's image jobs: total / done / busy, plus per-URL `ready` flags. |
| `imgls` | List the `/browser/img` cache: `<hash>.bmp:<size>` for every cached image. |
| `imgtest` | Re-run the image-URL collector on the last page and print what it found (debug). |

> The parser emits an `Image` block per `<img>` in document order, so a **sized
> placeholder is placed inline** where the image appears (height = declared size scaled
> to ≤240×180, default 90px). Only `.jpg`/`.jpeg`/`.png` (≤256 KB each, pixel-capped)
> are downloaded, one-by-one in a background task on core 1, decoded (JpegDEC / PNGdec),
> down-scaled to ≤240×180, and saved as 16-bit BMPs under `/browser/img/`. Each
> placeholder swaps in the image as it lands. A watchdog reboots the device if a single
> image wedges the worker (>50 s), so image loading never stays permanently dead.

## Text / cache debugging
| Command | Effect |
|---|---|
| `find <text>` | In-page find; jumps to first match. |
| `findnext` | Jump to the next match. |
| `findclear` | Clear the active find. |
| `compact` | Toggle compact rendering (also `compact on` / `compact off`). |
| `parse <url>` | Show URL parse result (scheme/host/port/path). |
| `saveraw` | Write the last raw (possibly gzipped) response body to `/browser/raw.gz`. |
| `reinflate` | Re-decompress `/browser/raw.gz` in a loop and print size + FNV hash (gzip stability check). |
| `dump` | Stream the last fetched HTML to the serial port (`DUMPBEGIN <n> … DUMPEND`). |
| `dumpfile <path>` | Stream an SD file to the serial port (same framing as `dump`). |

## Notes
- `goto`, `dlfile`, `dump` block until done; the serial test helper gives those a
  longer read window (downloads up to ~185 s).
- Binary responses are **not** transcoded; `dump`/`dumpfile` are byte-exact, so
  they're safe for verifying a downloaded `.bin` is intact.
- `clearcookies` wipes the cookie jar (no output on success).
