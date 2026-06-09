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

#include "components/binlog_server/archive_sender.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include <openssl/crypto.h>

#include <mysqld_error.h>
#include <mysql/components/services/log_builtins.h>

#include "components/binlog_server/binlog_archive.h"
#include "components/binlog_server/encryption.h"
#include "components/binlog_server/gtid_set.h"
#include "components/binlog_server/log_helpers.h"
#include "components/binlog_server/server_services.h"

namespace binlog_server {

namespace {

constexpr unsigned kBinlogMagicSize = 4;
constexpr unsigned kLogEventHeaderLen = 19;
constexpr unsigned kEventTypeOffset = 4;
constexpr unsigned kServerIdOffset = 5;
constexpr unsigned kEventLenOffset = 9;
constexpr unsigned kLogPosOffset = 13;
constexpr unsigned kFlagsOffset = 17;
constexpr unsigned kRotatePostHeaderLen = 8;
constexpr unsigned kBinlogChecksumLen = 4;
constexpr unsigned char kLogEventArtificialF = 0x20;

constexpr unsigned char kQueryEventType = 2;
constexpr unsigned char kStopEventType = 3;
constexpr unsigned char kRotateEventType = 4;
constexpr unsigned char kFormatDescriptionEventType = 15;
constexpr unsigned char kXidEventType = 16;
constexpr unsigned char kHeartbeatLogEventType = 27;
constexpr unsigned char kGtidLogEventType = 33;
constexpr unsigned char kPreviousGtidsLogEventType = 35;
constexpr unsigned char kXaPrepareLogEventType = 38;
constexpr unsigned char kHeartbeatLogEventV2Type = 41;
constexpr unsigned char kGtidTaggedLogEventType = 42;

constexpr unsigned char kChecksumAlgCrc32 = 1;

constexpr uint32_t kBinlogDumpNonBlock = (1U << 0);
constexpr uint32_t kUseHeartbeatEventV2 = (1U << 1);

constexpr int kDefaultHeartbeatSeconds = 5;
constexpr int kTailPollMs = 100;

uint32_t read_u32_le(const unsigned char *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t read_u16_le(const unsigned char *p) {
  return static_cast<uint16_t>(p[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

void write_u16_le(unsigned char *p, uint16_t v) {
  p[0] = static_cast<unsigned char>(v & 0xFF);
  p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
}

void write_u32_le(unsigned char *p, uint32_t v) {
  p[0] = static_cast<unsigned char>(v & 0xFF);
  p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
  p[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
  p[3] = static_cast<unsigned char>((v >> 24) & 0xFF);
}

void write_u64_le(unsigned char *p, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<unsigned char>((v >> (i * 8)) & 0xFF);
  }
}

struct Crc32Table {
  uint32_t v[256]{};
  constexpr Crc32Table() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
      }
      v[i] = c;
    }
  }
};
constexpr Crc32Table kCrc32Table{};

uint32_t crc32_ieee(uint32_t init, const unsigned char *buf, size_t len) {
  uint32_t c = init ^ 0xFFFFFFFFU;
  for (size_t i = 0; i < len; ++i) {
    c = kCrc32Table.v[(c ^ buf[i]) & 0xFFU] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFU;
}

void trim_inplace(std::string &s) {
  size_t start = 0;
  while (start < s.size() &&
         (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' ||
          s[start] == '\n')) {
    ++start;
  }
  size_t end = s.size();
  while (end > start &&
         (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' ||
          s[end - 1] == '\n')) {
    --end;
  }
  s = s.substr(start, end - start);
}

const char *event_type_name(unsigned char type) {
  switch (type) {
    case kQueryEventType: return "Query";
    case kStopEventType: return "Stop";
    case kRotateEventType: return "Rotate";
    case kFormatDescriptionEventType: return "FDE";
    case kXidEventType: return "Xid";
    case kHeartbeatLogEventType: return "Heartbeat_v1";
    case kGtidLogEventType: return "Gtid";
    case kPreviousGtidsLogEventType: return "Previous_gtids";
    case kXaPrepareLogEventType: return "XA_prepare";
    case kHeartbeatLogEventV2Type: return "Heartbeat_v2";
    case kGtidTaggedLogEventType: return "Gtid_tagged";
    default: return "Unknown";
  }
}

/* ----------------------------------------------------------------------- */
/*                            ArchiveDumpSession                           */
/* ----------------------------------------------------------------------- */

class ArchiveDumpSession {
 public:
  ArchiveDumpSession(MYSQL_THD thd, std::string channel, std::string base_dir,
                     const StorageBackend *backend,
                     const binlog_server::gtid::Gtid_set *excluded_gtids,
                     std::string start_file, my_off_t start_pos,
                     std::uint32_t flags, std::uint32_t source_server_id,
                     int negotiated_checksum_alg, bool trace)
      : m_thd(thd),
        m_killed(thd),
        m_channel(std::move(channel)),
        m_base_dir(std::move(base_dir)),
        m_backend(backend),
        m_excluded_gtids(excluded_gtids),
        m_requested_file(std::move(start_file)),
        m_requested_pos(start_pos),
        m_flags(flags),
        m_wait_new_events((flags & kBinlogDumpNonBlock) == 0),
        m_use_heartbeat_v2((flags & kUseHeartbeatEventV2) != 0),
        m_trace(trace) {
    m_server_id = source_server_id;
    m_negotiated_checksum = negotiated_checksum_alg;
    m_last_event_sent = std::chrono::steady_clock::now();
  }

  ~ArchiveDumpSession() {
    if (!m_pinned_file.empty()) {
      ArchiveSender::instance().unpin_file(m_channel, m_pinned_file);
    }
  }

  bool run();

 private:
  bool resolve_starting_position();
  bool open_current_file();
  bool peek_fde_for_checksum();
  bool ship_current_files_fde();
  bool send_initial_fake_rotate();
  bool send_event_loop();
  bool advance_to_next_archive_file();
  bool wait_for_more_data();
  bool send_synthetic_rotate(const std::string &new_file, my_off_t new_pos);
  bool send_synthetic_rotate_with_checksum(const std::string &new_file,
                                           my_off_t new_pos,
                                           bool with_checksum);
  bool send_heartbeat();
  bool send_heartbeat_v1();
  bool send_heartbeat_v2();
  bool maybe_send_idle_heartbeat();
  bool send_event(const std::vector<unsigned char> &buf);
  bool refresh_index_cache();
  int current_file_index() const;
  bool is_active_file() const;
  bool should_skip_event(const std::vector<unsigned char> &buf);

  // Read from current file at logical offset (binlog body), decrypt if needed.
  bool read_at(my_off_t logical_pos, unsigned char *buf, size_t len);

  MYSQL_THD m_thd;
  server_services::kill_observer m_killed;
  std::string m_channel;
  std::string m_base_dir;
  const StorageBackend *m_backend;
  const binlog_server::gtid::Gtid_set *m_excluded_gtids;
  std::string m_requested_file;
  my_off_t m_requested_pos;
  std::uint32_t m_flags;
  bool m_wait_new_events{true};
  bool m_use_heartbeat_v2{false};

  std::string m_current_file;
  my_off_t m_current_pos{0};
  std::unique_ptr<StorageReadStream> m_in;
  bool m_has_checksum{false};
  int m_negotiated_checksum{-1};
  bool m_fde_seen{false};
  uint32_t m_server_id{0};

  std::vector<std::string> m_index_cache;
  bool m_skipping_transaction{false};

  std::chrono::steady_clock::time_point m_last_event_sent;
  std::chrono::seconds m_heartbeat_period{kDefaultHeartbeatSeconds};
  bool m_trace{false};

  unsigned long long m_events_sent{0};
  unsigned long long m_events_skipped{0};

  std::string m_pinned_file;

  // Encryption support: decrypt on-the-fly when serving encrypted files
  std::unique_ptr<AesCtrCipher> m_decryptor;
  my_off_t m_body_offset{0};  // 0 for plaintext, 512 for encrypted
};

bool ArchiveDumpSession::refresh_index_cache() {
  m_index_cache.clear();
  return m_backend->index_load(m_base_dir, m_index_cache);
}

int ArchiveDumpSession::current_file_index() const {
  for (size_t i = 0; i < m_index_cache.size(); ++i) {
    if (m_index_cache[i] == m_current_file) return static_cast<int>(i);
  }
  return -1;
}

bool ArchiveDumpSession::is_active_file() const {
  if (m_index_cache.empty()) return false;
  return m_index_cache.back() == m_current_file;
}

bool ArchiveDumpSession::resolve_starting_position() {
  if (!refresh_index_cache() || m_index_cache.empty()) {
    bslog(WARNING_LEVEL,
          "binlog_server: channel '%s' has no archived binlog files; "
          "nothing to serve",
          m_channel.c_str());
    return false;
  }

  const bool gtid_mode =
      m_excluded_gtids != nullptr && !m_excluded_gtids->empty();
  const bool explicit_start =
      !m_requested_file.empty() && m_requested_file != "";

  if (gtid_mode && !explicit_start) {
    for (auto it = m_index_cache.rbegin(); it != m_index_cache.rend(); ++it) {
      auto stream = m_backend->open_read(m_base_dir, *it);
      if (!stream) continue;

      unsigned char header[kLogEventHeaderLen];
      if (!stream->read_at(kBinlogMagicSize, header, kLogEventHeaderLen))
        continue;
      const uint32_t fde_len = read_u32_le(header + kEventLenOffset);
      if (fde_len < kLogEventHeaderLen + 5) continue;
      std::vector<unsigned char> fde(fde_len);
      if (!stream->read_at(kBinlogMagicSize, fde.data(), fde_len)) continue;

      const unsigned char alg = fde[fde_len - 5];
      const bool fde_has_checksum = (alg == kChecksumAlgCrc32);

      const uint64_t pgev_offset = kBinlogMagicSize + fde_len;
      unsigned char pgev_hdr[kLogEventHeaderLen];
      if (!stream->read_at(pgev_offset, pgev_hdr, kLogEventHeaderLen))
        continue;
      if (pgev_hdr[kEventTypeOffset] != kPreviousGtidsLogEventType) continue;
      const uint32_t pgev_len = read_u32_le(pgev_hdr + kEventLenOffset);
      if (pgev_len < kLogEventHeaderLen) continue;
      std::vector<unsigned char> pgev(pgev_len);
      if (!stream->read_at(pgev_offset, pgev.data(), pgev_len)) continue;

      binlog_server::gtid::Gtid_set file_previous;
      if (!file_previous.assign_from_previous_gtids_event(
              pgev.data(), pgev.size(), fde_has_checksum))
        continue;
      if (file_previous.is_subset_of(*m_excluded_gtids)) {
        m_current_file = *it;
        m_current_pos = kBinlogMagicSize;
        bslog(WARNING_LEVEL,
              "binlog_server: channel '%s' AUTO_POSITION matched at "
              "file='%s'",
              m_channel.c_str(), m_current_file.c_str());
        return true;
      }
    }
    m_current_file = m_index_cache.front();
    m_current_pos = kBinlogMagicSize;
    bslog(WARNING_LEVEL,
          "binlog_server: channel '%s' AUTO_POSITION found no subset match; "
          "starting from oldest '%s'",
          m_channel.c_str(), m_current_file.c_str());
    return true;
  }

  if (explicit_start) {
    for (const auto &f : m_index_cache) {
      if (f == m_requested_file) {
        m_current_file = m_requested_file;
        m_current_pos = (m_requested_pos < kBinlogMagicSize)
                            ? kBinlogMagicSize
                            : m_requested_pos;
        return true;
      }
    }
    bslog(WARNING_LEVEL,
          "binlog_server: channel '%s' requested '%s' not in archive; "
          "falling through to oldest",
          m_channel.c_str(), m_requested_file.c_str());
  }

  m_current_file = m_index_cache.front();
  m_current_pos = kBinlogMagicSize;
  return true;
}

bool ArchiveDumpSession::open_current_file() {
  if (!m_pinned_file.empty()) {
    ArchiveSender::instance().unpin_file(m_channel, m_pinned_file);
    m_pinned_file.clear();
  }
  m_in.reset();
  m_decryptor.reset();
  m_body_offset = 0;

  m_in = m_backend->open_read(m_base_dir, m_current_file);
  if (!m_in) {
    bslog_code(WARNING_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
               "cannot open archive '%s%s' via storage backend",
               m_base_dir.c_str(), m_current_file.c_str());
    return false;
  }

  unsigned char magic_check[4];
  if (m_in->read_at(0, magic_check, 4) &&
      EncryptionHeader::is_encrypted(magic_check)) {
    unsigned char hdr_buf[kEncryptionHeaderSize];
    if (!m_in->read_at(0, hdr_buf, kEncryptionHeaderSize)) {
      bslog(WARNING_LEVEL,
            "binlog_server: channel '%s' encrypted file '%s' header truncated",
            m_channel.c_str(), m_current_file.c_str());
      m_in.reset();
      return false;
    }

    EncryptionHeader hdr;
    if (!hdr.deserialize(hdr_buf)) {
      bslog(WARNING_LEVEL,
            "binlog_server: channel '%s' failed to parse encryption header "
            "in '%s'",
            m_channel.c_str(), m_current_file.c_str());
      m_in.reset();
      return false;
    }

    unsigned char password[kFilePasswordLen];
    if (!decrypt_file_password(hdr.key_id, hdr.encrypted_password, hdr.iv,
                               password)) {
      bslog(WARNING_LEVEL,
            "binlog_server: channel '%s' cannot decrypt file password for "
            "'%s' (key='%s')",
            m_channel.c_str(), m_current_file.c_str(), hdr.key_id.c_str());
      OPENSSL_cleanse(password, sizeof(password));
      m_in.reset();
      return false;
    }

    m_decryptor = std::make_unique<AesCtrCipher>();
    if (!m_decryptor->open(password, kFilePasswordLen)) {
      OPENSSL_cleanse(password, sizeof(password));
      m_decryptor.reset();
      m_in.reset();
      return false;
    }
    OPENSSL_cleanse(password, sizeof(password));
    m_body_offset = kEncryptionHeaderSize;
  }

  m_fde_seen = false;
  ArchiveSender::instance().pin_file(m_channel, m_current_file);
  m_pinned_file = m_current_file;
  return true;
}

bool ArchiveDumpSession::read_at(my_off_t logical_pos, unsigned char *buf,
                                 size_t len) {
  my_off_t phys_pos = m_body_offset + logical_pos;
  if (!m_in || !m_in->read_at(static_cast<uint64_t>(phys_pos), buf, len))
    return false;
  if (m_decryptor) {
    m_decryptor->set_offset(logical_pos);
    if (!m_decryptor->process(buf, len)) return false;
  }
  return true;
}

bool ArchiveDumpSession::peek_fde_for_checksum() {
  unsigned char header[kLogEventHeaderLen];
  if (!read_at(kBinlogMagicSize, header, kLogEventHeaderLen)) {
    bslog_code(ERROR_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
               "channel '%s': cannot read FDE header from '%s'",
               m_channel.c_str(), m_current_file.c_str());
    return false;
  }
  if (header[kEventTypeOffset] != kFormatDescriptionEventType) {
    bslog_code(ERROR_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
               "channel '%s': first event in '%s' is type %u (%s), "
               "expected FDE (15)",
               m_channel.c_str(), m_current_file.c_str(),
               header[kEventTypeOffset],
               event_type_name(header[kEventTypeOffset]));
    return false;
  }
  const uint32_t fde_len = read_u32_le(header + kEventLenOffset);
  if (fde_len < kLogEventHeaderLen + 5) {
    bslog_code(ERROR_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
               "channel '%s': FDE in '%s' too short (len=%u)",
               m_channel.c_str(), m_current_file.c_str(), fde_len);
    return false;
  }

  unsigned char alg = 0;
  if (!read_at(kBinlogMagicSize + fde_len - 5, &alg, 1)) return false;
  m_has_checksum = (alg == kChecksumAlgCrc32);

  if (m_has_checksum && m_negotiated_checksum == -1) {
    bslog_code(ERROR_LEVEL, ER_BINLOG_SERVER_CONFIG_REJECTED,
               "channel '%s' has CRC32 checksums but replica did not negotiate "
               "@source_binlog_checksum; refusing dump",
               m_channel.c_str());
    return false;
  }
  return true;
}

bool ArchiveDumpSession::ship_current_files_fde() {
  unsigned char header[kLogEventHeaderLen];
  if (!read_at(kBinlogMagicSize, header, kLogEventHeaderLen)) return false;
  if (header[kEventTypeOffset] != kFormatDescriptionEventType) return false;
  const uint32_t fde_len = read_u32_le(header + kEventLenOffset);
  if (fde_len < kLogEventHeaderLen + 5) return false;
  std::vector<unsigned char> fde(fde_len);
  if (!read_at(kBinlogMagicSize, fde.data(), fde_len)) return false;

  const unsigned char alg = fde[fde_len - 5];
  m_has_checksum = (alg == kChecksumAlgCrc32);
  m_fde_seen = true;

  if (send_event(fde)) return false;
  return true;
}

bool ArchiveDumpSession::send_initial_fake_rotate() {
  const bool wire_has_checksum = (m_negotiated_checksum == 1);
  return send_synthetic_rotate_with_checksum(m_current_file, m_current_pos,
                                             wire_has_checksum);
}

bool ArchiveDumpSession::send_synthetic_rotate(const std::string &new_file,
                                               my_off_t new_pos) {
  return send_synthetic_rotate_with_checksum(new_file, new_pos, m_has_checksum);
}

bool ArchiveDumpSession::send_synthetic_rotate_with_checksum(
    const std::string &new_file, my_off_t new_pos, bool with_checksum) {
  const size_t body = kRotatePostHeaderLen + new_file.size();
  const size_t total =
      kLogEventHeaderLen + body + (with_checksum ? kBinlogChecksumLen : 0);
  std::vector<unsigned char> ev(total, 0);

  ev[kEventTypeOffset] = kRotateEventType;
  write_u32_le(ev.data() + kServerIdOffset, m_server_id);
  write_u32_le(ev.data() + kEventLenOffset, static_cast<uint32_t>(total));
  write_u16_le(ev.data() + kFlagsOffset, kLogEventArtificialF);
  write_u64_le(ev.data() + kLogEventHeaderLen, static_cast<uint64_t>(new_pos));
  std::memcpy(ev.data() + kLogEventHeaderLen + kRotatePostHeaderLen,
              new_file.data(), new_file.size());

  if (with_checksum) {
    const uint32_t crc = crc32_ieee(0, ev.data(), total - kBinlogChecksumLen);
    write_u32_le(ev.data() + total - kBinlogChecksumLen, crc);
  }
  return send_event(ev) == false;
}

bool ArchiveDumpSession::send_heartbeat() {
  if (m_use_heartbeat_v2) return send_heartbeat_v2();
  return send_heartbeat_v1();
}

bool ArchiveDumpSession::send_heartbeat_v1() {
  const size_t total = kLogEventHeaderLen + m_current_file.size() +
                       (m_has_checksum ? kBinlogChecksumLen : 0);
  std::vector<unsigned char> ev(total, 0);

  ev[kEventTypeOffset] = kHeartbeatLogEventType;
  write_u32_le(ev.data() + kServerIdOffset, m_server_id);
  write_u32_le(ev.data() + kEventLenOffset, static_cast<uint32_t>(total));
  write_u32_le(ev.data() + kLogPosOffset,
               static_cast<uint32_t>(m_current_pos));
  std::memcpy(ev.data() + kLogEventHeaderLen, m_current_file.data(),
              m_current_file.size());

  if (m_has_checksum) {
    const uint32_t crc = crc32_ieee(0, ev.data(), total - kBinlogChecksumLen);
    write_u32_le(ev.data() + total - kBinlogChecksumLen, crc);
  }
  return send_event(ev) == false;
}

bool ArchiveDumpSession::send_heartbeat_v2() {
  const size_t name_len = m_current_file.size();
  const size_t encoded_name_len =
      (name_len < 128) ? 1 + name_len : 2 + name_len;
  const size_t total = kLogEventHeaderLen + encoded_name_len +
                       (m_has_checksum ? kBinlogChecksumLen : 0);
  std::vector<unsigned char> ev(total, 0);

  ev[kEventTypeOffset] = kHeartbeatLogEventV2Type;
  write_u32_le(ev.data() + kServerIdOffset, m_server_id);
  write_u32_le(ev.data() + kEventLenOffset, static_cast<uint32_t>(total));
  write_u32_le(ev.data() + kLogPosOffset,
               static_cast<uint32_t>(m_current_pos));

  size_t off = kLogEventHeaderLen;
  if (name_len < 128) {
    ev[off++] = static_cast<unsigned char>(name_len);
  } else {
    ev[off++] = static_cast<unsigned char>((name_len & 0x7F) | 0x80);
    ev[off++] = static_cast<unsigned char>(name_len >> 7);
  }
  std::memcpy(ev.data() + off, m_current_file.data(), name_len);

  if (m_has_checksum) {
    const uint32_t crc = crc32_ieee(0, ev.data(), total - kBinlogChecksumLen);
    write_u32_le(ev.data() + total - kBinlogChecksumLen, crc);
  }
  return send_event(ev) == false;
}

bool ArchiveDumpSession::maybe_send_idle_heartbeat() {
  using namespace std::chrono;
  const auto now = steady_clock::now();
  if (duration_cast<seconds>(now - m_last_event_sent) >= m_heartbeat_period) {
    return !send_heartbeat();
  }
  return true;
}

bool ArchiveDumpSession::send_event(const std::vector<unsigned char> &buf) {
  if (server_services::send_event(m_thd, buf.data(), buf.size())) {
    bslog(WARNING_LEVEL,
          "binlog_server: channel '%s' send_event failed "
          "(type=%u/%s, len=%zu, file='%s', pos=%llu)",
          m_channel.c_str(),
          buf.size() >= kLogEventHeaderLen ? buf[kEventTypeOffset] : 0,
          buf.size() >= kLogEventHeaderLen
              ? event_type_name(buf[kEventTypeOffset])
              : "?",
          buf.size(), m_current_file.c_str(),
          static_cast<unsigned long long>(m_current_pos));
    return true;
  }
  if (server_services::flush(m_thd)) {
    bslog(WARNING_LEVEL,
          "binlog_server: channel '%s' flush failed (file='%s', pos=%llu)",
          m_channel.c_str(), m_current_file.c_str(),
          static_cast<unsigned long long>(m_current_pos));
    return true;
  }
  m_last_event_sent = std::chrono::steady_clock::now();
  return false;
}

bool ArchiveDumpSession::should_skip_event(
    const std::vector<unsigned char> &buf) {
  if (buf.size() < kLogEventHeaderLen) return false;
  const unsigned char event_type = buf[kEventTypeOffset];

  const uint16_t flags = read_u16_le(buf.data() + kFlagsOffset);
  if (flags & kLogEventArtificialF) return true;

  if (event_type == kFormatDescriptionEventType) return true;

  if (event_type == kStopEventType && !is_active_file()) return true;

  if (event_type == kHeartbeatLogEventType ||
      event_type == kHeartbeatLogEventV2Type)
    return true;

  if (m_excluded_gtids != nullptr) {
    if (event_type == kGtidLogEventType ||
        event_type == kGtidTaggedLogEventType) {
      std::string tsid_text;
      std::int64_t gno = 0;
      const bool decoded = binlog_server::gtid::decode_gtid_event(
          buf.data(), buf.size(), m_has_checksum, &tsid_text, &gno);
      if (decoded && !tsid_text.empty() && gno >= 1) {
        m_skipping_transaction =
            m_excluded_gtids->contains(tsid_text, gno);
      } else {
        m_skipping_transaction = false;
      }
    }
    if (m_skipping_transaction) {
      if (event_type == kXidEventType ||
          event_type == kXaPrepareLogEventType)
        m_skipping_transaction = false;
      return true;
    }
  }

  return false;
}

bool ArchiveDumpSession::advance_to_next_archive_file() {
  if (!refresh_index_cache()) return false;
  const int idx = current_file_index();
  if (idx < 0 || idx + 1 >= static_cast<int>(m_index_cache.size()))
    return false;
  const std::string next = m_index_cache[idx + 1];

  (void)send_synthetic_rotate(next, kBinlogMagicSize);

  m_current_file = next;
  m_current_pos = kBinlogMagicSize;
  if (!open_current_file()) return false;
  if (!ship_current_files_fde()) return false;

  unsigned char fde_hdr[kLogEventHeaderLen];
  if (!read_at(kBinlogMagicSize, fde_hdr, kLogEventHeaderLen)) return false;
  const uint32_t fde_len = read_u32_le(fde_hdr + kEventLenOffset);
  m_current_pos = kBinlogMagicSize + fde_len;
  return true;
}

bool ArchiveDumpSession::wait_for_more_data() {
  using namespace std::chrono;
  const auto deadline = steady_clock::now() + m_heartbeat_period;
  while (steady_clock::now() < deadline) {
    if (m_killed.killed()) return false;
    const uint64_t sz = m_backend->file_size(m_base_dir, m_current_file);
    if (static_cast<unsigned long long>(m_current_pos + m_body_offset) < sz) {
      return true;
    }
    if (refresh_index_cache()) {
      const int idx = current_file_index();
      if (idx >= 0 && idx + 1 < static_cast<int>(m_index_cache.size())) {
        return true;
      }
    }
    std::this_thread::sleep_for(milliseconds(kTailPollMs));
  }
  return true;
}

bool ArchiveDumpSession::send_event_loop() {
  while (!m_killed.killed()) {
    unsigned char header[kLogEventHeaderLen];
    if (!read_at(m_current_pos, header, kLogEventHeaderLen)) {
      if (!is_active_file()) {
        if (!advance_to_next_archive_file()) {
          if (!m_wait_new_events) return true;
          if (!wait_for_more_data()) return false;
          if (m_backend->file_size(m_base_dir, m_current_file) <=
              static_cast<unsigned long long>(m_current_pos +
                                             m_body_offset)) {
            (void)send_heartbeat();
          }
        }
        continue;
      }
      if (!m_wait_new_events) return true;
      if (!wait_for_more_data()) return false;
      if (m_backend->file_size(m_base_dir, m_current_file) <=
          static_cast<unsigned long long>(m_current_pos + m_body_offset)) {
        if (!is_active_file()) {
          if (!advance_to_next_archive_file()) (void)send_heartbeat();
        } else {
          (void)send_heartbeat();
        }
      }
      continue;
    }

    const uint32_t event_len = read_u32_le(header + kEventLenOffset);
    const unsigned char event_type = header[kEventTypeOffset];
    const uint16_t event_flags = read_u16_le(header + kFlagsOffset);
    if (event_len < kLogEventHeaderLen) {
      bslog_code(ERROR_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
                 "channel '%s': corrupt event header in '%s' at pos %llu "
                 "(event_len=%u, type=%u/%s)",
                 m_channel.c_str(), m_current_file.c_str(),
                 static_cast<unsigned long long>(m_current_pos), event_len,
                 event_type, event_type_name(event_type));
      return false;
    }

    std::vector<unsigned char> ev(event_len);
    if (!read_at(m_current_pos, ev.data(), event_len)) {
      if (!m_wait_new_events) return true;
      if (!wait_for_more_data()) return false;
      continue;
    }

    if (should_skip_event(ev)) {
      ++m_events_skipped;
      m_current_pos += event_len;
      if (!maybe_send_idle_heartbeat()) return false;
      continue;
    }

    if (send_event(ev)) return false;
    ++m_events_sent;
    m_current_pos += event_len;

    if (event_type == kRotateEventType &&
        !(event_flags & kLogEventArtificialF)) {
      if (!refresh_index_cache()) return false;
      const int idx = current_file_index();
      if (idx >= 0 && idx + 1 < static_cast<int>(m_index_cache.size())) {
        m_current_file = m_index_cache[idx + 1];
        m_current_pos = kBinlogMagicSize;
        if (!open_current_file()) return false;
        if (!ship_current_files_fde()) return false;
        unsigned char fde_hdr[kLogEventHeaderLen];
        if (!read_at(kBinlogMagicSize, fde_hdr, kLogEventHeaderLen))
          return false;
        const uint32_t fde_sz = read_u32_le(fde_hdr + kEventLenOffset);
        m_current_pos = kBinlogMagicSize + fde_sz;
      }
    }
  }
  return true;
}

bool ArchiveDumpSession::run() {
  if (!resolve_starting_position()) return true;
  if (!open_current_file()) return true;
  if (!peek_fde_for_checksum()) return true;
  if (!send_initial_fake_rotate()) return true;
  if (!ship_current_files_fde()) return true;

  unsigned char fde_hdr[kLogEventHeaderLen];
  if (!read_at(kBinlogMagicSize, fde_hdr, kLogEventHeaderLen)) return true;
  const uint32_t fde_len = read_u32_le(fde_hdr + kEventLenOffset);
  const my_off_t after_fde =
      static_cast<my_off_t>(kBinlogMagicSize) + fde_len;
  if (m_current_pos < after_fde) m_current_pos = after_fde;

  (void)send_event_loop();
  return true;
}

}  // namespace

/* ---------------------------- UserChannelMap ---------------------------- */

bool UserChannelMap::set_from_csv(const char *csv) {
  std::map<std::string, std::string> parsed;
  if (csv == nullptr) csv = "";

  std::stringstream ss(csv);
  std::string item;
  while (std::getline(ss, item, ',')) {
    trim_inplace(item);
    if (item.empty()) continue;
    const auto eq = item.find('=');
    if (eq == std::string::npos || eq == 0 || eq == item.size() - 1)
      return false;
    std::string user = item.substr(0, eq);
    std::string channel = item.substr(eq + 1);
    trim_inplace(user);
    trim_inplace(channel);
    if (user.empty() || channel.empty()) return false;
    parsed[std::move(user)] = std::move(channel);
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  m_map = std::move(parsed);
  return true;
}

bool UserChannelMap::set_from_pairs(
    const std::vector<std::pair<std::string, std::string>> &pairs) {
  std::map<std::string, std::string> parsed;
  for (const auto &p : pairs) {
    if (p.first.empty() || p.second.empty()) return false;
    parsed[p.first] = p.second;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  m_map = std::move(parsed);
  return true;
}

std::string UserChannelMap::lookup(const char *user) const {
  if (user == nullptr || user[0] == '\0') return {};
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_map.find(user);
  return (it == m_map.end()) ? std::string{} : it->second;
}

std::size_t UserChannelMap::size() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_map.size();
}

void UserChannelMap::clear() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_map.clear();
}

/* ----------------------------- ArchiveSender ---------------------------- */

ArchiveSender &ArchiveSender::instance() {
  static ArchiveSender s;
  return s;
}

void ArchiveSender::set_storage(BinlogArchive *storage) { m_storage = storage; }

void ArchiveSender::set_default_channel(const char *channel_name) {
  m_default_channel.assign(channel_name == nullptr ? "" : channel_name);
}

void ArchiveSender::pin_file(const std::string &channel,
                             const std::string &filename) {
  std::lock_guard<std::mutex> lk(m_pin_mutex);
  m_pinned_files[channel].insert(filename);
}

void ArchiveSender::unpin_file(const std::string &channel,
                               const std::string &filename) {
  std::lock_guard<std::mutex> lk(m_pin_mutex);
  auto it = m_pinned_files.find(channel);
  if (it != m_pinned_files.end()) {
    it->second.erase(filename);
    if (it->second.empty()) m_pinned_files.erase(it);
  }
}

bool ArchiveSender::is_file_pinned(const std::string &channel,
                                   const std::string &filename) const {
  std::lock_guard<std::mutex> lk(m_pin_mutex);
  auto it = m_pinned_files.find(channel);
  if (it == m_pinned_files.end()) return false;
  return it->second.count(filename) > 0;
}

std::string ArchiveSender::resolve_channel(const char *replica_user) const {
  if (std::string mapped = m_user_channel_map.lookup(replica_user);
      !mapped.empty()) {
    return mapped;
  }
  return m_default_channel;
}

bool ArchiveSender::handle(MYSQL_THD thd, const char *log_ident,
                           std::uint64_t pos,
                           const char *replica_executed_gtids_text,
                           std::uint32_t flags,
                           std::uint32_t source_server_id,
                           std::uint32_t replica_server_id,
                           enum mysql_binlog_dump_handler_checksum_alg
                               negotiated_checksum_alg,
                           const char *replica_user) {
  if (m_storage == nullptr) return false;

  const char *user = replica_user == nullptr ? "" : replica_user;
  const std::string channel = resolve_channel(user);
  if (channel.empty()) {
    bslog(INFORMATION_LEVEL,
          "binlog_server: dump request from user='%s' "
          "server_id=%u: no channel mapping and no "
          "default_serve_channel configured; falling through to "
          "native Binlog_sender",
          user, static_cast<unsigned>(replica_server_id));
    return false;
  }

  const std::string base_dir =
      m_storage->resolve_channel_base_dir(channel.c_str());
  if (base_dir.empty()) {
    bslog_code(WARNING_LEVEL, ER_BINLOG_SERVER_CONFIG_REJECTED,
               "dump request from user='%s' server_id=%u resolved to "
               "channel '%s' but that channel has no configured storage "
               "(not yet started or configure_channel not called); "
               "falling through",
               user, static_cast<unsigned>(replica_server_id),
               channel.c_str());
    return false;
  }

  StorageBackend *backend =
      m_storage->resolve_channel_backend(channel.c_str());
  if (backend == nullptr) {
    bslog_code(WARNING_LEVEL, ER_BINLOG_SERVER_CONFIG_REJECTED,
               "dump request from user='%s' server_id=%u resolved to "
               "channel '%s' but no storage backend available; "
               "falling through",
               user, static_cast<unsigned>(replica_server_id),
               channel.c_str());
    return false;
  }

  binlog_server::gtid::Gtid_set replica_executed;
  bool gtid_mode = false;
  if (replica_executed_gtids_text != nullptr &&
      replica_executed_gtids_text[0] != '\0') {
    if (!replica_executed.assign_from_text(replica_executed_gtids_text)) {
      bslog_code(WARNING_LEVEL, ER_BINLOG_SERVER_INVARIANT_VIOLATED,
                 "unparseable replica-executed GTID set from user='%s' "
                 "server_id=%u; treating as empty",
                 user, static_cast<unsigned>(replica_server_id));
    } else {
      gtid_mode = !replica_executed.empty();
    }
  }
  const binlog_server::gtid::Gtid_set *excluded_ptr =
      gtid_mode ? &replica_executed : nullptr;

  bslog(INFORMATION_LEVEL,
        "binlog_server: serving dump for user='%s' server_id=%u "
        "channel='%s' base_dir='%s' gtid_mode=%s log_ident='%s' pos=%llu "
        "flags=0x%04x",
        user, static_cast<unsigned>(replica_server_id), channel.c_str(),
        base_dir.c_str(), gtid_mode ? "ON" : "OFF",
        log_ident == nullptr ? "" : log_ident,
        static_cast<unsigned long long>(pos), static_cast<unsigned>(flags));

  ArchiveDumpSession session(
      thd, channel, base_dir, backend, excluded_ptr,
      log_ident == nullptr ? "" : log_ident, pos, flags, source_server_id,
      static_cast<int>(negotiated_checksum_alg), m_trace_send_path);
  const bool rc = session.run();
  bslog(INFORMATION_LEVEL,
        "binlog_server: dump session ended for user='%s' server_id=%u "
        "channel='%s' (result=%s)",
        user, static_cast<unsigned>(replica_server_id), channel.c_str(),
        rc ? "handled" : "error");
  return rc;
}

}  // namespace binlog_server
