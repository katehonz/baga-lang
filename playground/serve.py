#!/usr/bin/env python3
"""Baga playground server — само python3 stdlib, нула зависимости.

Сервира index.html и изпълнява локалния ./baga компилатор върху POST-нат код.
Режими: run (компилирай и стартирай), verify (--verify), json (--verify --json),
proofs (--proofs).

ВНИМАНИЕ: режимът run изпълнява компилирания код. За локална употреба и демо;
ако го хостваш публично, сложи го зад sandbox (container без мрежа, ulimit).
"""
import json
import os
import subprocess
import sys
import tempfile
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.dirname(os.path.abspath(__file__))
BAGA = os.path.join(ROOT, "..", "baga")
INDEX = os.path.join(ROOT, "index.html")

MODES = {
    "run": [],
    "verify": ["--verify"],
    "json": ["--verify", "--json"],
    "proofs": ["--proofs"],
}
TIMEOUT = 10           # секунди за компилация + изпълнение
MAX_CODE = 64 * 1024   # байта код


class Handler(BaseHTTPRequestHandler):
    def _send(self, status, body, ctype):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            with open(INDEX, "rb") as f:
                self._send(200, f.read(), "text/html; charset=utf-8")
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path != "/api/run":
            self.send_error(404)
            return
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(411)
            return
        if n > MAX_CODE + 4096:
            self.send_error(413)
            return
        try:
            req = json.loads(self.rfile.read(n))
        except (json.JSONDecodeError, UnicodeDecodeError):
            self.send_error(400)
            return
        code = req.get("code", "")
        mode = req.get("mode", "run")
        if mode not in MODES or not isinstance(code, str):
            self.send_error(400)
            return
        if len(code.encode("utf-8")) > MAX_CODE:
            self.send_error(413)
            return

        fd, path = tempfile.mkstemp(suffix=".baga", text=True)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write(code)
            p = subprocess.run(
                [BAGA, *MODES[mode], path],
                capture_output=True, text=True, timeout=TIMEOUT,
            )
            out = {"stdout": p.stdout, "stderr": p.stderr, "exit": p.returncode}
        except subprocess.TimeoutExpired:
            out = {"stdout": "", "stderr": f"baga: timeout ({TIMEOUT}s)", "exit": -1}
        finally:
            os.unlink(path)
        self._send(200, json.dumps(out).encode("utf-8"), "application/json")

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    if not os.path.exists(BAGA):
        sys.exit("baga binary липсва — първо изпълни `make` в корена на проекта")
    print(f"⚔️  baga playground → http://localhost:{port}")
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
