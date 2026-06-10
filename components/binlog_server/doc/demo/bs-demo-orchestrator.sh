#!/bin/bash
#
# Binlog Server Demo — Act 3: Start Orchestrator and discover the topology.
#
# Prerequisites: the full 6-instance topology must be running (bs-demo-start.sh).
# All nodes already have the orc_client_user created by bs-demo-start.sh.
#
# Orchestrator UI: http://127.0.0.1:3000/
#

set -e

ORC_BIN="/home/kamil/repo/orchestrator/orchestrator/bin/orchestrator"
ORC_CONF="/home/kamil/repo/orchestrator/orchestrator/conf/orchestrator.conf.json"

log() { echo ">>> $*"; }

if [ ! -x "$ORC_BIN" ]; then
  echo "ERROR: Orchestrator binary not found at $ORC_BIN"
  echo "       Build it first: cd /home/kamil/repo/orchestrator/orchestrator && ./build.sh"
  exit 1
fi

# Clean previous state
rm -f /home/kamil/repo/orchestrator/orchestrator/bin/orchestrator.sqlite3*

log "Discovering demo instances..."
"$ORC_BIN" -config "$ORC_CONF" -c discover -i 127.0.0.1:4000 2>/dev/null || true
"$ORC_BIN" -config "$ORC_CONF" -c discover -i 127.0.0.1:5000 2>/dev/null || true
"$ORC_BIN" -config "$ORC_CONF" -c discover -i 127.0.0.1:6000 2>/dev/null || true
"$ORC_BIN" -config "$ORC_CONF" -c discover -i 127.0.0.1:7000 2>/dev/null || true
"$ORC_BIN" -config "$ORC_CONF" -c discover -i 127.0.0.1:8000 2>/dev/null || true
"$ORC_BIN" -config "$ORC_CONF" -c discover -i 127.0.0.1:9000 2>/dev/null || true

log "Starting Orchestrator web UI on http://127.0.0.1:3000/"
log "(Press Ctrl+C to stop)"
echo ""
exec "$ORC_BIN" -config "$ORC_CONF" http
