#!/usr/bin/env python3
"""Assembles the design review page.

Everything is inlined: the artifact runs under a CSP that blocks every external
host, so the WebAssembly build, both typefaces, and all styling have to travel
inside the document.
"""

import base64
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))


def data_uri(path, mime):
    with open(path, "rb") as fh:
        return "data:%s;base64,%s" % (mime, base64.b64encode(fh.read()).decode("ascii"))


def main():
    wasm_js = open(os.path.join(ROOT, "build/wasm/wedge.js"), encoding="utf-8").read()
    if "</script" in wasm_js.lower():
        # Would terminate the host script tag early and silently break the page.
        sys.exit("wedge.js contains a closing script tag; inline it differently")

    inter = data_uri(os.path.join(ROOT, "build/webfonts/inter.woff"), "font/woff")
    news = data_uri(os.path.join(ROOT, "build/webfonts/newsreader.woff"), "font/woff")

    body = open(os.path.join(HERE, "artifact_body.html"), encoding="utf-8").read()
    body = body.replace("__INTER__", inter).replace("__NEWSREADER__", news)
    body = body.replace("/*__WEDGE_WASM__*/", wasm_js)

    out = os.path.join(ROOT, "build/wedge-review.html")
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(body)
    print("  %s  %.1f KB" % (out, os.path.getsize(out) / 1024.0))


if __name__ == "__main__":
    main()
