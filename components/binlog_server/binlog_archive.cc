/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "archive_sender.h"
#include "binlog_archive.h"
#include "encryption.h"
#include "file_storage.h"
#include "gtid_set.h"
#include "log_helpers.h"
#include "s3_storage.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

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
constexpr unsigned char kPreviousGtidsLogEventType = 35;
constexpr unsigned char kGtidLogEventType = 33;
constexpr unsigned char kAnonymousGtidLogEventType = 34;
constexpr unsigned char kGtidTaggedLogEventType = 42;
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

std::string sanitize_channel_dir_name(const std::string &channel_name) {
  if (channel_name.empty()) return "_default";
  std::string result;
  result.reserve(channel_name.size());
  for (char ch : channel_name) {
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
      result += ch;
    } else if (ch == '.') {
      result += '_';
    } else {
      result += '_';
    }
  }
  if (result == "." || result == ".." || result.empty()) return "_sanitized";
  while (!result.empty() && result[0] == '.') result[0] = '_';
  return result;
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

  std::vector<std::string> lines;
  if (state.backend) {
    state.backend->index_load(state.base_dir, lines);
  } else {
    lines = read_index_lines(state.base_dir);
  }
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

  bool ok = false;
  if (state.backend) {
    ok = state.backend->index_append(state.base_dir, log_name);
  } else {
    std::string path = state.base_dir + "binlog.index";
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out.is_open()) return false;
    out << log_name << "\n";
    out.flush();
    ok = out.good();
  }
  if (!ok) return false;
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
  if (!state.out) return true;
  if (!state.out->sync()) {
    bslog_code(WARNING_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
               "channel '%s' sync failed on '%s%s'", channel_name,
               state.base_dir.c_str(), state.current_log_name.c_str());
    return false;
  }
  return true;
}

void close_archive_file(ChannelState &state) {
  if (state.out) {
    state.out->flush();
    state.out->close();
    state.out.reset();
  }
  state.encryptor.reset();
}

// Set up encryptor for an existing encrypted file (on re-open / append).
static bool setup_encryptor_for_existing(ChannelState &state,
                                         const std::string &log_name,
                                         uint64_t file_size) {
  if (!state.backend) return false;
  auto reader = state.backend->open_read(state.base_dir, log_name);
  if (!reader) return false;

  unsigned char hdr_buf[kEncryptionHeaderSize];
  if (!reader->read_at(0, hdr_buf, kEncryptionHeaderSize)) return false;
  reader->close();

  if (!EncryptionHeader::is_encrypted(hdr_buf)) return false;

  EncryptionHeader hdr;
  if (!hdr.deserialize(hdr_buf)) return false;

  unsigned char password[kFilePasswordLen];
  if (!decrypt_file_password(hdr.key_id, hdr.encrypted_password, hdr.iv,
                             password)) {
    OPENSSL_cleanse(password, sizeof(password));
    return false;
  }

  state.encryptor = std::make_unique<AesCtrCipher>();
  if (!state.encryptor->open(password, kFilePasswordLen)) {
    OPENSSL_cleanse(password, sizeof(password));
    state.encryptor.reset();
    return false;
  }
  OPENSSL_cleanse(password, sizeof(password));

  // Position encryptor at end of file body (after header)
  uint64_t body_offset = file_size - kEncryptionHeaderSize;
  state.encryptor->set_offset(body_offset);
  return true;
}

// Set up encryption for a brand new file.
static bool setup_encryption_new_file(ChannelState &state,
                                      const std::string &path,
                                      const char *channel_name) {
  if (!encryption_keyring_available()) {
    bslog(ERROR_LEVEL,
          "binlog_server: encryption enabled but keyring service not "
          "available for channel '%s'",
          channel_name);
    return false;
  }

  std::string key_id = current_master_key_id(channel_name);
  if (key_id.empty()) {
    key_id = generate_master_key(channel_name);
    if (key_id.empty()) {
      bslog(ERROR_LEVEL,
            "binlog_server: failed to generate master encryption key for "
            "channel '%s'",
            channel_name);
      return false;
    }
  }

  // Generate random per-file password
  unsigned char password[kFilePasswordLen];
  if (RAND_bytes(password, kFilePasswordLen) != 1) {
    bslog(ERROR_LEVEL,
          "binlog_server: failed to generate random file password for '%s'",
          path.c_str());
    return false;
  }

  // Encrypt file password with master key
  EncryptionHeader hdr;
  hdr.key_id = key_id;
  if (!encrypt_file_password(key_id, password, hdr.encrypted_password,
                             hdr.iv)) {
    OPENSSL_cleanse(password, sizeof(password));
    bslog(ERROR_LEVEL,
          "binlog_server: failed to encrypt file password for '%s'",
          path.c_str());
    return false;
  }

  // Write 512-byte header
  unsigned char hdr_buf[kEncryptionHeaderSize];
  if (!hdr.serialize(hdr_buf)) {
    OPENSSL_cleanse(password, sizeof(password));
    return false;
  }

  if (!state.out->write(hdr_buf, kEncryptionHeaderSize) ||
      !state.out->flush() || !state.out->good()) {
    OPENSSL_cleanse(password, sizeof(password));
    return false;
  }

  // Initialize encryptor
  state.encryptor = std::make_unique<AesCtrCipher>();
  if (!state.encryptor->open(password, kFilePasswordLen)) {
    OPENSSL_cleanse(password, sizeof(password));
    state.encryptor.reset();
    return false;
  }
  OPENSSL_cleanse(password, sizeof(password));
  return true;
}

bool open_archive_file(ChannelState &state, const std::string &log_name,
                       const char *channel_name, bool encrypt = false) {
  close_archive_file(state);

  uint64_t existing_size = 0;
  if (state.backend) {
    existing_size = state.backend->file_size(state.base_dir, log_name);
  } else {
    const std::string path = state.base_dir + log_name;
    std::error_code ec;
    if (fs::exists(path, ec)) {
      existing_size = fs::file_size(path, ec);
      if (ec) existing_size = 0;
    }
  }
  const bool file_already_has_content = (existing_size >= kBinlogMagicSize);

  if (state.backend) {
    state.out = state.backend->open_write(state.base_dir, log_name);
  }
  if (!state.out) {
    bslog_code(ERROR_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
               "failed to open archive file '%s%s' for channel '%s'",
               state.base_dir.c_str(), log_name.c_str(), channel_name);
    return false;
  }

  if (!file_already_has_content) {
    if (encrypt) {
      if (!setup_encryption_new_file(state, state.base_dir + log_name,
                                     channel_name)) {
        bslog(ERROR_LEVEL,
              "binlog_server: encryption setup failed for '%s%s'; "
              "falling back to plaintext",
              state.base_dir.c_str(), log_name.c_str());
      }
    }
    {
      unsigned char magic[kBinlogMagicSize];
      std::memcpy(magic, kBinlogMagic, kBinlogMagicSize);
      if (state.encryptor) {
        state.encryptor->process(magic, kBinlogMagicSize);
      }
      state.out->write(magic, kBinlogMagicSize);
    }
    if (!state.out->flush() || !state.out->good()) {
      bslog_code(ERROR_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
                 "failed to write header to '%s%s' for channel '%s'",
                 state.base_dir.c_str(), log_name.c_str(), channel_name);
      state.out->close();
      state.out.reset();
      return false;
    }
    state.wrote_fde = false;
  } else {
    if (existing_size >= kEncryptionHeaderSize) {
      setup_encryptor_for_existing(state, log_name, existing_size);
    }
    state.wrote_fde = true;
  }

  state.current_log_name = log_name;
  add_to_index(state, log_name);

  bslog(INFORMATION_LEVEL,
        "binlog_server: channel '%s' opened archive file '%s%s' "
        "(existing_bytes=%llu, encrypted=%s)",
        channel_name, state.base_dir.c_str(), log_name.c_str(),
        static_cast<unsigned long long>(existing_size),
        state.encryptor ? "yes" : "no");
  return true;
}

// Forward declarations for metadata helpers defined below
void load_file_metadata(ChannelState &state);
void flush_file_metadata(ChannelState &state, const std::string &log_name);
void note_event_timestamp(ChannelState &state, const char *event_buf);
void note_gtid_event(ChannelState &state, const char *event_buf,
                     unsigned long event_len);
void record_channel_error(ChannelState &state, int code,
                          const std::string &msg);

// Crash recovery: walk events in the last archive file, restore watermark,
// truncate to transaction boundary (not just event boundary), and restore
// GTID metadata from sidecar.
void recover_state_from_archive(ChannelState &state,
                                const char *channel_name) {
  load_index_file(state);

  std::string last_name;
  if (state.backend) {
    std::vector<std::string> entries;
    state.backend->index_load(state.base_dir, entries);
    if (!entries.empty()) last_name = entries.back();
  } else {
    last_name = read_last_index_entry(state.base_dir);
  }
  if (last_name.empty()) return;

  if (!state.backend) return;
  auto reader = state.backend->open_read(state.base_dir, last_name);
  if (!reader) {
    bslog(WARNING_LEVEL,
          "binlog_server: channel '%s' cannot open '%s' for recovery",
          channel_name, last_name.c_str());
    return;
  }

  const uint64_t file_sz = reader->size();

  // Detect encryption: check first bytes for encryption magic
  unsigned char first_bytes[kEncryptionHeaderSize];
  size_t probe_len = (file_sz >= kEncryptionHeaderSize)
                         ? kEncryptionHeaderSize
                         : static_cast<size_t>(file_sz);
  if (probe_len < kBinlogMagicSize) return;
  if (!reader->read_at(0, first_bytes, probe_len)) return;

  std::unique_ptr<AesCtrCipher> decryptor;
  uint64_t body_offset = 0;

  if (EncryptionHeader::is_encrypted(first_bytes)) {
    if (probe_len < kEncryptionHeaderSize) {
      bslog(WARNING_LEVEL,
            "binlog_server: channel '%s' encrypted file '%s' too short for "
            "header",
            channel_name, last_name.c_str());
      return;
    }
    EncryptionHeader hdr;
    if (!hdr.deserialize(first_bytes)) {
      bslog(WARNING_LEVEL,
            "binlog_server: channel '%s' failed to parse encryption header "
            "in '%s'",
            channel_name, last_name.c_str());
      return;
    }
    unsigned char password[kFilePasswordLen];
    if (!decrypt_file_password(hdr.key_id, hdr.encrypted_password, hdr.iv,
                               password)) {
      bslog(WARNING_LEVEL,
            "binlog_server: channel '%s' failed to decrypt file password "
            "for '%s' (key='%s')",
            channel_name, last_name.c_str(), hdr.key_id.c_str());
      OPENSSL_cleanse(password, sizeof(password));
      return;
    }
    decryptor = std::make_unique<AesCtrCipher>();
    if (!decryptor->open(password, kFilePasswordLen)) {
      OPENSSL_cleanse(password, sizeof(password));
      return;
    }
    OPENSSL_cleanse(password, sizeof(password));
    body_offset = kEncryptionHeaderSize;
  } else if (std::memcmp(first_bytes, kBinlogMagic, kBinlogMagicSize) == 0) {
    body_offset = 0;
  } else {
    return;
  }

  auto read_decrypted = [&](uint64_t logical_off, char *buf,
                            size_t len) -> bool {
    uint64_t phys_off = body_offset + logical_off;
    if (!reader->read_at(phys_off, reinterpret_cast<unsigned char *>(buf), len))
      return false;
    if (decryptor) {
      decryptor->set_offset(logical_off);
      if (!decryptor->process(reinterpret_cast<unsigned char *>(buf), len))
        return false;
    }
    return true;
  };

  // Verify binlog magic (first 4 bytes of body)
  char magic[kBinlogMagicSize];
  if (!read_decrypted(0, magic, kBinlogMagicSize) ||
      std::memcmp(magic, kBinlogMagic, kBinlogMagicSize) != 0) {
    return;
  }

  uint64_t last_good_offset = kBinlogMagicSize;
  uint64_t last_safe_offset = kBinlogMagicSize;
  uint64_t watermark = 0;
  uint64_t safe_watermark = 0;
  bool has_fde = false;
  bool has_checksum = false;

  for (;;) {
    char hdr[kLogEventHeaderLen];
    if (!read_decrypted(last_good_offset, hdr, kLogEventHeaderLen)) break;

    const unsigned char event_type =
        static_cast<unsigned char>(hdr[kEventTypeOffset]);
    const uint32_t event_len = read_u32_le(hdr + kEventLenOffset);
    const uint32_t log_pos = read_u32_le(hdr + kLogPosOffset);
    const uint16_t flags = read_u16_le(hdr + kFlagsOffset);

    if (event_len < kLogEventHeaderLen || event_len > (64ULL << 20)) break;

    if (event_type == kFormatDescriptionEventType &&
        event_len >= kLogEventHeaderLen + kBinlogChecksumAlgDescLen +
                         kBinlogChecksumLen) {
      uint64_t alg_off = last_good_offset + event_len -
                         kBinlogChecksumAlgDescLen - kBinlogChecksumLen;
      char alg_byte = 0;
      if (read_decrypted(alg_off, &alg_byte, 1)) {
        const unsigned char alg = static_cast<unsigned char>(alg_byte);
        has_checksum = (alg != kChecksumAlgOff && alg != kChecksumAlgUndef);
      }
    }

    // Verify last byte of event is accessible (completeness check)
    char probe = 0;
    if (!read_decrypted(last_good_offset + event_len - 1, &probe, 1)) break;

    last_good_offset += event_len;
    if (event_type == kFormatDescriptionEventType) has_fde = true;
    if (log_pos > 0 && !(flags & kLogEventArtificialF)) {
      watermark = log_pos;
    }

    if (event_type == kFormatDescriptionEventType ||
        event_type == kPreviousGtidsLogEventType ||
        event_type == kXidEventType ||
        event_type == kXaPrepareEventType) {
      last_safe_offset = last_good_offset;
      safe_watermark = watermark;
    }
  }
  reader->close();

  // Truncate to transaction boundary (physical offset includes header)
  const uint64_t truncate_to = body_offset + last_safe_offset;
  if (file_sz > truncate_to) {
    if (state.backend->truncate_file(state.base_dir, last_name, truncate_to)) {
      if (last_safe_offset < last_good_offset) {
        bslog(WARNING_LEVEL,
              "binlog_server: channel '%s' truncated '%s' from %llu to %llu "
              "(discarded %llu bytes of incomplete transaction after crash)",
              channel_name, last_name.c_str(),
              static_cast<unsigned long long>(file_sz),
              static_cast<unsigned long long>(truncate_to),
              static_cast<unsigned long long>(last_good_offset -
                                             last_safe_offset));
      } else {
        bslog(WARNING_LEVEL,
              "binlog_server: channel '%s' truncated partial tail of '%s' "
              "from %llu to %llu (recovered after crash)",
              channel_name, last_name.c_str(),
              static_cast<unsigned long long>(file_sz),
              static_cast<unsigned long long>(truncate_to));
      }
    }
  }

  state.current_log_name = last_name;
  state.last_source_log_pos = safe_watermark;
  state.wrote_fde = has_fde;
  state.has_checksum = has_checksum;

  load_file_metadata(state);

  auto meta_it = state.file_metadata.find(last_name);
  if (meta_it != state.file_metadata.end() &&
      !meta_it->second.last_gtid_set.empty()) {
    bslog(INFORMATION_LEVEL,
          "binlog_server: channel '%s' restored GTID state from sidecar: %s",
          channel_name, meta_it->second.last_gtid_set.c_str());
  }

  bslog(INFORMATION_LEVEL,
        "binlog_server: channel '%s' recovered state from '%s' "
        "(truncated_to=%llu, watermark=%llu, has_fde=%d, has_checksum=%d, "
        "discarded_partial_txn=%s, encrypted=%s)",
        channel_name, last_name.c_str(),
        static_cast<unsigned long long>(truncate_to),
        static_cast<unsigned long long>(safe_watermark),
        static_cast<int>(has_fde), static_cast<int>(has_checksum),
        (last_safe_offset < last_good_offset) ? "yes" : "no",
        decryptor ? "yes" : "no");
}

// Write event data to archive, encrypting in-place if encryptor is active.
bool write_event_data(ChannelState &state, const char *event_buf,
                      unsigned long event_len) {
  if (!state.out) return false;
  if (state.encryptor) {
    std::vector<unsigned char> buf(
        reinterpret_cast<const unsigned char *>(event_buf),
        reinterpret_cast<const unsigned char *>(event_buf) + event_len);
    if (!state.encryptor->process(buf.data(), buf.size())) return false;
    return state.out->write(buf.data(), event_len);
  }
  return state.out->write(reinterpret_cast<const unsigned char *>(event_buf),
                          event_len);
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
  if (!is_artificial && state.out) {
    write_event_data(state, event_buf, event_len);
    state.out->flush();
    sync_channel_to_disk(state, channel_name);
  }

  // Flush per-file metadata sidecar for the file being rotated away
  if (!state.current_log_name.empty()) {
    auto &fm = state.file_metadata[state.current_log_name];
    std::error_code ec;
    auto sz = fs::file_size(state.base_dir + state.current_log_name, ec);
    if (!ec) fm.size_bytes = sz;
    flush_file_metadata(state, state.current_log_name);
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
                                     unsigned long event_len,
                                     bool encrypt = false) {
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

  if (!state.out) {
    if (!open_archive_file(state, state.current_log_name, channel_name,
                           encrypt)) {
      return;
    }
  }

  if (state.wrote_fde) {
    return;
  }

  if (!write_event_data(state, event_buf, event_len)) {
    bslog(WARNING_LEVEL,
          "binlog_server: failed to write FDE for channel '%s' to '%s%s'",
          channel_name, state.base_dir.c_str(),
          state.current_log_name.c_str());
    return;
  }
  state.out->flush();

  state.wrote_fde = true;
  ++state.events_appended;
  state.bytes_appended += event_len;

  const uint32_t fde_log_pos = read_u32_le(event_buf + kLogPosOffset);
  if (fde_log_pos > state.last_source_log_pos) {
    state.last_source_log_pos = fde_log_pos;
  }
}

// --- Per-file metadata helpers (Phase 4) ---

uint64_t now_microseconds() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(system_clock::now().time_since_epoch())
          .count());
}

bool should_log_io_failure(ChannelState &state) {
  const unsigned long count = ++state.io_failure_count;
  return count <= 5 || count % 10000 == 0;
}

void record_channel_error(ChannelState &state, int code,
                          const std::string &msg) {
  state.last_error_code = code;
  state.last_error_message = msg;
  state.last_error_timestamp_us = now_microseconds();
  ++state.write_errors;
}

void note_event_timestamp(ChannelState &state, const char *event_buf) {
  const uint32_t ts = read_u32_le(event_buf);
  if (ts == 0) return;

  state.last_event_timestamp_us = now_microseconds();

  auto &fm = state.file_metadata[state.current_log_name];
  if (fm.min_event_timestamp == 0 || ts < fm.min_event_timestamp)
    fm.min_event_timestamp = ts;
  if (ts > fm.max_event_timestamp) fm.max_event_timestamp = ts;
  ++fm.event_count;
}

// Extract GTID text from a Gtid_log_event payload.
// Layout: flags(1) + uuid(16) + gno(8) [+ ts_type(1) + ...].
// We format as "uuid:gno".
std::string extract_gtid_text(const char *event_buf, unsigned long event_len) {
  constexpr unsigned kGtidPostHeaderLen = 42;
  if (event_len < kLogEventHeaderLen + kGtidPostHeaderLen) return {};
  const unsigned char *p =
      reinterpret_cast<const unsigned char *>(event_buf) + kLogEventHeaderLen;
  // p[0] = commit_flag, p[1..16] = uuid, p[17..24] = gno (LE 8 bytes)
  const unsigned char *uuid = p + 1;
  char buf[64];
  std::snprintf(buf, sizeof(buf),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
                "%02x%02x-%02x%02x%02x%02x%02x%02x",
                uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5],
                uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11],
                uuid[12], uuid[13], uuid[14], uuid[15]);
  uint64_t gno = 0;
  for (int i = 0; i < 8; ++i)
    gno |= static_cast<uint64_t>(p[17 + i]) << (i * 8);
  char result[128];
  std::snprintf(result, sizeof(result), "%s:%llu", buf,
                static_cast<unsigned long long>(gno));
  return result;
}

// Format a 16-byte UUID into canonical "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
std::string format_uuid(const unsigned char *uuid) {
  char buf[37];
  std::snprintf(buf, sizeof(buf),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
                "%02x%02x-%02x%02x%02x%02x%02x%02x",
                uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5],
                uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11],
                uuid[12], uuid[13], uuid[14], uuid[15]);
  return buf;
}

uint64_t read_u64_le(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (i * 8);
  return v;
}

// Extract Previous_gtids_log_event text. The event payload is either:
// - FLAG_ENCODING (flag byte 0x01): remaining bytes are raw GTID text
// - SID_ENCODING (flag byte 0x00): binary n_sids + (uuid + n_intervals +
//   intervals[]) pairs
std::string extract_previous_gtids_text(const char *event_buf,
                                        unsigned long event_len,
                                        bool has_checksum) {
  const unsigned overhead =
      kLogEventHeaderLen + (has_checksum ? kBinlogChecksumLen : 0);
  if (event_len <= overhead) return {};
  const unsigned char *p =
      reinterpret_cast<const unsigned char *>(event_buf) + kLogEventHeaderLen;
  const unsigned payload_len = event_len - overhead;
  if (payload_len < 1) return {};

  // Binary SID-encoding: n_sids (8 bytes LE) + for each SID: uuid (16) +
  // n_intervals (8) + intervals (n_intervals * 16 bytes: start(8) + end(8))
  const unsigned char *cursor = p;
  const unsigned char *end_ptr = p + payload_len;

  if (payload_len < 8) return {};

  uint64_t n_sids = read_u64_le(cursor);
  cursor += 8;

  if (n_sids == 0) return {};
  if (n_sids > 100000) return {};  // sanity limit

  std::string result;
  for (uint64_t s = 0; s < n_sids; ++s) {
    if (cursor + 16 + 8 > end_ptr) break;
    std::string uuid_str = format_uuid(cursor);
    cursor += 16;
    uint64_t n_intervals = read_u64_le(cursor);
    cursor += 8;
    if (n_intervals == 0 || n_intervals > 1000000) break;
    if (cursor + n_intervals * 16 > end_ptr) break;

    if (!result.empty()) result += ',';
    result += uuid_str;
    result += ':';

    for (uint64_t i = 0; i < n_intervals; ++i) {
      uint64_t iv_start = read_u64_le(cursor);
      cursor += 8;
      uint64_t iv_end = read_u64_le(cursor);
      cursor += 8;
      if (i > 0) result += ':';
      if (iv_end == iv_start + 1) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%llu",
                      static_cast<unsigned long long>(iv_start));
        result += buf;
      } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%llu-%llu",
                      static_cast<unsigned long long>(iv_start),
                      static_cast<unsigned long long>(iv_end - 1));
        result += buf;
      }
    }
  }
  return result;
}

void note_gtid_event(ChannelState &state, const char *event_buf,
                     unsigned long event_len) {
  const unsigned char event_type =
      static_cast<unsigned char>(event_buf[kEventTypeOffset]);
  auto &fm = state.file_metadata[state.current_log_name];

  if (event_type == kPreviousGtidsLogEventType) {
    std::string text =
        extract_previous_gtids_text(event_buf, event_len, state.has_checksum);
    if (!text.empty()) {
      fm.previous_gtid_set = text;
      fm.last_gtid_set = text;
    }
  } else if (event_type == kGtidLogEventType ||
             event_type == kGtidTaggedLogEventType) {
    std::string gtid = extract_gtid_text(event_buf, event_len);
    if (!gtid.empty()) {
      gtid::Gtid_set merged;
      if (!fm.last_gtid_set.empty())
        merged.assign_from_text(fm.last_gtid_set);
      gtid::Gtid_set single;
      if (single.assign_from_text(gtid))
        merged.merge(single);
      fm.last_gtid_set = merged.to_text();
    }
  } else if (event_type == kAnonymousGtidLogEventType) {
  }
}

void flush_file_metadata(ChannelState &state, const std::string &log_name) {
  auto it = state.file_metadata.find(log_name);
  if (it == state.file_metadata.end()) return;
  auto &fm = it->second;
  {
    std::error_code ec;
    auto sz = fs::file_size(state.base_dir + log_name, ec);
    if (!ec && sz > 0) fm.size_bytes = sz;
  }

  std::string content;
  content += "version=1\n";
  content += "min_event_timestamp=" + std::to_string(fm.min_event_timestamp) + "\n";
  content += "max_event_timestamp=" + std::to_string(fm.max_event_timestamp) + "\n";
  content += "event_count=" + std::to_string(fm.event_count) + "\n";
  content += "size_bytes=" + std::to_string(fm.size_bytes) + "\n";
  if (!fm.previous_gtid_set.empty())
    content += "previous_gtid_set=" + fm.previous_gtid_set + "\n";
  if (!fm.last_gtid_set.empty())
    content += "last_gtid_set=" + fm.last_gtid_set + "\n";

  if (state.backend) {
    state.backend->sidecar_store(state.base_dir, log_name, content);
  } else {
    const std::string path = state.base_dir + log_name + ".meta";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out.is_open()) {
      out << content;
      out.flush();
    }
  }
}

void parse_metadata_content(const std::string &content, FileMetadata &fm) {
  std::istringstream in(content);
  std::string line;
  while (std::getline(in, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);
    try {
      if (key == "min_event_timestamp")
        fm.min_event_timestamp = static_cast<uint32_t>(std::stoul(val));
      else if (key == "max_event_timestamp")
        fm.max_event_timestamp = static_cast<uint32_t>(std::stoul(val));
      else if (key == "event_count")
        fm.event_count = std::stoull(val);
      else if (key == "size_bytes")
        fm.size_bytes = std::stoull(val);
      else if (key == "previous_gtid_set")
        fm.previous_gtid_set = val;
      else if (key == "last_gtid_set")
        fm.last_gtid_set = val;
    } catch (const std::exception &) {
    }
  }
}

void load_file_metadata(ChannelState &state) {
  for (const auto &fname : state.file_order) {
    std::string content;
    bool loaded = false;

    if (state.backend) {
      loaded = state.backend->sidecar_load(state.base_dir, fname, content);
    } else {
      const std::string path = state.base_dir + fname + ".meta";
      std::ifstream in(path);
      if (in.is_open()) {
        std::ostringstream ss;
        ss << in.rdbuf();
        content = ss.str();
        loaded = true;
      }
    }

    if (loaded && !content.empty()) {
      auto &fm = state.file_metadata[fname];
      parse_metadata_content(content, fm);
    }
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

void BinlogArchive::set_storage_root(const std::string &root) {
  std::lock_guard<std::mutex> lk(m_root_mutex);
  m_storage_root = root;
  if (!m_storage_root.empty() && m_storage_root.back() != '/')
    m_storage_root += '/';
}

bool BinlogArchive::storage_uri_allowed(const std::string &uri,
                                        std::string *reason) const {
  std::lock_guard<std::mutex> lk(m_root_mutex);
  if (m_storage_root.empty()) return true;

  std::string path = parse_file_uri(uri);
  if (path.empty()) {
    if (uri.compare(0, 5, "s3://") == 0) return true;
    if (reason) *reason = "URI scheme not recognized";
    return false;
  }
  std::error_code ec;
  auto resolved = fs::canonical(fs::path(path), ec);
  if (ec) {
    auto parent = fs::path(path).parent_path();
    resolved = fs::canonical(parent, ec);
    if (ec) {
      if (reason) *reason = "path does not exist and parent is not resolvable";
      return false;
    }
    resolved /= fs::path(path).filename();
  }
  auto root_resolved = fs::canonical(fs::path(m_storage_root), ec);
  if (ec) {
    if (reason) *reason = "storage_root path is not resolvable";
    return false;
  }

  std::string resolved_str = resolved.string();
  std::string root_str = root_resolved.string();
  if (!root_str.empty() && root_str.back() != '/') root_str += '/';

  if (resolved_str.compare(0, root_str.size(), root_str) != 0) {
    if (reason)
      *reason = "URI path is not under binlog_server.storage_root ('" +
                m_storage_root + "')";
    return false;
  }
  return true;
}

void BinlogArchive::set_encryption_enabled(bool enabled) {
  m_encryption_enabled.store(enabled, std::memory_order_relaxed);
}

bool BinlogArchive::encryption_enabled() const {
  return m_encryption_enabled.load(std::memory_order_relaxed);
}

std::string BinlogArchive::active_file_path(const char *channel_name) const {
  if (channel_name == nullptr) return {};
  std::string lookup = channel_name;
  // The default channel is stored under "" internally but users reference
  // it as "_default" (the sanitized directory name).
  if (lookup == "_default") lookup.clear();
  std::lock_guard<std::mutex> lk(m_registry_mutex);
  auto it = m_channels.find(lookup);
  if (it == m_channels.end()) return {};
  std::lock_guard<std::mutex> io_lk(it->second->io_mutex);
  if (it->second->current_log_name.empty()) return {};
  return it->second->base_dir + it->second->current_log_name;
}

std::string BinlogArchive::active_file_name(const char *channel_name) const {
  if (channel_name == nullptr) return {};
  std::string lookup = channel_name;
  if (lookup == "_default") lookup.clear();
  std::lock_guard<std::mutex> lk(m_registry_mutex);
  auto it = m_channels.find(lookup);
  if (it == m_channels.end()) return {};
  std::lock_guard<std::mutex> io_lk(it->second->io_mutex);
  return it->second->current_log_name;
}

std::string BinlogArchive::resolve_channel_base_dir(
    const char *channel_name) const {
  if (channel_name == nullptr) return {};
  std::string lookup = channel_name;
  if (lookup == "_default") lookup.clear();
  std::lock_guard<std::mutex> lk(m_registry_mutex);
  auto it = m_channels.find(lookup);
  if (it == m_channels.end()) return {};
  std::lock_guard<std::mutex> io_lk(it->second->io_mutex);
  return it->second->base_dir;
}

StorageBackend *BinlogArchive::resolve_channel_backend(
    const char *channel_name) const {
  if (channel_name == nullptr) return nullptr;
  std::string lookup = channel_name;
  if (lookup == "_default") lookup.clear();
  std::lock_guard<std::mutex> lk(m_registry_mutex);
  auto it = m_channels.find(lookup);
  if (it == m_channels.end()) return nullptr;
  std::lock_guard<std::mutex> io_lk(it->second->io_mutex);
  return it->second->backend;
}

std::string BinlogArchive::effective_uri(const std::string &channel_uri) {
  if (!channel_uri.empty()) return channel_uri;
  std::lock_guard<std::mutex> lk(m_uri_mutex);
  return m_default_storage_uri;
}

std::unique_ptr<StorageBackend> BinlogArchive::create_backend(
    const std::string &uri) {
  if (uri.compare(0, 7, "file://") == 0) {
    return std::make_unique<FileStorage>();
  }
  if (uri.compare(0, 5, "s3://") == 0) {
    return create_s3_storage();
  }
  bslog(ERROR_LEVEL,
        "binlog_server: unsupported or invalid storage URI: '%s'",
        uri.c_str());
  return nullptr;
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

  {
    std::string rejection;
    if (!storage_uri_allowed(eff_uri, &rejection)) {
      bslog(ERROR_LEVEL,
            "binlog_server: channel '%s' configure rejected: %s",
            name.c_str(), rejection.c_str());
      return 1;
    }
  }

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

  std::string dir_name = sanitize_channel_dir_name(name);

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
  if (!cs_ptr) {
    static std::atomic<unsigned long> unconfigured_count{0};
    unsigned long count = ++unconfigured_count;
    if (count <= 3 || count % 10000 == 0) {
      bslog(WARNING_LEVEL,
            "binlog_server: append_event for unconfigured channel '%s' "
            "(occurrence #%lu); event dropped",
            name.c_str(), count);
    }
    return 0;
  }

  auto &cs = *cs_ptr;
  std::lock_guard<std::mutex> lk(cs.io_mutex);

  if (!cs.enabled) return 0;

  const unsigned char event_type =
      static_cast<unsigned char>(event_buf[kEventTypeOffset]);
  const uint32_t hdr_event_len = read_u32_le(event_buf + kEventLenOffset);
  const uint32_t log_pos = read_u32_le(event_buf + kLogPosOffset);
  const uint16_t flags = read_u16_le(event_buf + kFlagsOffset);
  const bool is_artificial = (flags & kLogEventArtificialF) != 0;

  if (hdr_event_len != event_len) {
    if (should_log_io_failure(cs)) {
      bslog(WARNING_LEVEL,
            "binlog_server: channel '%s' event_len mismatch: header says %u "
            "but buffer is %lu bytes; skipping event",
            name.c_str(), hdr_event_len, event_len);
    }
    return 0;
  }

  const bool encrypt = m_encryption_enabled.load(std::memory_order_relaxed);

  switch (event_type) {
    case kRotateEventType:
      handle_rotate_event(cs, name.c_str(), event_buf, event_len);
      return 0;
    case kFormatDescriptionEventType:
      handle_format_description_event(cs, name.c_str(), event_buf, event_len,
                                      encrypt);
      return 0;
    default:
      break;
  }

  if (is_artificial) return 0;
  if (!cs.wrote_fde) return 0;
  if (!cs.out) return 0;

  // Deduplication: skip events already archived (watermark-based).
  if (log_pos != 0 && log_pos <= cs.last_source_log_pos) {
    ++cs.duplicates_dropped;
    return 0;
  }

  if (!write_event_data(cs, event_buf, event_len)) {
    record_channel_error(
        cs, ER_BINLOG_SERVER_IO_FAILURE,
        std::string("write error on file '") + cs.current_log_name + "'");
    if (should_log_io_failure(cs)) {
      bslog_code(ERROR_LEVEL, ER_BINLOG_SERVER_IO_FAILURE,
                 "write error on channel '%s', file '%s%s' "
                 "(io_failure_count=%lu)",
                 name.c_str(), cs.base_dir.c_str(),
                 cs.current_log_name.c_str(), cs.io_failure_count);
    }
    return 1;
  }
  cs.out->flush();

  cs.io_failure_count = 0;
  if (log_pos > 0) cs.last_source_log_pos = log_pos;
  ++cs.events_appended;
  cs.bytes_appended += event_len;

  // Track per-file metadata (timestamps + GTIDs)
  note_event_timestamp(cs, event_buf);
  if (event_type == kGtidLogEventType ||
      event_type == kGtidTaggedLogEventType ||
      event_type == kPreviousGtidsLogEventType ||
      event_type == kAnonymousGtidLogEventType) {
    note_gtid_event(cs, event_buf, event_len);
  }

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

  if (!cs_ptr->out) return 0;

  sync_channel_to_disk(*cs_ptr, name.c_str());

  // Flush per-file metadata sidecar before closing
  if (!cs_ptr->current_log_name.empty()) {
    auto &fm = cs_ptr->file_metadata[cs_ptr->current_log_name];
    std::error_code ec;
    auto sz = fs::file_size(cs_ptr->base_dir + cs_ptr->current_log_name, ec);
    if (!ec) fm.size_bytes = sz;
    flush_file_metadata(*cs_ptr, cs_ptr->current_log_name);
  }

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
  cs_ptr->duplicates_dropped = 0;
  cs_ptr->write_errors = 0;
  cs_ptr->last_error_code = 0;
  cs_ptr->last_error_message.clear();
  cs_ptr->last_error_timestamp_us = 0;
  cs_ptr->last_event_timestamp_us = 0;
  cs_ptr->file_metadata.clear();
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

  // Flush per-file metadata sidecar before rotating away
  if (!cs_ptr->current_log_name.empty()) {
    auto &fm = cs_ptr->file_metadata[cs_ptr->current_log_name];
    std::error_code ec;
    auto sz =
        fs::file_size(cs_ptr->base_dir + cs_ptr->current_log_name, ec);
    if (!ec) fm.size_bytes = sz;
    flush_file_metadata(*cs_ptr, cs_ptr->current_log_name);
  }

  close_archive_file(*cs_ptr);

  cs_ptr->current_log_name = new_log_name;
  cs_ptr->wrote_fde = false;
  cs_ptr->last_source_log_pos = 0;
  return 0;
}

// --- PFS snapshot methods (Phase 4) ---

std::vector<ChannelStatus> BinlogArchive::snapshot_status() const {
  std::vector<ChannelStatus> result;
  std::vector<std::shared_ptr<ChannelState>> channels;
  {
    std::lock_guard<std::mutex> lk(m_registry_mutex);
    channels.reserve(m_channels.size());
    for (auto &kv : m_channels) channels.push_back(kv.second);
  }

  for (auto &cs_ptr : channels) {
    ChannelStatus row;
    std::string file_path;
    {
      std::lock_guard<std::mutex> lk(cs_ptr->io_mutex);
      row.channel_name = cs_ptr->name.empty()
                             ? "_default"
                             : cs_ptr->name;
      row.enabled = cs_ptr->enabled;
      row.storage_uri = cs_ptr->storage_uri;
      row.base_dir = cs_ptr->base_dir;
      row.current_log_name = cs_ptr->current_log_name;
      row.has_checksum = cs_ptr->has_checksum;
      row.events_appended = cs_ptr->events_appended;
      row.bytes_appended = cs_ptr->bytes_appended;
      row.last_source_log_pos = cs_ptr->last_source_log_pos;
      row.duplicates_dropped = cs_ptr->duplicates_dropped;
      row.indexed_files = cs_ptr->indexed_files.size();
      row.last_event_timestamp_us = cs_ptr->last_event_timestamp_us;
      row.last_error_code = cs_ptr->last_error_code;
      row.last_error_message = cs_ptr->last_error_message;
      row.last_error_timestamp_us = cs_ptr->last_error_timestamp_us;
      row.write_errors = cs_ptr->write_errors;
      if (!row.base_dir.empty() && !row.current_log_name.empty())
        file_path = row.base_dir + row.current_log_name;
    }
    // stat() outside lock
    if (!file_path.empty()) {
      std::error_code ec;
      auto sz = fs::file_size(file_path, ec);
      if (!ec) row.current_file_size_bytes = sz;
    }
    result.push_back(std::move(row));
  }
  return result;
}

std::vector<ChannelStorage> BinlogArchive::snapshot_storage() const {
  std::vector<ChannelStorage> result;
  std::vector<std::shared_ptr<ChannelState>> channels;
  {
    std::lock_guard<std::mutex> lk(m_registry_mutex);
    channels.reserve(m_channels.size());
    for (auto &kv : m_channels) channels.push_back(kv.second);
  }

  for (auto &cs_ptr : channels) {
    ChannelStorage row;
    std::string base_dir;
    std::string active_file_path;
    {
      std::lock_guard<std::mutex> lk(cs_ptr->io_mutex);
      row.channel_name = cs_ptr->name.empty()
                             ? "_default"
                             : cs_ptr->name;
      row.storage_type = "FILE";
      row.storage_uri = cs_ptr->storage_uri;
      row.base_path = cs_ptr->base_dir;
      row.file_count = cs_ptr->indexed_files.size();
      row.active_file = cs_ptr->current_log_name;
      row.last_error_code = cs_ptr->last_error_code;
      row.last_error_message = cs_ptr->last_error_message;
      row.last_error_timestamp_us = cs_ptr->last_error_timestamp_us;
      base_dir = cs_ptr->base_dir;
      if (!base_dir.empty() && !cs_ptr->current_log_name.empty())
        active_file_path = base_dir + cs_ptr->current_log_name;

      if (cs_ptr->last_error_code != 0)
        row.status = "ERROR";
      else if (!cs_ptr->enabled)
        row.status = "DISABLED";
      else if (cs_ptr->out)
        row.status = "ACTIVE";
      else
        row.status = "IDLE";
    }
    // Filesystem operations outside lock
    if (!active_file_path.empty()) {
      std::error_code ec;
      auto sz = fs::file_size(active_file_path, ec);
      if (!ec) row.active_file_bytes = sz;
    }
    if (!base_dir.empty()) {
      uint64_t total = 0;
      std::error_code ec;
      for (auto &entry : fs::directory_iterator(base_dir, ec)) {
        if (entry.is_regular_file()) {
          auto sz = entry.file_size(ec);
          if (!ec) total += sz;
        }
      }
      row.total_bytes_on_disk = total;
    }
    result.push_back(std::move(row));
  }
  return result;
}

std::vector<ChannelArchive> BinlogArchive::snapshot_archive() const {
  std::vector<ChannelArchive> result;
  std::vector<std::shared_ptr<ChannelState>> channels;
  {
    std::lock_guard<std::mutex> lk(m_registry_mutex);
    channels.reserve(m_channels.size());
    for (auto &kv : m_channels) channels.push_back(kv.second);
  }

  for (auto &cs_ptr : channels) {
    struct LocalRow {
      std::string file_name;
      FileMetadata meta;
      bool is_active;
    };
    std::vector<LocalRow> rows;
    std::string base_dir;
    std::string channel_name;
    {
      std::lock_guard<std::mutex> lk(cs_ptr->io_mutex);
      load_index_file(*cs_ptr);
      base_dir = cs_ptr->base_dir;
      channel_name = cs_ptr->name.empty() ? "_default" : cs_ptr->name;
      rows.reserve(cs_ptr->file_order.size());
      for (const auto &fname : cs_ptr->file_order) {
        LocalRow r;
        r.file_name = fname;
        auto it = cs_ptr->file_metadata.find(fname);
        if (it != cs_ptr->file_metadata.end()) r.meta = it->second;
        r.is_active = (fname == cs_ptr->current_log_name &&
                       cs_ptr->out != nullptr);
        rows.push_back(std::move(r));
      }
    }
    // Filesystem stat outside lock
    for (auto &lr : rows) {
      ChannelArchive row;
      row.channel_name = channel_name;
      row.file_name = lr.file_name;
      row.is_active = lr.is_active;
      row.min_event_timestamp_us =
          static_cast<uint64_t>(lr.meta.min_event_timestamp) * 1000000ULL;
      row.max_event_timestamp_us =
          static_cast<uint64_t>(lr.meta.max_event_timestamp) * 1000000ULL;
      row.event_count = lr.meta.event_count;
      row.previous_gtid_set = std::move(lr.meta.previous_gtid_set);
      row.last_gtid_set = std::move(lr.meta.last_gtid_set);
      if (!base_dir.empty()) {
        std::error_code ec;
        auto sz = fs::file_size(base_dir + lr.file_name, ec);
        row.size_bytes = (!ec && sz > 0) ? sz : lr.meta.size_bytes;
      } else {
        row.size_bytes = lr.meta.size_bytes;
      }
      result.push_back(std::move(row));
    }
  }
  return result;
}

long long BinlogArchive::rebuild_archive_index(const char *channel_name) {
  std::string name = channel_name != nullptr ? channel_name : "";
  auto cs_ptr = find_channel(name);
  if (!cs_ptr) return -1;

  std::lock_guard<std::mutex> lk(cs_ptr->io_mutex);
  load_index_file(*cs_ptr);

  long long rebuilt = 0;
  for (const auto &fname : cs_ptr->file_order) {
    // Skip files that already have a credible .meta on disk
    {
      const std::string meta_path = cs_ptr->base_dir + fname + ".meta";
      std::ifstream mf(meta_path);
      if (mf.is_open()) {
        FileMetadata on_disk{};
        std::string line;
        while (std::getline(mf, line)) {
          auto eq = line.find('=');
          if (eq == std::string::npos) continue;
          std::string key = line.substr(0, eq);
          std::string val = line.substr(eq + 1);
          try {
            if (key == "event_count") on_disk.event_count = std::stoull(val);
            else if (key == "min_event_timestamp")
              on_disk.min_event_timestamp = static_cast<uint32_t>(std::stoul(val));
            else if (key == "max_event_timestamp")
              on_disk.max_event_timestamp = static_cast<uint32_t>(std::stoul(val));
            else if (key == "size_bytes") on_disk.size_bytes = std::stoull(val);
            else if (key == "previous_gtid_set") on_disk.previous_gtid_set = val;
            else if (key == "last_gtid_set") on_disk.last_gtid_set = val;
          } catch (...) {}
        }
        if (on_disk.event_count != 0 || on_disk.min_event_timestamp != 0 ||
            on_disk.max_event_timestamp != 0 || on_disk.size_bytes != 0 ||
            !on_disk.previous_gtid_set.empty() ||
            !on_disk.last_gtid_set.empty()) {
          cs_ptr->file_metadata[fname] = std::move(on_disk);
          continue;
        }
      }
    }

    const std::string path = cs_ptr->base_dir + fname;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) continue;

    char magic[kBinlogMagicSize];
    if (!f.read(magic, kBinlogMagicSize) ||
        std::memcmp(magic, kBinlogMagic, kBinlogMagicSize) != 0)
      continue;

    FileMetadata fm;
    bool file_has_checksum = cs_ptr->has_checksum;
    uint64_t offset = kBinlogMagicSize;

    for (;;) {
      char hdr[kLogEventHeaderLen];
      f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
      f.read(hdr, kLogEventHeaderLen);
      if (f.gcount() < static_cast<std::streamsize>(kLogEventHeaderLen)) break;

      const uint32_t ts = read_u32_le(hdr);
      const unsigned char etype =
          static_cast<unsigned char>(hdr[kEventTypeOffset]);
      const uint32_t elen = read_u32_le(hdr + kEventLenOffset);
      const uint16_t eflags = read_u16_le(hdr + kFlagsOffset);
      if (elen < kLogEventHeaderLen || elen > (64ULL << 20)) break;

      // Verify we can read the full event
      f.seekg(static_cast<std::streamoff>(offset) +
                  static_cast<std::streamoff>(elen) - 1,
              std::ios::beg);
      char probe = 0;
      if (!f.read(&probe, 1) || f.gcount() != 1) break;
      f.clear();

      // Exclude artificial events and FDE from timestamp accounting
      if (ts > 0 && !(eflags & kLogEventArtificialF) &&
          etype != kFormatDescriptionEventType) {
        if (fm.min_event_timestamp == 0 || ts < fm.min_event_timestamp)
          fm.min_event_timestamp = ts;
        if (ts > fm.max_event_timestamp) fm.max_event_timestamp = ts;
        ++fm.event_count;
      }

      // Detect checksum from FDE
      if (etype == kFormatDescriptionEventType &&
          elen >= kLogEventHeaderLen + kBinlogChecksumAlgDescLen +
                      kBinlogChecksumLen) {
        f.seekg(static_cast<std::streamoff>(offset) +
                    static_cast<std::streamoff>(elen) -
                    static_cast<std::streamoff>(kBinlogChecksumAlgDescLen) -
                    static_cast<std::streamoff>(kBinlogChecksumLen),
                std::ios::beg);
        char alg_byte = 0;
        if (f.read(&alg_byte, 1) && f.gcount() == 1) {
          const unsigned char alg = static_cast<unsigned char>(alg_byte);
          file_has_checksum =
              (alg != kChecksumAlgOff && alg != kChecksumAlgUndef);
        }
        f.clear();
      }

      // Extract GTID info
      if (etype == kPreviousGtidsLogEventType ||
          etype == kGtidLogEventType ||
          etype == kGtidTaggedLogEventType) {
        std::vector<char> ev(elen);
        f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        f.read(ev.data(), elen);
        if (f.gcount() == static_cast<std::streamsize>(elen)) {
          if (etype == kPreviousGtidsLogEventType) {
            std::string text = extract_previous_gtids_text(
                ev.data(), elen, file_has_checksum);
            if (!text.empty()) {
              fm.previous_gtid_set = text;
              fm.last_gtid_set = std::move(text);
            }
          } else {
            std::string gtid = extract_gtid_text(ev.data(), elen);
            if (!gtid.empty()) {
              gtid::Gtid_set merged;
              if (!fm.last_gtid_set.empty())
                merged.assign_from_text(fm.last_gtid_set);
              gtid::Gtid_set single;
              if (single.assign_from_text(gtid))
                merged.merge(single);
              fm.last_gtid_set = merged.to_text();
            }
          }
        }
        f.clear();
      }

      offset += elen;
    }
    f.close();

    // Get file size
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (!ec) fm.size_bytes = sz;

    cs_ptr->file_metadata[fname] = std::move(fm);
    flush_file_metadata(*cs_ptr, fname);
    ++rebuilt;
  }
  return rebuilt;
}

// --- Purge implementation (Phase 5) ---

namespace {

// Shared helper: commit purge via index-first strategy.
// 1. Update in-memory state (file_order, indexed_files, file_metadata)
// 2. Atomically rewrite binlog.index via StorageBackend (commit point)
// 3. Best-effort removal of payload + sidecar files
// Returns count of files purged, or -1 if index rewrite failed.
long long commit_purge(ChannelState &cs, const std::vector<std::string> &victims,
                       const char *channel_name) {
  if (victims.empty()) return 0;

  // Check if any victim is pinned by an active dump session
  auto &sender = ArchiveSender::instance();
  std::string channel_str(channel_name);
  for (const auto &fname : victims) {
    if (sender.is_file_pinned(channel_str, fname)) {
      bslog(WARNING_LEVEL,
            "binlog_server: channel '%s' purge refused: file '%s' is "
            "being served to a downstream replica",
            channel_name, fname.c_str());
      return -1;
    }
  }

  // Step 1: update in-memory state
  for (const auto &fname : victims) {
    cs.file_metadata.erase(fname);
    cs.indexed_files.erase(fname);
  }
  std::vector<std::string> remaining;
  remaining.reserve(cs.file_order.size() - victims.size());
  for (const auto &f : cs.file_order) {
    if (cs.indexed_files.count(f)) remaining.push_back(f);
  }
  cs.file_order = std::move(remaining);

  // Step 2: atomic index rewrite (commit point)
  if (cs.backend) {
    if (!cs.backend->index_rewrite(cs.base_dir, cs.file_order)) {
      bslog(ERROR_LEVEL,
            "binlog_server: channel '%s' purge ABORTED: atomic index rewrite "
            "failed; restoring in-memory state",
            channel_name);
      for (const auto &fname : victims) {
        cs.indexed_files.insert(fname);
      }
      cs.file_order.clear();
      for (const auto &f : cs.indexed_files) cs.file_order.push_back(f);
      std::sort(cs.file_order.begin(), cs.file_order.end());
      return -1;
    }
  } else {
    bslog(ERROR_LEVEL,
          "binlog_server: channel '%s' purge ABORTED: no storage backend "
          "available for index rewrite",
          channel_name);
    for (const auto &fname : victims) {
      cs.indexed_files.insert(fname);
    }
    cs.file_order.clear();
    for (const auto &f : cs.indexed_files) cs.file_order.push_back(f);
    std::sort(cs.file_order.begin(), cs.file_order.end());
    return -1;
  }

  // Step 3: best-effort file removal (after commit point)
  long long purged = 0;
  std::string cleanup_warnings;
  for (const auto &fname : victims) {
    std::error_code ec;
    bool binlog_ok = fs::remove(cs.base_dir + fname, ec);
    if (!binlog_ok && !ec) {
      if (!cleanup_warnings.empty()) cleanup_warnings += ", ";
      cleanup_warnings += fname + " (file not found)";
    } else if (ec) {
      if (!cleanup_warnings.empty()) cleanup_warnings += ", ";
      cleanup_warnings += fname + " (" + ec.message() + ")";
    }
    // Remove .meta sidecar (best-effort, no warning)
    fs::remove(cs.base_dir + fname + ".meta", ec);
    ++purged;
  }

  if (!cleanup_warnings.empty()) {
    bslog(WARNING_LEVEL,
          "binlog_server: channel '%s' purge committed (index updated) but "
          "some file removals had issues: %s",
          channel_name, cleanup_warnings.c_str());
  }

  return purged;
}

// Validate a purge target name: must look like a binlog filename (base.NNNNNN)
bool is_valid_binlog_name(const std::string &name) {
  auto dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0) return false;
  if (name.size() - dot - 1 < 6) return false;
  for (size_t i = dot + 1; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  return true;
}

// Check base-name consistency: the target must share a prefix with the archive
bool base_name_matches(const std::string &target,
                       const std::vector<std::string> &file_order) {
  if (file_order.empty()) return true;
  auto dot_target = target.rfind('.');
  auto dot_first = file_order.front().rfind('.');
  if (dot_target == std::string::npos || dot_first == std::string::npos)
    return false;
  return target.substr(0, dot_target) ==
         file_order.front().substr(0, dot_first);
}

}  // anonymous namespace

long long BinlogArchive::purge_channel(const char *channel_name,
                                       const char *up_to_file) {
  if (up_to_file == nullptr || up_to_file[0] == '\0') return -1;
  std::string name = channel_name != nullptr ? channel_name : "";
  auto cs_ptr = find_channel(name);
  if (!cs_ptr) return -1;

  std::lock_guard<std::mutex> lk(cs_ptr->io_mutex);
  load_index_file(*cs_ptr);

  auto &fo = cs_ptr->file_order;

  // Empty storage check
  if (fo.empty()) {
    bslog(WARNING_LEVEL,
          "binlog_server: purge_channel('%s', '%s') failed: storage is empty",
          name.c_str(), up_to_file);
    return -1;
  }

  const std::string target(up_to_file);

  // Validate target looks like a binlog filename
  if (!is_valid_binlog_name(target)) {
    bslog(WARNING_LEVEL,
          "binlog_server: purge_channel('%s', '%s') failed: "
          "not a valid binlog filename",
          name.c_str(), up_to_file);
    return -1;
  }

  // Base-name consistency check
  if (!base_name_matches(target, fo)) {
    bslog(WARNING_LEVEL,
          "binlog_server: purge_channel('%s', '%s') failed: "
          "target has a different base name than the archive files",
          name.c_str(), up_to_file);
    return -1;
  }

  // Find the target in file_order
  auto target_it = std::find(fo.begin(), fo.end(), target);
  if (target_it == fo.end()) {
    bslog(WARNING_LEVEL,
          "binlog_server: purge_channel('%s', '%s') failed: "
          "target not present in the archive",
          name.c_str(), up_to_file);
    return -1;
  }

  // Tail protection: refuse the last index entry unconditionally
  if (target_it == std::prev(fo.end())) {
    bslog(WARNING_LEVEL,
          "binlog_server: purge_channel('%s', '%s') refused: "
          "cannot purge the tail file (at least one file must remain)",
          name.c_str(), up_to_file);
    return -1;
  }

  // Build victim list: [oldest ... target] inclusive
  std::vector<std::string> victims;
  for (auto it = fo.begin(); ; ++it) {
    victims.push_back(*it);
    if (*it == target) break;
  }

  long long purged = commit_purge(*cs_ptr, victims, name.c_str());
  bslog(INFORMATION_LEVEL,
        "binlog_server: channel '%s' purged %lld file(s) up to '%s'",
        name.c_str(), purged, up_to_file);
  return purged;
}

long long BinlogArchive::purge_before_gtid(const char *channel_name,
                                           const char *gtid_set_text) {
  if (gtid_set_text == nullptr || gtid_set_text[0] == '\0') return -1;
  std::string name = channel_name != nullptr ? channel_name : "";
  auto cs_ptr = find_channel(name);
  if (!cs_ptr) return -1;

  gtid::Gtid_set threshold;
  if (!threshold.assign_from_text(gtid_set_text)) return -1;

  std::lock_guard<std::mutex> lk(cs_ptr->io_mutex);
  load_index_file(*cs_ptr);

  auto &fo = cs_ptr->file_order;
  if (fo.empty()) return -1;

  std::vector<std::string> victims;

  for (const auto &fname : fo) {
    // Always keep at least one file (the tail)
    if (fo.size() - victims.size() <= 1) break;

    auto it = cs_ptr->file_metadata.find(fname);
    if (it == cs_ptr->file_metadata.end()) break;

    const auto &fm = it->second;
    if (fm.last_gtid_set.empty()) break;

    gtid::Gtid_set file_gtids;
    if (!file_gtids.assign_from_text(fm.last_gtid_set)) break;

    if (file_gtids.is_subset_of(threshold)) {
      victims.push_back(fname);
    } else {
      break;
    }
  }

  long long purged = commit_purge(*cs_ptr, victims, name.c_str());
  if (purged > 0) {
    bslog(INFORMATION_LEVEL,
          "binlog_server: channel '%s' purged %lld file(s) by GTID containment",
          name.c_str(), purged);
  }
  return purged;
}

long long BinlogArchive::purge_before_timestamp(const char *channel_name,
                                                unsigned long timestamp) {
  if (timestamp == 0) return -1;
  std::string name = channel_name != nullptr ? channel_name : "";
  auto cs_ptr = find_channel(name);
  if (!cs_ptr) return -1;

  std::lock_guard<std::mutex> lk(cs_ptr->io_mutex);
  load_index_file(*cs_ptr);

  auto &fo = cs_ptr->file_order;
  if (fo.empty()) return -1;

  std::vector<std::string> victims;

  for (const auto &fname : fo) {
    // Always keep at least one file (the tail)
    if (fo.size() - victims.size() <= 1) break;

    auto it = cs_ptr->file_metadata.find(fname);
    if (it == cs_ptr->file_metadata.end()) break;

    const auto &fm = it->second;
    if (fm.max_event_timestamp == 0) break;

    if (fm.max_event_timestamp < static_cast<uint32_t>(timestamp)) {
      victims.push_back(fname);
    } else {
      break;
    }
  }

  long long purged = commit_purge(*cs_ptr, victims, name.c_str());
  if (purged > 0) {
    bslog(INFORMATION_LEVEL,
          "binlog_server: channel '%s' purged %lld file(s) before timestamp %lu",
          name.c_str(), purged, timestamp);
  }
  return purged;
}

// ---------------------------------------------------------------------------
// Search helpers
// ---------------------------------------------------------------------------

static uint32_t parse_iso_timestamp(const char *s) {
  if (s == nullptr || s[0] == '\0') return 0;
  struct tm tm {};
  const char *p = strptime(s, "%Y-%m-%dT%H:%M:%S", &tm);
  if (p == nullptr) {
    p = strptime(s, "%Y-%m-%d %H:%M:%S", &tm);
  }
  if (p == nullptr) return 0;
  tm.tm_isdst = -1;
  time_t t = mktime(&tm);
  return (t <= 0) ? 0 : static_cast<uint32_t>(t);
}

static std::string escape_json_string(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  return out;
}

static std::string format_iso_timestamp(uint32_t ts) {
  if (ts == 0) return "";
  time_t t = static_cast<time_t>(ts);
  struct tm tm {};
  localtime_r(&t, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  return buf;
}

static std::string file_meta_to_json(const std::string &name,
                                     const std::string &base_dir,
                                     const FileMetadata &fm) {
  std::string gtids_text;
  if (!fm.last_gtid_set.empty()) {
    gtid::Gtid_set last;
    if (last.assign_from_text(fm.last_gtid_set)) {
      if (!fm.previous_gtid_set.empty()) {
        gtid::Gtid_set prev;
        if (prev.assign_from_text(fm.previous_gtid_set))
          gtids_text = last.subtract(prev).to_text();
        else
          gtids_text = fm.last_gtid_set;
      } else {
        gtids_text = fm.last_gtid_set;
      }
    }
  }

  std::string o = "{";
  o += "\"name\":\"" + escape_json_string(name) + "\"";
  o += ",\"size\":" + std::to_string(fm.size_bytes);
  o += ",\"uri\":\"file://" + escape_json_string(base_dir + name) + "\"";
  o += ",\"min_timestamp\":\"" + format_iso_timestamp(fm.min_event_timestamp) +
       "\"";
  o += ",\"max_timestamp\":\"" + format_iso_timestamp(fm.max_event_timestamp) +
       "\"";
  o += ",\"gtids\":\"" + escape_json_string(gtids_text) + "\"";
  o += "}";
  return o;
}

std::string BinlogArchive::search_by_timestamp(const char *channel_name,
                                               const char *iso_from,
                                               const char *iso_to) {
  if (iso_from == nullptr || iso_from[0] == '\0' || iso_to == nullptr ||
      iso_to[0] == '\0')
    return R"js({"status":"error","message":"Both from and to timestamps are required (ISO-8601)"})js";

  const uint32_t ts_from = parse_iso_timestamp(iso_from);
  const uint32_t ts_to = parse_iso_timestamp(iso_to);
  if (ts_from == 0 || ts_to == 0)
    return R"js({"status":"error","message":"Invalid timestamp format (use YYYY-MM-DDTHH:MM:SS)"})js";
  if (ts_from > ts_to)
    return R"({"status":"error","message":"'from' must be earlier than 'to'"})";

  std::string name = channel_name != nullptr ? channel_name : "";
  auto cs_ptr = find_channel(name);
  if (!cs_ptr)
    return R"({"status":"error","message":"Channel not found"})";

  std::lock_guard<std::mutex> lk(cs_ptr->io_mutex);
  load_index_file(*cs_ptr);

  auto &fo = cs_ptr->file_order;
  if (fo.empty())
    return R"({"status":"error","message":"Binlog storage is empty"})";

  // Find files that overlap with [ts_from, ts_to].
  // A file overlaps if: file.min <= ts_to AND file.max >= ts_from
  std::string result_arr = "[";
  bool first = true;
  for (const auto &fname : fo) {
    auto it = cs_ptr->file_metadata.find(fname);
    if (it == cs_ptr->file_metadata.end()) continue;
    const auto &fm = it->second;

    if (fm.min_event_timestamp == 0) continue;

    if (fm.min_event_timestamp > ts_to) break;

    if (fm.max_event_timestamp >= ts_from) {
      if (!first) result_arr += ",";
      result_arr += file_meta_to_json(fname, cs_ptr->base_dir, fm);
      first = false;
    }
  }
  result_arr += "]";

  return "{\"status\":\"success\",\"result\":" + result_arr + "}";
}

std::string BinlogArchive::search_by_gtid_set(const char *channel_name,
                                              const char *gtid_set_text) {
  if (gtid_set_text == nullptr || gtid_set_text[0] == '\0')
    return R"({"status":"error","message":"cannot parse GTID set"})";

  gtid::Gtid_set requested;
  if (!requested.assign_from_text(gtid_set_text))
    return R"({"status":"error","message":"cannot parse GTID set"})";

  if (requested.empty())
    return R"({"status":"error","message":"cannot parse GTID set"})";

  std::string name = channel_name != nullptr ? channel_name : "";
  auto cs_ptr = find_channel(name);
  if (!cs_ptr)
    return R"({"status":"error","message":"Channel not found"})";

  std::lock_guard<std::mutex> lk(cs_ptr->io_mutex);
  load_index_file(*cs_ptr);

  auto &fo = cs_ptr->file_order;
  if (fo.empty())
    return R"({"status":"error","message":"Binlog storage is empty"})";

  // Check if any file has GTID metadata
  bool has_gtid_meta = false;
  for (const auto &fname : fo) {
    auto it = cs_ptr->file_metadata.find(fname);
    if (it != cs_ptr->file_metadata.end() &&
        !it->second.last_gtid_set.empty()) {
      has_gtid_meta = true;
      break;
    }
  }
  if (!has_gtid_meta)
    return R"({"status":"error","message":"GTID set search is not supported in storages created in position-based replication mode"})";

  // Find minimal set of files that contain GTIDs from the requested set.
  // A file is included only if its last_gtid_set actually overlaps with
  // the requested set (i.e., the file contains relevant GTIDs).
  // Once last_gtid_set fully covers the requested set, stop.
  std::string result_arr = "[";
  bool first = true;
  bool covered = false;

  for (const auto &fname : fo) {
    auto it = cs_ptr->file_metadata.find(fname);
    if (it == cs_ptr->file_metadata.end()) continue;
    const auto &fm = it->second;

    // If previous_gtids already covers everything requested, done
    if (!fm.previous_gtid_set.empty()) {
      gtid::Gtid_set prev;
      if (prev.assign_from_text(fm.previous_gtid_set)) {
        if (requested.is_subset_of(prev)) {
          covered = true;
          break;
        }
      }
    }

    // Skip file if its cumulative last_gtid_set has no overlap with requested
    if (!fm.last_gtid_set.empty()) {
      gtid::Gtid_set last;
      if (last.assign_from_text(fm.last_gtid_set)) {
        if (!requested.intersects(last)) continue;

        // File has relevant GTIDs — include it
        if (!first) result_arr += ",";
        result_arr += file_meta_to_json(fname, cs_ptr->base_dir, fm);
        first = false;

        if (requested.is_subset_of(last)) {
          covered = true;
          break;
        }
      }
    }
  }
  result_arr += "]";

  if (!covered && first)
    return R"({"status":"error","message":"The specified GTID set cannot be covered"})";

  return "{\"status\":\"success\",\"result\":" + result_arr + "}";
}

}  // namespace binlog_server
