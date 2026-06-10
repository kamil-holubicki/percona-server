#!/bin/bash
#
# Binlog Server Demo — Act 1: Basic Single-Source (manual setup)
#
# Topology:
#   source_1 (port 4000) ──▶ binlog_server (port 6000) ──▶ replica_1 (port 7000)
#
# This script ONLY starts the 3 instances with clean datadirs.
# All configuration is done manually during the demo to show each step.
#
# Run from: /home/kamil/repo/ps/8.0/install
#

set -e

BASE_DIR="${BASE_DIR:-/home/kamil/repo/ps/8.0/install}"
CNF_DIR="$BASE_DIR/ps-node/bs_demo_cnf"
NODE_DIR="$BASE_DIR/ps-node"
SEEDDB="$BASE_DIR/seeddb"
MYSQLD="$BASE_DIR/bin/mysqld"
MYSQL="$BASE_DIR/bin/mysql"

S1=/tmp/bs_demo_n1.sock
S3=/tmp/bs_demo_n3.sock
S4=/tmp/bs_demo_n4.sock

log() { echo ">>> $*"; }

wait_for_socket() {
  local sock="$1" name="$2" timeout=30 elapsed=0
  while [ ! -S "$sock" ]; do
    sleep 1
    elapsed=$((elapsed + 1))
    if [ $elapsed -ge $timeout ]; then
      echo "ERROR: $name did not start within ${timeout}s"
      exit 1
    fi
  done
}

sql() {
  local sock="$1"; shift
  "$MYSQL" --socket="$sock" -uroot -e "$*"
}

# Stop any running instances from previous demo
for sock in $S1 $S3 $S4 /tmp/bs_demo_n2.sock /tmp/bs_demo_n5.sock /tmp/bs_demo_n6.sock; do
  if [ -S "$sock" ]; then
    "$MYSQL" --socket="$sock" -uroot -e "SHUTDOWN;" 2>/dev/null || true
    sleep 1
  fi
done

# ============================================================
# Start 3 instances with fresh datadirs
# ============================================================

log "Starting source_1 (port 4000)"
rm -rf "$NODE_DIR/dn1"
cp -ra "$SEEDDB" "$NODE_DIR/dn1"
"$MYSQLD" --defaults-file="$CNF_DIR/n1.cnf" --daemonize --pid-file="$NODE_DIR/dn1/mysqld.pid"
wait_for_socket "$S1" "source_1"
log "source_1 is up"

log "Starting binlog_server (port 6000)"
rm -rf "$NODE_DIR/dn3"
cp -ra "$SEEDDB" "$NODE_DIR/dn3"
mkdir -p "$NODE_DIR/dn3/bs_storage/src1"
"$MYSQLD" --defaults-file="$CNF_DIR/n3.cnf" --daemonize --pid-file="$NODE_DIR/dn3/mysqld.pid"
wait_for_socket "$S3" "binlog_server"
log "binlog_server is up"

log "Starting replica_1 (port 7000)"
rm -rf "$NODE_DIR/dn4"
cp -ra "$SEEDDB" "$NODE_DIR/dn4"
"$MYSQLD" --defaults-file="$CNF_DIR/n4.cnf" --daemonize --pid-file="$NODE_DIR/dn4/mysqld.pid"
wait_for_socket "$S4" "replica_1"
log "replica_1 is up"

echo ""
log "============================================"
log "  Act 1: Three instances are RUNNING"
log "============================================"
echo ""
echo "  source_1:       mysql --socket=$S1 -uroot"
echo "  binlog_server:  mysql --socket=$S3 -uroot"
echo "  replica_1:      mysql --socket=$S4 -uroot"
echo ""
echo "  Now configure manually (see demo-walkthrough.md)."
echo ""
