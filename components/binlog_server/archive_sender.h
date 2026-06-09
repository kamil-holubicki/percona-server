/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef COMPONENTS_BINLOG_SERVER_ARCHIVE_SENDER_H
#define COMPONENTS_BINLOG_SERVER_ARCHIVE_SENDER_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <mysql/components/services/bits/mysql_binlog_dump_handler_bits.h>
#include <mysql/components/services/bits/thd.h>

#include "my_inttypes.h"

namespace binlog_server {
namespace gtid {
class Gtid_set;
}

class BinlogArchive;

class UserChannelMap {
 public:
  bool set_from_csv(const char *csv);
  bool set_from_pairs(
      const std::vector<std::pair<std::string, std::string>> &pairs);
  std::string lookup(const char *user) const;
  std::size_t size() const;
  void clear();

 private:
  mutable std::mutex m_mutex;
  std::map<std::string, std::string> m_map;
};

class ArchiveSender {
 public:
  static ArchiveSender &instance();

  void set_storage(BinlogArchive *storage);
  void set_default_channel(const char *channel_name);
  void set_trace_send_path(bool enabled) { m_trace_send_path = enabled; }
  bool trace_send_path() const { return m_trace_send_path; }
  UserChannelMap &user_channel_map() { return m_user_channel_map; }

  bool handle(MYSQL_THD thd, const char *log_ident, std::uint64_t pos,
              const char *replica_executed_gtids_text, std::uint32_t flags,
              std::uint32_t source_server_id, std::uint32_t replica_server_id,
              enum mysql_binlog_dump_handler_checksum_alg
                  negotiated_checksum_alg,
              const char *replica_user);

 private:
  ArchiveSender() = default;

  std::string resolve_channel(const char *replica_user) const;

  BinlogArchive *m_storage{nullptr};
  std::string m_default_channel;
  UserChannelMap m_user_channel_map;
  bool m_trace_send_path{false};
};

}  // namespace binlog_server

#endif /* COMPONENTS_BINLOG_SERVER_ARCHIVE_SENDER_H */
