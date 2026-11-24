#!/usr/bin/env python3
"""Serve the SMC-Mixer editor (editor/) on localhost and open it in Chrome or Edge (Web MIDI needs
one of them, and a secure origin: localhost is one). Ctrl-C stops the server. Any OS; on Linux,
host/mixer_editor.sh does the same.

    python3 host/mixer_editor.py           # PORT=8770 by default
"""
import functools
import http.server
import os
import shutil
import subprocess
import sys
import webbrowser
from pathlib import Path

EDITOR = Path(__file__).resolve().parent.parent / "editor"
PORT = int(os.environ.get("PORT", "8770"))


class NoCache(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")      # an updated editor shows on a plain reload
        super().end_headers()

    def log_message(self, *args):
        pass


def open_browser(url):
    """Chrome or Edge if we can find them (Firefox and Safari have no Web MIDI), else the default."""
    if sys.platform == "darwin":
        for app in ("Google Chrome", "Microsoft Edge", "Chromium"):
            if subprocess.run(["open", "-a", app, url], capture_output=True).returncode == 0:
                return
    elif sys.platform.startswith("win"):
        for exe in (r"%ProgramFiles%\Google\Chrome\Application\chrome.exe",
                    r"%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe",
                    r"%LocalAppData%\Google\Chrome\Application\chrome.exe",
                    r"%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe",
                    r"%ProgramFiles%\Microsoft\Edge\Application\msedge.exe"):
            path = os.path.expandvars(exe)
            if os.path.isfile(path):
                subprocess.Popen([path, url])
                return
    else:
        for exe in ("google-chrome", "chromium", "chromium-browser", "microsoft-edge"):
            if shutil.which(exe):
                subprocess.Popen([exe, url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return
    webbrowser.open(url)


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), functools.partial(NoCache, directory=str(EDITOR)))
    url = f"http://localhost:{PORT}/"
    print(f"editor on {url} (Ctrl-C to stop). Use Chrome or Edge: Firefox and Safari have no Web MIDI.")
    open_browser(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
