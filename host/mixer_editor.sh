#!/usr/bin/env bash
# Serve the SMC-Mixer editor (editor/) on localhost and open it in Chrome (Web MIDI needs a
# secure origin: localhost is one). Ctrl-C stops the server.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-8770}"
# no caching: an updated editor is picked up by a plain reload
python3 - "$PORT" "$ROOT/editor" >/dev/null 2>&1 <<'PY' &
import functools, http.server, sys
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])),
                                functools.partial(H, directory=sys.argv[2])).serve_forever()
PY
SERVER=$!
trap 'kill $SERVER' EXIT
sleep 0.5
URL="http://localhost:$PORT/"
echo "editor on $URL (Ctrl-C to stop)"
(google-chrome "$URL" || chromium "$URL" || xdg-open "$URL") >/dev/null 2>&1 &
wait $SERVER
