/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "pfs_status_table.h"

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/pfs_plugin_table_service.h>
#include <cstring>
#include <vector>

#include "binlog_archive.h"

extern REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_table_v1);
extern REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_column_string_v2);
extern REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_column_integer_v1);
extern REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_column_bigint_v1);
extern REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_column_timestamp_v2);

namespace binlog_server {

namespace {

BinlogArchive *g_storage_for_pfs = nullptr;

enum Column : unsigned int {
  COL_CHANNEL_NAME = 0,
  COL_ENABLED,
  COL_STORAGE_URI,
  COL_BASE_DIR,
  COL_CURRENT_LOG_NAME,
  COL_HAS_CHECKSUM,
  COL_EVENTS_APPENDED,
  COL_BYTES_APPENDED,
  COL_LAST_SOURCE_LOG_POS,
  COL_DUPLICATES_DROPPED,
  COL_INDEXED_FILES,
  COL_CURRENT_FILE_SIZE_BYTES,
  COL_LAST_EVENT_TIMESTAMP,
  COL_LAST_ERROR_CODE,
  COL_LAST_ERROR_MESSAGE,
  COL_LAST_ERROR_TIMESTAMP,
  COL_WRITE_ERRORS,
  COL_COUNT
};

constexpr const char kTableDef[] =
    "CHANNEL_NAME VARCHAR(64) NOT NULL,"
    "ENABLED VARCHAR(3) NOT NULL,"
    "STORAGE_URI VARCHAR(512) NOT NULL,"
    "BASE_DIR VARCHAR(1024) NOT NULL,"
    "CURRENT_LOG_NAME VARCHAR(255) NOT NULL,"
    "HAS_CHECKSUM VARCHAR(3) NOT NULL,"
    "EVENTS_APPENDED BIGINT UNSIGNED NOT NULL,"
    "BYTES_APPENDED BIGINT UNSIGNED NOT NULL,"
    "LAST_SOURCE_LOG_POS BIGINT UNSIGNED NOT NULL,"
    "DUPLICATES_DROPPED BIGINT UNSIGNED NOT NULL,"
    "INDEXED_FILES BIGINT UNSIGNED NOT NULL,"
    "CURRENT_FILE_SIZE_BYTES BIGINT UNSIGNED NOT NULL,"
    "LAST_EVENT_TIMESTAMP TIMESTAMP(6) NULL,"
    "LAST_ERROR_CODE INT NOT NULL,"
    "LAST_ERROR_MESSAGE VARCHAR(512) NOT NULL,"
    "LAST_ERROR_TIMESTAMP TIMESTAMP(6) NULL,"
    "WRITE_ERRORS BIGINT UNSIGNED NOT NULL";

PFS_engine_table_share_proxy *g_share_list[1] = {nullptr};
PFS_engine_table_share_proxy g_share;

struct ScanHandle {
  std::vector<ChannelStatus> rows;
  unsigned long pos{0};
};

static PSI_table_handle *open_table(PSI_pos **pos) {
  auto *h = new (std::nothrow) ScanHandle();
  if (!h) return nullptr;
  if (g_storage_for_pfs) h->rows = g_storage_for_pfs->snapshot_status();
  *pos = reinterpret_cast<PSI_pos *>(&h->pos);
  return reinterpret_cast<PSI_table_handle *>(h);
}

static void close_table(PSI_table_handle *handle) {
  delete reinterpret_cast<ScanHandle *>(handle);
}

static int rnd_init(PSI_table_handle *handle, bool) {
  auto *h = reinterpret_cast<ScanHandle *>(handle);
  h->pos = 0;
  return 0;
}

static int rnd_next(PSI_table_handle *handle) {
  auto *h = reinterpret_cast<ScanHandle *>(handle);
  if (h->pos >= h->rows.size()) return PFS_HA_ERR_END_OF_FILE;
  ++h->pos;
  return 0;
}

static int rnd_pos(PSI_table_handle *handle) {
  auto *h = reinterpret_cast<ScanHandle *>(handle);
  if (h->pos == 0 || h->pos > h->rows.size()) return PFS_HA_ERR_END_OF_FILE;
  return 0;
}

static void reset_position(PSI_table_handle *handle) {
  auto *h = reinterpret_cast<ScanHandle *>(handle);
  h->pos = 0;
}

static int read_column_value(PSI_table_handle *handle, PSI_field *field,
                             unsigned int index) {
  auto *h = reinterpret_cast<ScanHandle *>(handle);
  if (h->pos == 0 || h->pos > h->rows.size()) return 0;
  const auto &row = h->rows[h->pos - 1];

  switch (static_cast<Column>(index)) {
    case COL_CHANNEL_NAME:
      mysql_service_pfs_plugin_column_string_v2->set_varchar_utf8mb4(
          field, row.channel_name.c_str());
      break;
    case COL_ENABLED:
      mysql_service_pfs_plugin_column_string_v2->set_varchar_utf8mb4(
          field, row.enabled ? "YES" : "NO");
      break;
    case COL_STORAGE_URI:
      mysql_service_pfs_plugin_column_string_v2->set_varchar_utf8mb4(
          field, row.storage_uri.c_str());
      break;
    case COL_BASE_DIR:
      mysql_service_pfs_plugin_column_string_v2->set_varchar_utf8mb4(
          field, row.base_dir.c_str());
      break;
    case COL_CURRENT_LOG_NAME:
      mysql_service_pfs_plugin_column_string_v2->set_varchar_utf8mb4(
          field, row.current_log_name.c_str());
      break;
    case COL_HAS_CHECKSUM:
      mysql_service_pfs_plugin_column_string_v2->set_varchar_utf8mb4(
          field, row.has_checksum ? "YES" : "NO");
      break;
    case COL_EVENTS_APPENDED: {
      PSI_ulonglong val{row.events_appended, false};
      mysql_service_pfs_plugin_column_bigint_v1->set_unsigned(field, val);
      break;
    }
    case COL_BYTES_APPENDED: {
      PSI_ulonglong val{row.bytes_appended, false};
      mysql_service_pfs_plugin_column_bigint_v1->set_unsigned(field, val);
      break;
    }
    case COL_LAST_SOURCE_LOG_POS: {
      PSI_ulonglong val{row.last_source_log_pos, false};
      mysql_service_pfs_plugin_column_bigint_v1->set_unsigned(field, val);
      break;
    }
    case COL_DUPLICATES_DROPPED: {
      PSI_ulonglong val{row.duplicates_dropped, false};
      mysql_service_pfs_plugin_column_bigint_v1->set_unsigned(field, val);
      break;
    }
    case COL_INDEXED_FILES: {
      PSI_ulonglong val{row.indexed_files, false};
      mysql_service_pfs_plugin_column_bigint_v1->set_unsigned(field, val);
      break;
    }
    case COL_CURRENT_FILE_SIZE_BYTES: {
      PSI_ulonglong val{row.current_file_size_bytes, false};
      mysql_service_pfs_plugin_column_bigint_v1->set_unsigned(field, val);
      break;
    }
    case COL_LAST_EVENT_TIMESTAMP:
      if (row.last_event_timestamp_us == 0)
        mysql_service_pfs_plugin_column_timestamp_v2->set(field, nullptr, 0);
      else
        mysql_service_pfs_plugin_column_timestamp_v2->set2(
            field, row.last_event_timestamp_us);
      break;
    case COL_LAST_ERROR_CODE: {
      PSI_int val{static_cast<long>(row.last_error_code), false};
      mysql_service_pfs_plugin_column_integer_v1->set(field, val);
      break;
    }
    case COL_LAST_ERROR_MESSAGE:
      mysql_service_pfs_plugin_column_string_v2->set_varchar_utf8mb4(
          field, row.last_error_message.c_str());
      break;
    case COL_LAST_ERROR_TIMESTAMP:
      if (row.last_error_timestamp_us == 0)
        mysql_service_pfs_plugin_column_timestamp_v2->set(field, nullptr, 0);
      else
        mysql_service_pfs_plugin_column_timestamp_v2->set2(
            field, row.last_error_timestamp_us);
      break;
    case COL_WRITE_ERRORS: {
      PSI_ulonglong val{row.write_errors, false};
      mysql_service_pfs_plugin_column_bigint_v1->set_unsigned(field, val);
      break;
    }
    default:
      break;
  }
  return 0;
}

static unsigned long long get_row_count() { return 0; }

}  // namespace

bool register_pfs_status_table(BinlogArchive *storage) {
  g_storage_for_pfs = storage;

  g_share.m_table_name = "replication_binlog_server_status";
  g_share.m_table_name_length =
      static_cast<unsigned int>(std::strlen(g_share.m_table_name));
  g_share.m_table_definition = kTableDef;
  g_share.m_ref_length = sizeof(unsigned long);
  g_share.m_acl = READONLY;
  g_share.delete_all_rows = nullptr;
  g_share.get_row_count = get_row_count;

  g_share.m_proxy_engine_table = {
      rnd_next,          rnd_init,
      rnd_pos,           nullptr,
      nullptr,           nullptr,
      read_column_value, reset_position,
      nullptr,           nullptr,
      nullptr,           nullptr,
      nullptr,           open_table,
      close_table};

  g_share_list[0] = &g_share;
  return mysql_service_pfs_plugin_table_v1->add_tables(&g_share_list[0], 1) !=
         0;
}

bool unregister_pfs_status_table() {
  bool rc =
      mysql_service_pfs_plugin_table_v1->delete_tables(&g_share_list[0], 1) !=
      0;
  g_storage_for_pfs = nullptr;
  return rc;
}

}  // namespace binlog_server
