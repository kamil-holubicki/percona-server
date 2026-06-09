/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "pfs_archive_table.h"

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/pfs_plugin_table_service.h>
#include <cstring>
#include <vector>

#include "binlog_archive.h"

extern REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_table_v1);
extern REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_column_string_v2);
extern REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_column_bigint_v1);
extern REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_column_timestamp_v2);
extern REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_column_text_v1);

namespace binlog_server {

namespace {

BinlogArchive *g_storage_for_pfs = nullptr;

enum Column : unsigned int {
  COL_CHANNEL_NAME = 0,
  COL_FILE_NAME,
  COL_MIN_EVENT_TIMESTAMP,
  COL_MAX_EVENT_TIMESTAMP,
  COL_EVENT_COUNT,
  COL_SIZE_BYTES,
  COL_IS_ACTIVE,
  COL_PREVIOUS_GTID_SET,
  COL_LAST_GTID_SET,
  COL_COUNT
};

constexpr const char kTableDef[] =
    "CHANNEL_NAME VARCHAR(64) NOT NULL,"
    "FILE_NAME VARCHAR(255) NOT NULL,"
    "MIN_EVENT_TIMESTAMP TIMESTAMP(6) NULL,"
    "MAX_EVENT_TIMESTAMP TIMESTAMP(6) NULL,"
    "EVENT_COUNT BIGINT UNSIGNED NOT NULL,"
    "SIZE_BYTES BIGINT UNSIGNED NOT NULL,"
    "IS_ACTIVE VARCHAR(3) NOT NULL,"
    "PREVIOUS_GTID_SET LONGTEXT,"
    "LAST_GTID_SET LONGTEXT";

PFS_engine_table_share_proxy *g_share_list[1] = {nullptr};
PFS_engine_table_share_proxy g_share;

struct ScanHandle {
  std::vector<ChannelArchive> rows;
  unsigned long pos{0};
};

static PSI_table_handle *open_table(PSI_pos **pos) {
  auto *h = new (std::nothrow) ScanHandle();
  if (!h) return nullptr;
  if (g_storage_for_pfs) h->rows = g_storage_for_pfs->snapshot_archive();
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
    case COL_FILE_NAME:
      mysql_service_pfs_plugin_column_string_v2->set_varchar_utf8mb4(
          field, row.file_name.c_str());
      break;
    case COL_MIN_EVENT_TIMESTAMP:
      if (row.min_event_timestamp_us == 0)
        mysql_service_pfs_plugin_column_timestamp_v2->set(field, nullptr, 0);
      else
        mysql_service_pfs_plugin_column_timestamp_v2->set2(
            field, row.min_event_timestamp_us);
      break;
    case COL_MAX_EVENT_TIMESTAMP:
      if (row.max_event_timestamp_us == 0)
        mysql_service_pfs_plugin_column_timestamp_v2->set(field, nullptr, 0);
      else
        mysql_service_pfs_plugin_column_timestamp_v2->set2(
            field, row.max_event_timestamp_us);
      break;
    case COL_EVENT_COUNT: {
      PSI_ulonglong val{row.event_count, false};
      mysql_service_pfs_plugin_column_bigint_v1->set_unsigned(field, val);
      break;
    }
    case COL_SIZE_BYTES: {
      PSI_ulonglong val{row.size_bytes, false};
      mysql_service_pfs_plugin_column_bigint_v1->set_unsigned(field, val);
      break;
    }
    case COL_IS_ACTIVE:
      mysql_service_pfs_plugin_column_string_v2->set_varchar_utf8mb4(
          field, row.is_active ? "YES" : "NO");
      break;
    case COL_PREVIOUS_GTID_SET:
      if (row.previous_gtid_set.empty())
        mysql_service_pfs_plugin_column_text_v1->set(field, nullptr, 0);
      else
        mysql_service_pfs_plugin_column_text_v1->set(
            field, row.previous_gtid_set.c_str(),
            static_cast<unsigned int>(row.previous_gtid_set.size()));
      break;
    case COL_LAST_GTID_SET:
      if (row.last_gtid_set.empty())
        mysql_service_pfs_plugin_column_text_v1->set(field, nullptr, 0);
      else
        mysql_service_pfs_plugin_column_text_v1->set(
            field, row.last_gtid_set.c_str(),
            static_cast<unsigned int>(row.last_gtid_set.size()));
      break;
    default:
      break;
  }
  return 0;
}

static unsigned long long get_row_count() { return 0; }

}  // namespace

bool register_pfs_archive_table(BinlogArchive *storage) {
  g_storage_for_pfs = storage;

  g_share.m_table_name = "replication_binlog_server_archive";
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

bool unregister_pfs_archive_table() {
  bool rc =
      mysql_service_pfs_plugin_table_v1->delete_tables(&g_share_list[0], 1) !=
      0;
  g_storage_for_pfs = nullptr;
  return rc;
}

}  // namespace binlog_server
