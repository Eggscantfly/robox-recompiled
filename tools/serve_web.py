#!/usr/bin/env python3
"""Local server for the Emscripten build.

The web build uses -pthread / PROXY_TO_PTHREAD, which needs SharedArrayBuffer,
which browsers only expose to cross-origin-isolated pages. That requires two
response headers -- so the build will NOT run off a plain static server, and
notably will not run on GitHub Pages, which cannot set them. Netlify,
Cloudflare Pages and itch.io can.

Usage:  python tools/serve_web.py [port]     (default 8080, serves build-web/)
"""

import http.server
import os
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    'build-web')


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def end_headers(self):
        # Cross-origin isolation: the two headers SharedArrayBuffer requires.
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def log_message(self, fmt, *args):
        # Only log failures; the asset bundle is one request but the wasm
        # streams, and success spam drowns the errors we care about.
        if args and str(args[1]).startswith(('4', '5')):
            super().log_message(fmt, *args)


if __name__ == '__main__':
    os.chdir(ROOT)
    print(f'serving {ROOT} at http://localhost:{PORT}/robox.html (COOP/COEP on)')
    http.server.ThreadingHTTPServer(('127.0.0.1', PORT), Handler).serve_forever()
