/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef BINLOG_SERVER_BINLOG_ARCHIVE_H
#define BINLOG_SERVER_BINLOG_ARCHIVE_H

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage_backend.h"

namespace binlog_server {

struct FileMetadata {
  uint32_t min_event_timestamp{0};
  uint32_t max_event_timestamp{0};
  uint64_t event_count{0};
  uint64_t size_bytes{0};
  std::string previous_gtid_set;
  std::string last_gtid_set;
};

struct ChannelState {
  std::mutex io_mutex;
  std::string name;
  bool enabled{false};
  std::string storage_uri;
  std::string base_dir;
  StorageBackend *backend{nullptr};

  std::string current_log_name;
  std::ofstream out;
  int fsync_fd{-1};

  uint64_t last_source_log_pos{0};
  bool wrote_fde{false};
  bool has_checksum{false};
  uint64_t events_appended{0};
  uint64_t bytes_appended{0};

  // Observability counters (Phase 4)
  uint64_t duplicates_dropped{0};
  uint64_t write_errors{0};
  unsigned long io_failure_count{0};
  int last_error_code{0};
  std::string last_error_message;
  uint64_t last_error_timestamp_us{0};
  uint64_t last_event_timestamp_us{0};

  // Per-file metadata index
  std::map<std::string, FileMetadata> file_metadata;

  bool index_loaded{false};
  std::set<std::string> indexed_files;
  std::vector<std::string> file_order;
};

// Snapshot DTOs for PFS tables (read-only copies safe outside locks)
struct ChannelStatus {
  std::string channel_name;
  bool enabled{false};
  std::string storage_uri;
  std::string base_dir;
  std::string current_log_name;
  bool has_checksum{false};
  uint64_t events_appended{0};
  uint64_t bytes_appended{0};
  uint64_t last_source_log_pos{0};
  uint64_t duplicates_dropped{0};
  uint64_t indexed_files{0};
  uint64_t current_file_size_bytes{0};
  uint64_t last_event_timestamp_us{0};
  int last_error_code{0};
  std::string last_error_message;
  uint64_t last_error_timestamp_us{0};
  uint64_t write_errors{0};
};

struct ChannelStorage {
  std::string channel_name;
  std::string storage_type;
  std::string storage_uri;
  std::string base_path;
  uint64_t file_count{0};
  uint64_t total_bytes_on_disk{0};
  std::string active_file;
  uint64_t active_file_bytes{0};
  std::string status;
  int last_error_code{0};
  std::string last_error_message;
  uint64_t last_error_timestamp_us{0};
};

struct ChannelArchive {
  std::string channel_name;
  std::string file_name;
  uint64_t min_event_timestamp_us{0};
  uint64_t max_event_timestamp_us{0};
  uint64_t event_count{0};
  uint64_t size_bytes{0};
  bool is_active{false};
  std::string previous_gtid_set;
  std::string last_gtid_set;
};

class BinlogArchive {
 public:
  BinlogArchive();
  ~BinlogArchive();

  int configure_channel(const char *channel_name, int enabled,
                        const char *storage_uri);
  int append_event(const char *channel_name, const char *event_buf,
                   unsigned long event_len);
  int close_channel(const char *channel_name);
  int reset_channel(const char *channel_name);
  int flush_channel(const char *channel_name);
  int rotate_channel(const char *channel_name, const char *new_log_name);

  void set_default_storage_uri(const std::string &uri);
  std::string get_default_storage_uri() const;

  void set_storage_root(const std::string &root);
  bool storage_uri_allowed(const std::string &uri,
                           std::string *reason = nullptr) const;

  std::string resolve_channel_base_dir(const char *channel_name) const;

  // PFS snapshot methods
  std::vector<ChannelStatus> snapshot_status() const;
  std::vector<ChannelStorage> snapshot_storage() const;
  std::vector<ChannelArchive> snapshot_archive() const;

  // Rebuild .meta sidecars for a channel (backfill missing metadata)
  long long rebuild_archive_index(const char *channel_name);

  // Purge archive files up to and including the named file
  long long purge_channel(const char *channel_name, const char *up_to_file);

  // Purge files whose accumulated GTID set is fully contained in the given set
  long long purge_before_gtid(const char *channel_name,
                              const char *gtid_set_text);

  // Purge files whose max_event_timestamp is below the threshold (unix seconds)
  long long purge_before_timestamp(const char *channel_name,
                                   unsigned long timestamp);

 private:
  std::shared_ptr<ChannelState> find_channel(const std::string &name);
  std::unique_ptr<StorageBackend> create_backend(const std::string &uri);
  std::string effective_uri(const std::string &channel_uri);

  mutable std::mutex m_registry_mutex;
  std::unordered_map<std::string, std::shared_ptr<ChannelState>> m_channels;
  std::vector<std::unique_ptr<StorageBackend>> m_backends;

  mutable std::mutex m_uri_mutex;
  std::string m_default_storage_uri;

  mutable std::mutex m_root_mutex;
  std::string m_storage_root;
};

}  // namespace binlog_server

#endif /* BINLOG_SERVER_BINLOG_ARCHIVE_H */
