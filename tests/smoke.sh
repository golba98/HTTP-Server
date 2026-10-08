#!/usr/bin/env bash
# End-to-end check of the real executable: start it on a free port, fetch a
# page with curl, then make sure SIGTERM shuts it down with exit status 0.
#
# Usage: smoke.sh path/to/http-server
set -euo pipefail

server=$1

# CTest treats exit status 77 as "skipped".
command -v curl >/dev/null || { echo "curl not found"; exit 77; }

work=$(mktemp -d)
pid=
cleanup() {
    if [[ -n $pid ]]; then
        kill "$pid" 2>/dev/null || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT

mkdir "$work/root"
echo "smoke test page" > "$work/root/index.html"

"$server" --port 0 --root "$work/root" --quiet > "$work/output" 2>&1 &
pid=$!

port=
for _ in $(seq 50); do
    port=$(sed -n 's|^Listening on http://127\.0\.0\.1:\([0-9]*\) .*|\1|p' "$work/output")
    [[ -n $port ]] && break
    sleep 0.1
done
if [[ -z $port ]]; then
    echo "server did not report its port; output:"
    cat "$work/output"
    exit 1
fi

body=$(curl --fail --silent --show-error --max-time 5 "http://127.0.0.1:$port/")
if [[ $body != "smoke test page" ]]; then
    echo "unexpected body: $body"
    exit 1
fi

kill -TERM "$pid"
status=0
wait "$pid" || status=$?
pid=
if [[ $status -ne 0 ]]; then
    echo "server exited with status $status after SIGTERM; output:"
    cat "$work/output"
    exit 1
fi
echo "ok"
