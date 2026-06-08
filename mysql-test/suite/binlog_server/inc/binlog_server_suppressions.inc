--disable_query_log
CALL mtr.add_suppression("binlog_server");
CALL mtr.add_suppression("Plugin binlog_server_relay reported");
CALL mtr.add_suppression("Replica SQL for channel.*Replica is not configured");
--enable_query_log
