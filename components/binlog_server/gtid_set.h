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

#ifndef COMPONENTS_BINLOG_SERVER_GTID_SET_H
#define COMPONENTS_BINLOG_SERVER_GTID_SET_H

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace binlog_server {
namespace gtid {

class Gtid_set {
 public:
  bool empty() const noexcept { return m_by_tsid.empty(); }
  void clear() noexcept { m_by_tsid.clear(); }

  bool assign_from_text(std::string_view text);

  bool assign_from_previous_gtids_event(const unsigned char *event_buf,
                                        std::size_t event_len,
                                        bool has_checksum);

  void add_one(std::string_view tsid_text, std::int64_t gno);

  void add_range(std::string_view tsid_text, std::int64_t lo,
                 std::int64_t hi);

  void merge(const Gtid_set &other);

  bool contains(std::string_view tsid_text, std::int64_t gno) const;

  bool is_subset_of(const Gtid_set &other) const;

  std::string to_text() const;

 private:
  using Interval = std::pair<std::int64_t, std::int64_t>;
  std::map<std::string, std::vector<Interval>> m_by_tsid;
};

bool decode_gtid_event(const unsigned char *event_buf, std::size_t event_len,
                       bool has_checksum, std::string *tsid_text,
                       std::int64_t *gno);

}  // namespace gtid
}  // namespace binlog_server

#endif  // COMPONENTS_BINLOG_SERVER_GTID_SET_H
