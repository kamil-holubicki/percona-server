/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "binlog_archive.h"
#include "file_storage.h"
#include "log_helpers.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include "mysqld_error.h"

namespace fs = std::filesystem;

namespace binlog_server {

namespace {

constexpr char kBinlogMagic[4] = {'\xfe', '\x62', '\x69', '\x6e'};
constexpr unsigned kBinlogMagicSize = 4;
constexpr unsigned kLogEventHeaderLen = 19;
constexpr unsigned kRotatePostHeaderLen = 8;
constexpr unsigned kBinlogChecksumLen = 4;
constexpr unsigned kBinlogChecksumAlgDescLen = 1;

constexpr unsigned kEventTypeOffset = 4;
constexpr unsigned kEventLenOffset = 9;
constexpr unsigned kLogPosOffset = 13;
constexpr unsigned kFlagsOffset = 17;

constexpr unsigned char kLogEventArtificialF = 0x20;
constexpr unsigned char kRotateEventType = 4;
constexpr unsigned char kFormatDescriptionEventType = 15;
constexpr unsigned char kXidEventType = 16;
constexpr unsigned char kXaPrepareEventType = 38;
constexpr unsigned char kChecksumAlgOff = 0;
constexpr unsigned char kChecksumAlgUndef = 255;

uint32_t read_u32_le(const char *p) {
  const auto *u = reinterpret_cast<const unsigned char *>(p);
  return static_cast<uint32_t>(u[0]) |
         (static_cast<uint32_t>(u[1]) << 8) |
         (static_cast<uint32_t>(u[2]) << 16) |
         (static_cast<uint32_t>(u[3]) << 24);
}

uint16_t read_u16_le(const char *p) {
  const auto *u = reinterpret_cast<const unsigned char *>(p);
  return static_cast<uint16_t>(u[0]) |
         (static_cast<uint16_t>(static_cast<uint16_t>(u[1]) << 8));
}

std::string parse_file_uri(const std::string &uri) {
  const std::string prefix = "file://";
  if (uri.compare(0, prefix.size(), prefix) != 0) return "";
  return uri.substr(prefix.size());
}

// Structural binlog filename parser, ported from the reference.
// Parses "<prefix>.NNNNNN" where NNNNNN is >= 6 digits.
// Stops at the end of the digit run, effectively stripping CRC32 bytes.
std::string sanitize_binlog_name(const char *p, size_t max_len) {
  if (p == nullptr || max_len == 0) return {};

  size_t start = 0;
  for (size_t i = 0; i < max_len; ++i) {
    const unsigned char ch = static_cast<unsigned char>(p[i]);
    if (ch < 0x20 || ch >= 0x7f) break;
    if (p[i] == '/' || p[i] == '\\') start = i + 1;
  }

  for (size_t i = start; i < max_len; ++i) {
    const unsigned char ch = static_cast<unsigned char>(p[i]);
    if (ch < 0x20 || ch >= 0x7f) {
      return std::string(p + start, i - start);
    }
    if (p[i] != '.') continue;

    size_t j = i + 1;
    while (j < max_len) {
      const unsigned char d = static_cast<unsigned char>(p[j]);
      if (d < '0' || d > '9') break;
      ++j;
    }
    if (j - (i + 1) >= 6) {
      return std::string(p + start, j - start);
    }
  }

  size_t end = start;
  while (end < max_len) {
    const unsigned char ch = static_cast<unsigned char>(p[end]);
    if (ch < 0x20 || ch >= 0x7f) break;
    ++end;
  }
  return std::string(p + start, end - start);
}

// Read all lines from binlog.index for a channel directory.
std::vector<std::string> read_index_lines(const std::string &base_dir) {
  std::vector<std::string> lines;
  std::string path = base_dir + "binlog.index";
  std::ifstream in(path);
  if (!in.is_open()) return lines;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

void load_index_file(ChannelState &state) {
  if (state.index_loaded) return;
  state.index_loaded = true;
  auto lines = read_index_lines(state.base_dir);
  for (auto &line : lines) {
    if (state.indexed_files.insert(line).second) {
      state.file_order.push_back(line);
    }
  }
}

bool add_to_index(ChannelState &state, const std::string &log_name) {
  load_index_file(state);
  if (state.indexed_files.find(log_name) != state.indexed_files.end()) {
    return true;
  }
  std::string path = state.base_dir + "binlog.index";
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out.is_open()) return false;
  out << log_name << "\n";
  out.flush();
  if (!out.good()) return false;
  state.indexed_files.insert(log_name);
  state.file_order.push_back(log_name);
  return true;
}

std::string read_last_index_entry(const std::string &base_dir) {
  auto lines = read_index_lines(base_dir);
  if (lines.empty()) return "";
  return lines.back();
}

bool sync_channel_to_disk(ChannelState &state, const char *channel_name) {
  if (!state.out.is_open()) return true;
  state.out.flush();
  if (!state.out.good()) return false;
  if (state.fsync_fd < 0) {
    const std::string path = state.base_dir + state.current_log_name;
    state.fsync_fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (state.fsync_fd < 0) {
      bslog_code(WARNING_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
                 "channel '%s' fsync open failed for '%s' (errno=%d: %s)",
                 channel_name, path.c_str(), errno, strerror(errno));
      return false;
    }
  }
  if (::fsync(state.fsync_fd) != 0) {
    bslog_code(WARNING_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
               "channel '%s' fsync failed on '%s%s' (errno=%d: %s)",
               channel_name, state.base_dir.c_str(),
               state.current_log_name.c_str(), errno, strerror(errno));
    return false;
  }
  return true;
}

void close_archive_file(ChannelState &state) {
  if (state.out.is_open()) {
    state.out.flush();
    state.out.close();
  }
  state.out.clear();
  if (state.fsync_fd >= 0) {
    ::close(state.fsync_fd);
    state.fsync_fd = -1;
  }
}

bool open_archive_file(ChannelState &state, const std::string &log_name,
                       const char *channel_name) {
  close_archive_file(state);

  const std::string path = state.base_dir + log_name;
  std::error_code ec;
  std::uintmax_t existing_size = 0;
  if (fs::exists(path, ec)) {
    existing_size = fs::file_size(path, ec);
    if (ec) existing_size = 0;
  }
  const bool file_already_has_content = (existing_size >= kBinlogMagicSize);

  state.out.open(path, std::ios::binary | std::ios::out | std::ios::app);
  if (!state.out.is_open()) {
    bslog_code(ERROR_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
               "failed to open archive file '%s' for channel '%s' "
               "(errno=%d: %s)",
               path.c_str(), channel_name, errno, strerror(errno));
    return false;
  }

  if (!file_already_has_content) {
    state.out.write(kBinlogMagic, kBinlogMagicSize);
    state.out.flush();
    if (!state.out.good()) {
      bslog_code(ERROR_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
                 "failed to write magic to '%s' for channel '%s'",
                 path.c_str(), channel_name);
      state.out.close();
      return false;
    }
    state.wrote_fde = false;
  } else {
    state.wrote_fde = true;
  }

  state.current_log_name = log_name;
  add_to_index(state, log_name);

  bslog(INFORMATION_LEVEL,
        "binlog_server: channel '%s' opened archive file '%s' "
        "(existing_bytes=%llu)",
        channel_name, path.c_str(),
        static_cast<unsigned long long>(existing_size));
  return true;
}

// Crash recovery: walk events in the last archive file, restore watermark,
// truncate partial tail.
void recover_state_from_archive(ChannelState &state,
                                const char *channel_name) {
  load_index_file(state);
  const std::string last_name = read_last_index_entry(state.base_dir);
  if (last_name.empty()) return;

  const std::string path = state.base_dir + last_name;
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) {
    bslog(WARNING_LEVEL,
          "binlog_server: channel '%s' cannot open '%s' for recovery "
          "(errno=%d: %s)",
          channel_name, path.c_str(), errno, strerror(errno));
    return;
  }

  char magic[kBinlogMagicSize];
  if (!f.read(magic, kBinlogMagicSize) ||
      std::memcmp(magic, kBinlogMagic, kBinlogMagicSize) != 0) {
    return;
  }

  uint64_t last_good_offset = kBinlogMagicSize;
  uint64_t watermark = 0;
  bool has_fde = false;
  bool has_checksum = false;

  for (;;) {
    char hdr[kLogEventHeaderLen];
    f.seekg(static_cast<std::streamoff>(last_good_offset), std::ios::beg);
    f.read(hdr, kLogEventHeaderLen);
    if (f.gcount() < static_cast<std::streamsize>(kLogEventHeaderLen)) break;

    const unsigned char event_type =
        static_cast<unsigned char>(hdr[kEventTypeOffset]);
    const uint32_t event_len = read_u32_le(hdr + kEventLenOffset);
    const uint32_t log_pos = read_u32_le(hdr + kLogPosOffset);
    const uint16_t flags = read_u16_le(hdr + kFlagsOffset);

    if (event_len < kLogEventHeaderLen || event_len > (64ULL << 20)) break;

    if (event_type == kFormatDescriptionEventType &&
        event_len >= kLogEventHeaderLen + kBinlogChecksumAlgDescLen +
                         kBinlogChecksumLen) {
      f.seekg(static_cast<std::streamoff>(last_good_offset) +
                  static_cast<std::streamoff>(event_len) -
                  static_cast<std::streamoff>(kBinlogChecksumAlgDescLen) -
                  static_cast<std::streamoff>(kBinlogChecksumLen),
              std::ios::beg);
      char alg_byte = 0;
      if (f.read(&alg_byte, 1) && f.gcount() == 1) {
        const unsigned char alg = static_cast<unsigned char>(alg_byte);
        has_checksum = (alg != kChecksumAlgOff && alg != kChecksumAlgUndef);
      }
    }

    f.clear();
    f.seekg(static_cast<std::streamoff>(last_good_offset) +
                static_cast<std::streamoff>(event_len) - 1,
            std::ios::beg);
    char probe = 0;
    if (!f.read(&probe, 1) || f.gcount() != 1) break;
    f.clear();

    last_good_offset += event_len;
    if (event_type == kFormatDescriptionEventType) has_fde = true;
    if (log_pos > 0 && !(flags & kLogEventArtificialF)) {
      watermark = log_pos;
    }
  }
  f.close();

  std::error_code ec;
  const uintmax_t physical_size = fs::file_size(path, ec);
  if (!ec && physical_size > last_good_offset) {
    fs::resize_file(path, last_good_offset, ec);
    if (!ec) {
      bslog(WARNING_LEVEL,
            "binlog_server: channel '%s' truncated partial tail of '%s' "
            "from %llu to %llu (recovered after crash)",
            channel_name, path.c_str(),
            static_cast<unsigned long long>(physical_size),
            static_cast<unsigned long long>(last_good_offset));
    }
  }

  state.current_log_name = last_name;
  state.last_source_log_pos = watermark;
  state.wrote_fde = has_fde;
  state.has_checksum = has_checksum;

  bslog(INFORMATION_LEVEL,
        "binlog_server: channel '%s' recovered state from '%s' "
        "(file_size=%llu, watermark=%llu, has_fde=%d, has_checksum=%d)",
        channel_name, last_name.c_str(),
        static_cast<unsigned long long>(last_good_offset),
        static_cast<unsigned long long>(watermark),
        static_cast<int>(has_fde), static_cast<int>(has_checksum));
}

void handle_rotate_event(ChannelState &state, const char *channel_name,
                         const char *event_buf, unsigned long event_len) {
  if (event_len < kLogEventHeaderLen + kRotatePostHeaderLen) return;

  const uint16_t flags = read_u16_le(event_buf + kFlagsOffset);
  const bool is_artificial = (flags & kLogEventArtificialF) != 0;
  const unsigned long name_off = kLogEventHeaderLen + kRotatePostHeaderLen;

  const std::string new_name =
      sanitize_binlog_name(event_buf + name_off, event_len - name_off);
  if (new_name.empty()) return;

  // Same-file artificial rotate on reconnect: preserve watermark.
  if (is_artificial && new_name == state.current_log_name) {
    return;
  }

  // Real rotate: write it to the outgoing file before switching.
  if (!is_artificial && state.out.is_open()) {
    state.out.write(event_buf, event_len);
    state.out.flush();
    sync_channel_to_disk(state, channel_name);
  }

  close_archive_file(state);
  state.current_log_name = new_name;
  state.wrote_fde = false;
  state.last_source_log_pos = 0;

  bslog(INFORMATION_LEVEL,
        "binlog_server: channel '%s' rotate %s -> source binlog '%s'",
        channel_name, is_artificial ? "(synthetic)" : "(real)",
        new_name.c_str());
}

void handle_format_description_event(ChannelState &state,
                                     const char *channel_name,
                                     const char *event_buf,
                                     unsigned long event_len) {
  if (event_len >=
      kLogEventHeaderLen + kBinlogChecksumAlgDescLen + kBinlogChecksumLen) {
    const unsigned char alg = static_cast<unsigned char>(
        event_buf[event_len - kBinlogChecksumAlgDescLen - kBinlogChecksumLen]);
    state.has_checksum = (alg != kChecksumAlgOff && alg != kChecksumAlgUndef);
  } else {
    state.has_checksum = false;
  }

  if (state.current_log_name.empty()) {
    state.current_log_name = "binlog.unknown";
    bslog(WARNING_LEVEL,
          "binlog_server: no source binlog name seen before FDE on "
          "channel '%s'; using '%s'",
          channel_name, state.current_log_name.c_str());
  }

  if (!state.out.is_open()) {
    if (!open_archive_file(state, state.current_log_name, channel_name)) {
      return;
    }
  }

  if (state.wrote_fde) {
    return;
  }

  state.out.write(event_buf, event_len);
  state.out.flush();
  if (!state.out.good()) {
    bslog(WARNING_LEVEL,
          "binlog_server: failed to write FDE for channel '%s' to '%s%s'",
          channel_name, state.base_dir.c_str(),
          state.current_log_name.c_str());
    return;
  }

  state.wrote_fde = true;
  ++state.events_appended;
  state.bytes_appended += event_len;

  const uint32_t fde_log_pos = read_u32_le(event_buf + kLogPosOffset);
  if (fde_log_pos > state.last_source_log_pos) {
    state.last_source_log_pos = fde_log_pos;
  }
}

}  // anonymous namespace

BinlogArchive::BinlogArchive() = default;
BinlogArchive::~BinlogArchive() = default;

void BinlogArchive::set_default_storage_uri(const std::string &uri) {
  std::lock_guard<std::mutex> lk(m_uri_mutex);
  m_default_storage_uri = uri;
}

std::string BinlogArchive::get_default_storage_uri() const {
  std::lock_guard<std::mutex> lk(m_uri_mutex);
  return m_default_storage_uri;
}

std::string BinlogArchive::resolve_channel_base_dir(
    const char *channel_name) const {
  if (channel_name == nullptr || channel_name[0] == '\0') return {};
  std::lock_guard<std::mutex> lk(m_registry_mutex);
  auto it = m_channels.find(channel_name);
  if (it == m_channels.end()) return {};
  std::lock_guard<std::mutex> io_lk(it->second->io_mutex);
  return it->second->base_dir;
}

std::string BinlogArchive::effective_uri(const std::string &channel_uri) {
  if (!channel_uri.empty()) return channel_uri;
  std::lock_guard<std::mutex> lk(m_uri_mutex);
  return m_default_storage_uri;
}

std::unique_ptr<StorageBackend> BinlogArchive::create_backend(
    const std::string &uri) {
  std::string path = parse_file_uri(uri);
  if (path.empty()) {
    bslog(ERROR_LEVEL,
          "binlog_server: unsupported or invalid storage URI: '%s'",
          uri.c_str());
    return nullptr;
  }
  return std::make_unique<FileStorage>(path);
}

std::shared_ptr<ChannelState> BinlogArchive::find_channel(
    const std::string &name) {
  std::lock_guard<std::mutex> lk(m_registry_mutex);
  auto it = m_channels.find(name);
  if (it == m_channels.end()) return nullptr;
  return it->second;
}

int BinlogArchive::configure_channel(const char *channel_name, int enabled,
                                     const char *storage_uri) {
  std::string name = channel_name != nullptr ? channel_name : "";
  std::string uri_str = storage_uri != nullptr ? storage_uri : "";
  std::string eff_uri = effective_uri(uri_str);

  std::lock_guard<std::mutex> lk(m_registry_mutex);
  auto &cs_ptr = m_channels[name];

  if (!cs_ptr) {
    cs_ptr = std::make_shared<ChannelState>();
    cs_ptr->name = name;
  }

  auto &cs = *cs_ptr;
  std::lock_guard<std::mutex> io_lk(cs.io_mutex);

  cs.enabled = (enabled != 0);
  cs.storage_uri = uri_str;

  if (!cs.enabled) {
    close_archive_file(cs);
    cs.backend = nullptr;
    return 0;
  }

  if (eff_uri.empty()) {
    bslog_code(ERROR_LEVEL, ER_BINLOG_SERVER_CONFIG_REJECTED,
               "channel '%s' enabled but no storage URI configured",
               name.c_str());
    return 1;
  }

  std::string path = parse_file_uri(eff_uri);
  if (path.empty()) {
    bslog_code(ERROR_LEVEL, ER_BINLOG_SERVER_CONFIG_REJECTED,
               "invalid URI '%s' for channel '%s'",
               eff_uri.c_str(), name.c_str());
    return 1;
  }
  if (!path.empty() && path.back() != '/') path += '/';

  // Sanitize channel name for directory (defense-in-depth against traversal)
  std::string dir_name;
  if (name.empty()) {
    dir_name = "_default";
  } else {
    dir_name.reserve(name.size());
    for (char ch : name) {
      if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-')
        dir_name.push_back(ch);
      else
        dir_name.push_back('_');
    }
    if (dir_name == "." || dir_name == "..") {
      dir_name = "_" + dir_name + "_";
    }
  }

  std::string base_dir = path + dir_name + "/";
  std::error_code ec;
  fs::create_directories(base_dir, ec);
  if (ec) {
    bslog_code(ERROR_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
               "cannot create dir '%s' for channel '%s': %s",
               base_dir.c_str(), name.c_str(), ec.message().c_str());
    return 1;
  }

  // Detect base_dir change: close old file and reset archive state.
  if (!cs.base_dir.empty() && cs.base_dir != base_dir) {
    close_archive_file(cs);
    cs.current_log_name.clear();
    cs.last_source_log_pos = 0;
    cs.wrote_fde = false;
    cs.has_checksum = false;
    cs.events_appended = 0;
    cs.bytes_appended = 0;
    cs.index_loaded = false;
    cs.indexed_files.clear();
    cs.file_order.clear();
    bslog(INFORMATION_LEVEL,
          "binlog_server: channel '%s' storage URI changed, reset state",
          name.c_str());
  }

  cs.base_dir = base_dir;
  if (!cs.backend) {
    auto be = create_backend(eff_uri);
    if (be) {
      m_backends.push_back(std::move(be));
      cs.backend = m_backends.back().get();
    }
  }

  // Crash recovery: restore state from existing archive.
  if (cs.current_log_name.empty()) {
    recover_state_from_archive(cs, name.c_str());
  }

  bslog(INFORMATION_LEVEL,
        "binlog_server: channel '%s' configured (uri='%s')", name.c_str(),
        eff_uri.c_str());
  return 0;
}

int BinlogArchive::append_event(const char *channel_name,
                                const char *event_buf,
                                unsigned long event_len) {
  if (event_buf == nullptr || event_len < kLogEventHeaderLen) return 0;

  std::string name = channel_name != nullptr ? channel_name : "";
  auto cs_ptr = find_channel(name);
  if (!cs_ptr) return 0;

  auto &cs = *cs_ptr;
  std::lock_guard<std::mutex> lk(cs.io_mutex);

  if (!cs.enabled) return 0;

  const unsigned char event_type =
      static_cast<unsigned char>(event_buf[kEventTypeOffset]);
  const uint32_t hdr_event_len = read_u32_le(event_buf + kEventLenOffset);
  const uint32_t log_pos = read_u32_le(event_buf + kLogPosOffset);
  const uint16_t flags = read_u16_le(event_buf + kFlagsOffset);
  const bool is_artificial = (flags & kLogEventArtificialF) != 0;

  (void)hdr_event_len;

  switch (event_type) {
    case kRotateEventType:
      handle_rotate_event(cs, name.c_str(), event_buf, event_len);
      return 0;
    case kFormatDescriptionEventType:
      handle_format_description_event(cs, name.c_str(), event_buf, event_len);
      return 0;
    default:
      break;
  }

  if (is_artificial) return 0;
  if (!cs.wrote_fde) return 0;
  if (!cs.out.is_open()) return 0;

  // Deduplication: skip events already archived (watermark-based).
  if (log_pos != 0 && log_pos <= cs.last_source_log_pos) return 0;

  cs.out.write(event_buf, event_len);
  if (!cs.out.good()) {
    bslog_code(ERROR_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
               "write error on channel '%s', file '%s%s'",
               name.c_str(), cs.base_dir.c_str(),
               cs.current_log_name.c_str());
    return 1;
  }

  if (log_pos > 0) cs.last_source_log_pos = log_pos;
  ++cs.events_appended;
  cs.bytes_appended += event_len;

  // Fsync at transaction boundaries (Xid / XA_prepare).
  if (event_type == kXidEventType || event_type == kXaPrepareEventType) {
    sync_channel_to_disk(cs, name.c_str());
  }

  if (cs.events_appended == 1 || cs.events_appended % 10000 == 0) {
    bslog(INFORMATION_LEVEL,
          "binlog_server: channel '%s' stored %lu events (%llu bytes) into "
          "'%s%s' (log_pos=%llu)",
          name.c_str(), static_cast<unsigned long>(cs.events_appended),
          static_cast<unsigned long long>(cs.bytes_appended),
          cs.base_dir.c_str(), cs.current_log_name.c_str(),
          static_cast<unsigned long long>(cs.last_source_log_pos));
  }
  return 0;
}

int BinlogArchive::close_channel(const char *channel_name) {
  std::string name = channel_name != nullptr ? channel_name : "";
  auto cs_ptr = find_channel(name);
  if (!cs_ptr) return 0;

  std::lock_guard<std::mutex> lk(cs_ptr->io_mutex);

  if (!cs_ptr->out.is_open()) return 0;

  sync_channel_to_disk(*cs_ptr, name.c_str());
  close_archive_file(*cs_ptr);

  // Preserve wrote_fde so reconnect to same file won't re-write FDE.
  cs_ptr->wrote_fde = !cs_ptr->current_log_name.empty();

  bslog(INFORMATION_LEVEL,
        "binlog_server: closed archive for channel '%s' "
        "(file='%s%s', events=%lu, bytes=%llu, watermark=%llu)",
        name.c_str(), cs_ptr->base_dir.c_str(),
        cs_ptr->current_log_name.c_str(),
        static_cast<unsigned long>(cs_ptr->events_appended),
        static_cast<unsigned long long>(cs_ptr->bytes_appended),
        static_cast<unsigned long long>(cs_ptr->last_source_log_pos));
  return 0;
}

int BinlogArchive::reset_channel(const char *channel_name) {
  std::string name = channel_name != nullptr ? channel_name : "";
  auto cs_ptr = find_channel(name);
  if (!cs_ptr) return 0;

  std::lock_guard<std::mutex> lk(cs_ptr->io_mutex);

  close_archive_file(*cs_ptr);

  if (!cs_ptr->base_dir.empty()) {
    std::error_code ec;
    fs::remove_all(cs_ptr->base_dir, ec);
    fs::create_directories(cs_ptr->base_dir, ec);
  }

  cs_ptr->current_log_name.clear();
  cs_ptr->last_source_log_pos = 0;
  cs_ptr->wrote_fde = false;
  cs_ptr->has_checksum = false;
  cs_ptr->events_appended = 0;
  cs_ptr->bytes_appended = 0;
  cs_ptr->index_loaded = false;
  cs_ptr->indexed_files.clear();
  cs_ptr->file_order.clear();
  return 0;
}

int BinlogArchive::flush_channel(const char *channel_name) {
  std::string name = channel_name != nullptr ? channel_name : "";
  auto cs_ptr = find_channel(name);
  if (!cs_ptr) return 0;

  std::lock_guard<std::mutex> lk(cs_ptr->io_mutex);
  sync_channel_to_disk(*cs_ptr, name.c_str());
  return 0;
}

int BinlogArchive::rotate_channel(const char *channel_name,
                                  const char *new_log_name) {
  if (new_log_name == nullptr) return 1;
  std::string name = channel_name != nullptr ? channel_name : "";
  auto cs_ptr = find_channel(name);
  if (!cs_ptr) return 0;

  std::lock_guard<std::mutex> lk(cs_ptr->io_mutex);

  sync_channel_to_disk(*cs_ptr, name.c_str());
  close_archive_file(*cs_ptr);

  cs_ptr->current_log_name = new_log_name;
  cs_ptr->wrote_fde = false;
  cs_ptr->last_source_log_pos = 0;
  return 0;
}

}  // namespace binlog_server
