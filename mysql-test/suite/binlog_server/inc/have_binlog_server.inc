--disable_query_log
if (!$BINLOG_SERVER_RELAY) {
  --skip binlog_server_relay plugin not available
}
if (!$BINLOG_SERVER_COMPONENT) {
  --skip component_binlog_server not available
}
--enable_query_log
