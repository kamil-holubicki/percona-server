#!/bin/bash
#
# Binlog Server Demo Topology — start all 6 instances and configure replication.
#
# Topology:
#   source_1 (port 4000) ──┐
#                           ├──▶ binlog_server (port 6000)
#   source_2 (port 5000) ──┘          │
#                                      ├──▶ replica_1 (port 7000) [source_1 binlogs]
#                                      ├──▶ replica_2 (port 8000) [source_2 binlogs]
#                                      └──▶ replica_3 (port 9000) [source_2 binlogs]
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

# Ports for each instance
declare -A PORTS=( [1]=4000 [2]=5000 [3]=6000 [4]=7000 [5]=8000 [6]=9000 )
declare -A SOCKETS=( [1]=/tmp/bs_demo_n1.sock [2]=/tmp/bs_demo_n2.sock [3]=/tmp/bs_demo_n3.sock \
                     [4]=/tmp/bs_demo_n4.sock [5]=/tmp/bs_demo_n5.sock [6]=/tmp/bs_demo_n6.sock )
declare -A NAMES=( [1]=source_1 [2]=source_2 [3]=binlog_server [4]=replica_1 [5]=replica_2 [6]=replica_3 )

log() { echo ">>> $*"; }

wait_for_socket() {
  local sock="$1" name="$2" timeout=30
  local elapsed=0
  while [ ! -S "$sock" ]; do
    sleep 1
    elapsed=$((elapsed + 1))
    if [ $elapsed -ge $timeout ]; then
      echo "ERROR: $name did not start within ${timeout}s (socket: $sock)"
      exit 1
    fi
  done
}

sql() {
  local sock="$1"; shift
  "$MYSQL" --socket="$sock" -uroot -e "$*"
}

# ============================================================
# Phase 1: Clean start all 6 instances
# ============================================================

for i in 1 2 3 4 5 6; do
  dn="$NODE_DIR/dn${i}"
  cnf="$CNF_DIR/n${i}.cnf"
  name="${NAMES[$i]}"

  log "Starting $name (port ${PORTS[$i]}) — clean datadir"
  rm -rf "$dn"
  cp -ra "$SEEDDB" "$dn"

  # Create bs_storage dir for binlog server
  if [ "$i" = "3" ]; then
    mkdir -p "$dn/bs_storage/src1" "$dn/bs_storage/src2"
  fi

  "$MYSQLD" --defaults-file="$cnf" --daemonize --pid-file="$dn/mysqld.pid"
  wait_for_socket "${SOCKETS[$i]}" "$name"
  log "$name is up"
done

echo ""
log "All 6 instances running."
echo ""

# ============================================================
# Phase 2: Configure sources (create replication user)
# ============================================================

log "Configuring source_1 (port 4000)"
sql "${SOCKETS[1]}" "CREATE USER IF NOT EXISTS 'repl'@'%' IDENTIFIED BY 'repl'; \
                     GRANT REPLICATION SLAVE ON *.* TO 'repl'@'%'; FLUSH PRIVILEGES;"

log "Configuring source_2 (port 5000)"
sql "${SOCKETS[2]}" "CREATE USER IF NOT EXISTS 'repl'@'%' IDENTIFIED BY 'repl'; \
                     GRANT REPLICATION SLAVE ON *.* TO 'repl'@'%'; FLUSH PRIVILEGES;"

# ============================================================
# Phase 2.5: Create orchestrator monitoring user on all nodes
# ============================================================

log "Creating orchestrator monitoring user on all nodes"
for i in 1 2 3 4 5 6; do
  sql "${SOCKETS[$i]}" "
    SET sql_log_bin=0;
    CREATE USER IF NOT EXISTS 'orc_client_user'@'%' IDENTIFIED BY 'orc_client_password';
    GRANT SUPER, PROCESS, REPLICATION SLAVE, REPLICATION CLIENT, RELOAD ON *.* TO 'orc_client_user'@'%';
    GRANT SELECT ON performance_schema.replication_group_members TO 'orc_client_user'@'%';
    FLUSH PRIVILEGES;
    SET sql_log_bin=1;
  "
done

# ============================================================
# Phase 3: Configure binlog server (port 6000)
# ============================================================

log "Installing components on binlog_server"
sql "${SOCKETS[3]}" "
SET sql_log_bin=0;
INSTALL COMPONENT 'file://component_binlog_server';
INSTALL COMPONENT 'file://component_binlog_server_rest_api';
INSTALL PLUGIN binlog_server_relay SONAME 'binlog_server_relay.so';
SET sql_log_bin=1;
"

log "Configuring collection channels"
sql "${SOCKETS[3]}" "
SET sql_log_bin=0;
CHANGE REPLICATION SOURCE TO
  SOURCE_HOST = '127.0.0.1',
  SOURCE_PORT = 4000,
  SOURCE_USER = 'repl',
  SOURCE_PASSWORD = 'repl',
  SOURCE_AUTO_POSITION = 1,
  BINLOG_SERVER = 1,
  BINLOG_SERVER_STORAGE_URI = 'file://$NODE_DIR/dn3/bs_storage/src1/'
  FOR CHANNEL 'src1';
SET sql_log_bin=1;
"

sql "${SOCKETS[3]}" "
SET sql_log_bin=0;
CHANGE REPLICATION SOURCE TO
  SOURCE_HOST = '127.0.0.1',
  SOURCE_PORT = 5000,
  SOURCE_USER = 'repl',
  SOURCE_PASSWORD = 'repl',
  SOURCE_AUTO_POSITION = 1,
  BINLOG_SERVER = 1,
  BINLOG_SERVER_STORAGE_URI = 'file://$NODE_DIR/dn3/bs_storage/src2/'
  FOR CHANNEL 'src2';
SET sql_log_bin=1;
"

log "Starting collection"
sql "${SOCKETS[3]}" "START REPLICA FOR CHANNEL 'src1'; START REPLICA FOR CHANNEL 'src2';"

log "Setting up user-channel routing"
sql "${SOCKETS[3]}" "SET GLOBAL binlog_server.user_channel_map = 'repl_src1=src1,repl_src2=src2';"

log "Creating downstream replication users"
sql "${SOCKETS[3]}" "
SET sql_log_bin=0;
CREATE USER IF NOT EXISTS 'repl_src1'@'%' IDENTIFIED BY 'repl';
GRANT REPLICATION SLAVE ON *.* TO 'repl_src1'@'%';
CREATE USER IF NOT EXISTS 'repl_src2'@'%' IDENTIFIED BY 'repl';
GRANT REPLICATION SLAVE ON *.* TO 'repl_src2'@'%';
FLUSH PRIVILEGES;
SET sql_log_bin=1;
"

# Give the binlog server a moment to collect initial events
sleep 2

# ============================================================
# Phase 4: Configure downstream replicas
# ============================================================

log "Configuring replica_1 (port 7000) — replicates source_1 via binlog_server"
sql "${SOCKETS[4]}" "
SET sql_log_bin=0;
CHANGE REPLICATION SOURCE TO
  SOURCE_HOST = '127.0.0.1',
  SOURCE_PORT = 6000,
  SOURCE_USER = 'repl_src1',
  SOURCE_PASSWORD = 'repl',
  SOURCE_AUTO_POSITION = 1;
SET sql_log_bin=1;
START REPLICA;
"

log "Configuring replica_2 (port 8000) — replicates source_2 via binlog_server"
sql "${SOCKETS[5]}" "
SET sql_log_bin=0;
CHANGE REPLICATION SOURCE TO
  SOURCE_HOST = '127.0.0.1',
  SOURCE_PORT = 6000,
  SOURCE_USER = 'repl_src2',
  SOURCE_PASSWORD = 'repl',
  SOURCE_AUTO_POSITION = 1;
SET sql_log_bin=1;
START REPLICA;
"

log "Configuring replica_3 (port 9000) — replicates source_2 via binlog_server"
sql "${SOCKETS[6]}" "
SET sql_log_bin=0;
CHANGE REPLICATION SOURCE TO
  SOURCE_HOST = '127.0.0.1',
  SOURCE_PORT = 6000,
  SOURCE_USER = 'repl_src2',
  SOURCE_PASSWORD = 'repl',
  SOURCE_AUTO_POSITION = 1;
SET sql_log_bin=1;
START REPLICA;
"

# ============================================================
# Done
# ============================================================

echo ""
log "============================================"
log "  Binlog Server Demo Topology is READY"
log "============================================"
echo ""
echo "  source_1:       mysql --socket=${SOCKETS[1]} -uroot"
echo "  source_2:       mysql --socket=${SOCKETS[2]} -uroot"
echo "  binlog_server:  mysql --socket=${SOCKETS[3]} -uroot"
echo "  replica_1:      mysql --socket=${SOCKETS[4]} -uroot"
echo "  replica_2:      mysql --socket=${SOCKETS[5]} -uroot"
echo "  replica_3:      mysql --socket=${SOCKETS[6]} -uroot"
echo ""
echo "  Dashboard:      http://127.0.0.1:8440/  (admin/admin)"
echo "  REST API:       curl -u admin:admin http://127.0.0.1:8440/api/v1/topology"
echo "  Orchestrator:   ./ps-node/bs_demo_cnf/bs-demo-orchestrator.sh"
echo ""
