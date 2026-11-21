# User-Agent investigation

Goal: find a UA (or request profile) that yields simpler / lighter HTML for common sites.

## Method
`curl -sL -A <ua> <url>`, then measured raw size, `<script>` count, `<style>/stylesheet` count,
and parsed with our parser to check the IR is sane.

## Google (www.google.com)

| UA | size | scripts | result |
|---|---|---|---|
| `Lilka/1.0 (ESP32; +…)` | 87 KB | 8 | normal homepage |
| modern mobile Chrome (Android 10) | 280 KB | 8 | normal, heaviest |
| old Android 4.4 Safari | ~2 KB | 1 | **"unsupported browser" page** |
| Lynx/2.9 | ~1.4 KB | 0 | **"unsupported browser" page** |
| curl/8.5 | 87 KB | 8 | normal homepage |
| Opera Mini 9.80 WP | 85 KB | 8 | normal homepage |

Takeaways:
- Very old / text-browser UAs trigger Google's "unsupported browser" interstitial — useless.
- A neutral, honest UA (`Lilka/1.0 …`) or plain modern UA both get the normal 87 KB homepage.
- The modern *mobile* Chrome UA is the worst (280 KB) — no need to impersonate a phone.

## Recommendation
Use a single honest, non-old UA by default:

```
Lilka/1.0 (ESP32-S3; browser; +https://github.com/lilka-dev/lilka)
```

- It avoids bot-blocking (not `curl`), avoids "unsupported browser" pages (not ancient),
  and is lighter than phone-impersonation.
- Keep it in settings (SD config) so it can be switched per-site if a site misbehaves.
- Also send `Accept: text/html`, `Accept-Language` from settings; skip `Accept-Encoding`
  (we don't inflate gzip on-device yet — see IDEAS.md).

## Sites to re-check when adding a UA override
- habr.com — serves full page to any UA, 435 IR blocks (heavy nav; consider filtering)
- bbc.com — 266 IR blocks, works with default UA
