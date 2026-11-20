#!/usr/bin/env bash
set -u

pkill -u "$USER" -x peer_server peer_client 2>/dev/null || true
sleep 1
rm -rf demo_runtime
rm -f ./*.track ./torrents/*.track
echo "Demo artifacts cleaned."