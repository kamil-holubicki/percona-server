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

#include "components/binlog_server/gtid_set.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

#define ALLOW_COMPONENT_INCLUDE
#include "mysql/binlog/event/binlog_event.h"
#include "mysql/binlog/event/control_events.h"
#undef ALLOW_COMPONENT_INCLUDE

namespace binlog_server {
namespace gtid {

namespace {

constexpr unsigned kLogEventHeaderLen = LOG_EVENT_HEADER_LEN;
constexpr unsigned kBinlogChecksumLen = BINLOG_CHECKSUM_LEN;

constexpr unsigned char kGtidLogEventType = 33;
constexpr unsigned char kAnonymousGtidLogEventType = 34;
constexpr unsigned char kGtidTaggedLogEventType = 42;

inline char nibble_to_hex(unsigned v) {
  return static_cast<char>(v < 10 ? '0' + v : 'a' + (v - 10));
}

void uuid_bytes_to_text(const unsigned char *uuid, char *out_buf) {
  static const int dash_after[4] = {4, 6, 8, 10};
  int oi = 0;
  for (int i = 0; i < 16; ++i) {
    out_buf[oi++] = nibble_to_hex((uuid[i] >> 4) & 0x0F);
    out_buf[oi++] = nibble_to_hex(uuid[i] & 0x0F);
    for (int d : dash_after) {
      if (i + 1 == d) {
        out_buf[oi++] = '-';
        break;
      }
    }
  }
  out_buf[oi] = '\0';
}

uint64_t read_u64_le(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(p[i]) << (i * 8);
  }
  return v;
}

void skip_ws(std::string_view &sv) {
  while (!sv.empty()) {
    const unsigned char c = static_cast<unsigned char>(sv.front());
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != ',') break;
    sv.remove_prefix(1);
  }
}

bool is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

bool consume_uuid(std::string_view &sv, std::string *out) {
  if (sv.size() < 36) return false;
  static const int kDashOffsets[4] = {8, 13, 18, 23};
  int next_dash_idx = 0;
  for (int i = 0; i < 36; ++i) {
    if (next_dash_idx < 4 && i == kDashOffsets[next_dash_idx]) {
      if (sv[i] != '-') return false;
      ++next_dash_idx;
      continue;
    }
    if (!is_hex_digit(sv[i])) return false;
  }
  out->resize(36);
  for (int i = 0; i < 36; ++i) {
    const char c = sv[i];
    (*out)[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  sv.remove_prefix(36);
  return true;
}

bool consume_tag(std::string_view &sv, std::string *out) {
  std::size_t n = 0;
  while (n < sv.size() && n < 32) {
    const char c = sv[n];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_';
    if (!ok) break;
    ++n;
  }
  if (n == 0) return false;
  out->assign(sv.substr(0, n));
  sv.remove_prefix(n);
  return true;
}

bool consume_int64(std::string_view &sv, std::int64_t *out) {
  if (sv.empty()) return false;
  std::size_t n = 0;
  std::int64_t v = 0;
  while (n < sv.size() && sv[n] >= '0' && sv[n] <= '9') {
    const int d = sv[n] - '0';
    if (v > 922337203685477580LL ||
        (v == 922337203685477580LL && d > 7)) {
      return false;
    }
    v = v * 10 + d;
    ++n;
  }
  if (n == 0) return false;
  *out = v;
  sv.remove_prefix(n);
  return true;
}

bool consume_char(std::string_view &sv, char c) {
  if (sv.empty() || sv.front() != c) return false;
  sv.remove_prefix(1);
  return true;
}

}  // namespace

void Gtid_set::add_range(std::string_view tsid_text, std::int64_t lo,
                         std::int64_t hi) {
  if (lo > hi) return;
  auto &intervals = m_by_tsid[std::string(tsid_text)];
  std::vector<Interval> merged;
  merged.reserve(intervals.size() + 1);
  bool inserted = false;
  std::int64_t cur_lo = lo, cur_hi = hi;
  for (const auto &iv : intervals) {
    if (iv.second + 1 < cur_lo) {
      merged.push_back(iv);
    } else if (iv.first > cur_hi + 1) {
      if (!inserted) {
        merged.emplace_back(cur_lo, cur_hi);
        inserted = true;
      }
      merged.push_back(iv);
    } else {
      cur_lo = std::min(cur_lo, iv.first);
      cur_hi = std::max(cur_hi, iv.second);
    }
  }
  if (!inserted) merged.emplace_back(cur_lo, cur_hi);
  intervals = std::move(merged);
}

void Gtid_set::add_one(std::string_view tsid_text, std::int64_t gno) {
  add_range(tsid_text, gno, gno);
}

void Gtid_set::merge(const Gtid_set &other) {
  for (const auto &[tsid, ivs] : other.m_by_tsid) {
    for (const auto &iv : ivs) add_range(tsid, iv.first, iv.second);
  }
}

bool Gtid_set::contains(std::string_view tsid_text, std::int64_t gno) const {
  const auto it = m_by_tsid.find(std::string(tsid_text));
  if (it == m_by_tsid.end()) return false;
  for (const auto &iv : it->second) {
    if (gno < iv.first) return false;
    if (gno <= iv.second) return true;
  }
  return false;
}

bool Gtid_set::is_subset_of(const Gtid_set &other) const {
  for (const auto &[tsid, ivs] : m_by_tsid) {
    const auto oit = other.m_by_tsid.find(tsid);
    if (oit == other.m_by_tsid.end()) {
      if (!ivs.empty()) return false;
      continue;
    }
    for (const auto &iv : ivs) {
      bool covered = false;
      for (const auto &oiv : oit->second) {
        if (oiv.first <= iv.first && iv.second <= oiv.second) {
          covered = true;
          break;
        }
        if (oiv.first > iv.first) break;
      }
      if (!covered) return false;
    }
  }
  return true;
}

bool Gtid_set::intersects(const Gtid_set &other) const {
  for (const auto &[tsid, ivs] : m_by_tsid) {
    const auto oit = other.m_by_tsid.find(tsid);
    if (oit == other.m_by_tsid.end()) continue;
    for (const auto &iv : ivs) {
      for (const auto &oiv : oit->second) {
        if (iv.first <= oiv.second && oiv.first <= iv.second) return true;
        if (oiv.first > iv.second) break;
      }
    }
  }
  return false;
}

Gtid_set Gtid_set::subtract(const Gtid_set &other) const {
  Gtid_set result;
  for (const auto &[tsid, ivs] : m_by_tsid) {
    const auto oit = other.m_by_tsid.find(tsid);
    if (oit == other.m_by_tsid.end()) {
      result.m_by_tsid[tsid] = ivs;
      continue;
    }
    for (const auto &iv : ivs) {
      std::int64_t lo = iv.first;
      const std::int64_t hi = iv.second;
      for (const auto &oiv : oit->second) {
        if (oiv.second < lo) continue;
        if (oiv.first > hi) break;
        if (oiv.first > lo) result.add_range(tsid, lo, oiv.first - 1);
        lo = oiv.second + 1;
      }
      if (lo <= hi) result.add_range(tsid, lo, hi);
    }
  }
  return result;
}

std::string Gtid_set::to_text() const {
  if (m_by_tsid.empty()) return {};
  std::ostringstream oss;
  bool first = true;
  for (const auto &[tsid, ivs] : m_by_tsid) {
    if (ivs.empty()) continue;
    if (!first) oss << ',';
    first = false;
    oss << tsid;
    for (const auto &iv : ivs) {
      oss << ':' << iv.first;
      if (iv.second != iv.first) oss << '-' << iv.second;
    }
  }
  return oss.str();
}

bool Gtid_set::assign_from_text(std::string_view text) {
  Gtid_set parsed;
  std::string_view sv = text;
  skip_ws(sv);
  while (!sv.empty()) {
    std::string uuid;
    if (!consume_uuid(sv, &uuid)) return false;
    std::string tsid = uuid;
    skip_ws(sv);
    if (!consume_char(sv, ':')) return false;
    skip_ws(sv);
    if (!sv.empty() && !(sv.front() >= '0' && sv.front() <= '9')) {
      std::string tag;
      if (!consume_tag(sv, &tag)) return false;
      tsid.push_back(':');
      tsid.append(tag);
      skip_ws(sv);
      if (!consume_char(sv, ':')) return false;
    }
    skip_ws(sv);
    while (true) {
      std::int64_t lo = 0;
      if (!consume_int64(sv, &lo)) return false;
      std::int64_t hi = lo;
      if (!sv.empty() && sv.front() == '-') {
        sv.remove_prefix(1);
        if (!consume_int64(sv, &hi)) return false;
      }
      if (lo < 1 || hi < lo) return false;
      parsed.add_range(tsid, lo, hi);
      if (sv.empty() || sv.front() != ':') break;
      sv.remove_prefix(1);
      skip_ws(sv);
    }
    skip_ws(sv);
  }
  m_by_tsid = std::move(parsed.m_by_tsid);
  return true;
}

bool Gtid_set::assign_from_previous_gtids_event(const unsigned char *event_buf,
                                                std::size_t event_len,
                                                bool has_checksum) {
  if (event_buf == nullptr ||
      event_len < kLogEventHeaderLen + 8 +
                      (has_checksum ? kBinlogChecksumLen : 0)) {
    return false;
  }
  const std::size_t trailer = has_checksum ? kBinlogChecksumLen : 0;
  const unsigned char *p = event_buf + kLogEventHeaderLen;
  const unsigned char *const end = event_buf + event_len - trailer;

  if (p + 8 > end) return false;
  const uint64_t header_word = read_u64_le(p);
  p += 8;
  const unsigned version = static_cast<unsigned>((header_word >> 56) & 0xFFU);
  constexpr uint64_t kTsidCountMask = (1ULL << 48) - 1ULL;
  const unsigned tsid_count_shift = (version == 0) ? 0u : 8u;
  const uint64_t tsid_count =
      (header_word >> tsid_count_shift) & kTsidCountMask;

  Gtid_set parsed;
  if (version == 0) {
    for (uint64_t i = 0; i < tsid_count; ++i) {
      if (p + 16 + 8 > end) return false;
      char uuid_text[37];
      uuid_bytes_to_text(p, uuid_text);
      p += 16;
      const uint64_t n_intervals = read_u64_le(p);
      p += 8;
      if (n_intervals > (static_cast<uint64_t>(end - p) / 16ULL)) return false;
      for (uint64_t k = 0; k < n_intervals; ++k) {
        const int64_t start = static_cast<int64_t>(read_u64_le(p));
        const int64_t end_excl = static_cast<int64_t>(read_u64_le(p + 8));
        p += 16;
        if (start < 1 || end_excl <= start) return false;
        parsed.add_range(uuid_text, start, end_excl - 1);
      }
    }
  } else if (version == 1) {
    if ((header_word & 0xFFU) != 1U) return false;
    for (uint64_t i = 0; i < tsid_count; ++i) {
      if (p + 16 + 1 > end) return false;
      char uuid_text[37];
      uuid_bytes_to_text(p, uuid_text);
      p += 16;
      const unsigned tag_len = *p;
      ++p;
      if (tag_len > 32) return false;
      if (p + tag_len + 8 > end) return false;
      std::string tsid(uuid_text);
      if (tag_len > 0) {
        tsid.push_back(':');
        tsid.append(reinterpret_cast<const char *>(p), tag_len);
      }
      p += tag_len;
      const uint64_t n_intervals = read_u64_le(p);
      p += 8;
      if (n_intervals > (static_cast<uint64_t>(end - p) / 16ULL)) return false;
      for (uint64_t k = 0; k < n_intervals; ++k) {
        const int64_t start = static_cast<int64_t>(read_u64_le(p));
        const int64_t end_excl = static_cast<int64_t>(read_u64_le(p + 8));
        p += 16;
        if (start < 1 || end_excl <= start) return false;
        parsed.add_range(tsid, start, end_excl - 1);
      }
    }
  } else {
    return false;
  }

  if (p != end) return false;
  m_by_tsid = std::move(parsed.m_by_tsid);
  return true;
}

bool decode_gtid_event(const unsigned char *event_buf, std::size_t event_len,
                       bool has_checksum, std::string *tsid_text,
                       std::int64_t *gno) {
  if (event_buf == nullptr || tsid_text == nullptr || gno == nullptr) {
    return false;
  }
  if (event_len < kLogEventHeaderLen) return false;

  tsid_text->clear();
  *gno = 0;

  const unsigned char event_type = event_buf[4];
  if (event_type != kGtidLogEventType &&
      event_type != kAnonymousGtidLogEventType &&
      event_type != kGtidTaggedLogEventType) {
    return false;
  }

  mysql::binlog::event::Format_description_event fde(BINLOG_VERSION, "");
  fde.footer()->checksum_alg =
      has_checksum
          ? mysql::binlog::event::BINLOG_CHECKSUM_ALG_CRC32
          : mysql::binlog::event::BINLOG_CHECKSUM_ALG_OFF;

  mysql::binlog::event::Gtid_event gev(
      reinterpret_cast<const char *>(event_buf), &fde);
  if (gev.header()->type_code ==
      mysql::binlog::event::ANONYMOUS_GTID_LOG_EVENT) {
    return true;
  }
  const mysql::gtid::Tsid tsid = gev.get_tsid();
  const std::int64_t event_gno = gev.get_gno();
  if (event_gno < 1) return false;
  *tsid_text = tsid.to_string();
  *gno = event_gno;
  return true;
}

}  // namespace gtid
}  // namespace binlog_server
