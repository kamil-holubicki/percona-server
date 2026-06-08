MYSQL_ADD_COMPONENT(binlog_server
  binlog_server_component.cc
  binlog_archive.cc
  file_storage.cc
  log_helpers.cc
  MODULE_ONLY
)
