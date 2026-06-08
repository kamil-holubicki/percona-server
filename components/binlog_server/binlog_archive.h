/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef BINLOG_SERVER_BINLOG_ARCHIVE_H
#define BINLOG_SERVER_BINLOG_ARCHIVE_H

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage_backend.h"

namespace binlog_server {

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

  bool index_loaded{false};
  std::set<std::string> indexed_files;
  std::vector<std::string> file_order;
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

  std::string resolve_channel_base_dir(const char *channel_name) const;

 private:
  std::shared_ptr<ChannelState> find_channel(const std::string &name);
  std::unique_ptr<StorageBackend> create_backend(const std::string &uri);
  std::string effective_uri(const std::string &channel_uri);

  mutable std::mutex m_registry_mutex;
  std::unordered_map<std::string, std::shared_ptr<ChannelState>> m_channels;
  std::vector<std::unique_ptr<StorageBackend>> m_backends;

  mutable std::mutex m_uri_mutex;
  std::string m_default_storage_uri;
};

}  // namespace binlog_server

#endif /* BINLOG_SERVER_BINLOG_ARCHIVE_H */
