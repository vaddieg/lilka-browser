#!/usr/bin/env python3
"""Local TLS server that serves gzip + chunked responses, like Google does.
Serves a deterministic ~200KB HTML document on https://<lan-ip>:8443/test.
"""
import gzip
import http.server
import os
import socketserver
import ssl

PORT = 8443

# Deterministic ~150KB HTML with lots of links + some Cyrillic
CHUNK = []
CHUNK.append("<!DOCTYPE html><html><head><title>Local Test Page</title></head><body>")
CHUNK.append("<h1>Local Test Page</h1><p>Привет, мир. Gzip chunked test.</p>")
for i in range(3000):
    CHUNK.append(
        f'<p>Paragraph {i}: Lorem ipsum dolor sit amet, consectetur adipiscing elit, '
        f'sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.</p>'
    )
for i in range(40):
    CHUNK.append(f'<a href="https://example.com/item/{i}">Link number {i} to example item page</a><br>')
CHUNK.append("<noscript><a href=\"https://example.com/noscript-fallback\">noscript fallback link</a> <p>NOSCRIPT-MARKER visible text</p></noscript>")
CHUNK.append("<table><tr><td>Name</td><td>Value</td></tr><tr><td>alpha</td><td>1</td></tr></table>")
CHUNK.append("<form action='https://example.com/search' method='get'><input name='q'></form>")
CHUNK.append("</body></html>")
BODY = "".join(CHUNK).encode("utf-8")
GZ = gzip.compress(BODY, compresslevel=6)

# Small page dedicated to the <noscript> check (must stay under the 400-block cap)
NS_BODY = (
    "<html><head><title>Noscript Check</title></head><body>"
    "<p>before</p>"
    "<noscript><a href=\"https://example.com/noscript-fallback\">noscript fallback link</a>"
    "<p>NOSCRIPT-MARKER visible text</p></noscript>"
    "<script>var s = '<a href=\"/bad\">hidden</a>';</script>"
    "<p>after</p>"
    "</body></html>"
).encode("utf-8")
NS_GZ = gzip.compress(NS_BODY, compresslevel=6)
print(f"plain={len(BODY)} gz={len(GZ)} ns={len(NS_BODY)}")


class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = NS_GZ if self.path.startswith("/ns") else GZ
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Encoding", "gzip")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        # Send in odd-sized chunks to exercise framing edge cases
        i = 0
        sizes = [7, 4096, 1, 999, 4096, 3, 5555, 2]
        while i < len(body):
            n = sizes[i % len(sizes)]
            piece = body[i : i + n]
            print(f"chunk: size={len(piece)} hex={len(piece):x} first4={piece[:4].hex()}", flush=True)
            self.wfile.write(("%x\r\n" % len(piece)).encode())
            self.wfile.write(piece)
            self.wfile.write(b"\r\n")
            i += len(piece)
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def log_message(self, *a):
        pass


class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    pass


if __name__ == "__main__":
    if not os.path.exists("/tmp/testcert.pem"):
        os.system(
            "openssl req -x509 -newkey rsa:2048 -keyout /tmp/testkey.pem "
            "-out /tmp/testcert.pem -days 30 -nodes -subj '/CN=192.168.2.34' >/dev/null 2>&1"
        )
    srv = S(("0.0.0.0", PORT), H)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain("/tmp/testcert.pem", "/tmp/testkey.pem")
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print(f"listening on https://0.0.0.0:{PORT}/test")
    srv.serve_forever()
