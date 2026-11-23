#!/usr/bin/env bash
# Serve the SMC-Mixer editor (editor/) on localhost and open it in Chrome (Web MIDI needs a
# secure origin: localhost is one). Ctrl-C stops the server.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-8770}"
python3 -m http.server "$PORT" --bind 127.0.0.1 -d "$ROOT/editor" >/dev/null 2>&1 &
SERVER=$!
trap 'kill $SERVER' EXIT
sleep 0.5
URL="http://localhost:$PORT/"
echo "editor on $URL (Ctrl-C to stop)"
(google-chrome "$URL" || chromium "$URL" || xdg-open "$URL") >/dev/null 2>&1 &
wait $SERVER
