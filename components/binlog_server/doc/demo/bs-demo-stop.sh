#!/bin/bash
#
# Stop all 6 binlog server demo instances.
#

BASE_DIR="${BASE_DIR:-/home/kamil/repo/ps/8.0/install}"
MYSQL="$BASE_DIR/bin/mysql"

declare -A SOCKETS=( [1]=/tmp/bs_demo_n1.sock [2]=/tmp/bs_demo_n2.sock [3]=/tmp/bs_demo_n3.sock \
                     [4]=/tmp/bs_demo_n4.sock [5]=/tmp/bs_demo_n5.sock [6]=/tmp/bs_demo_n6.sock )
declare -A NAMES=( [1]=source_1 [2]=source_2 [3]=binlog_server [4]=replica_1 [5]=replica_2 [6]=replica_3 )

for i in 6 5 4 3 2 1; do
  sock="${SOCKETS[$i]}"
  name="${NAMES[$i]}"
  if [ -S "$sock" ]; then
    echo ">>> Stopping $name"
    "$MYSQL" --socket="$sock" -uroot -e "SHUTDOWN;" 2>/dev/null || true
  fi
done

echo ">>> All instances stopped."
