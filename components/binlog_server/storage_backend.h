/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef BINLOG_SERVER_STORAGE_BACKEND_H
#define BINLOG_SERVER_STORAGE_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace binlog_server {

/// Abstract append-only write stream returned by StorageBackend::open_write.
class StorageWriteStream {
 public:
  virtual ~StorageWriteStream() = default;
  virtual bool write(const unsigned char *data, size_t len) = 0;
  virtual bool flush() = 0;
  virtual bool sync() = 0;
  virtual void close() = 0;
  virtual bool good() const = 0;
};

/// Abstract random-read stream returned by StorageBackend::open_read.
class StorageReadStream {
 public:
  virtual ~StorageReadStream() = default;
  virtual bool read_at(uint64_t offset, unsigned char *buf, size_t len) = 0;
  virtual uint64_t size() const = 0;
  virtual void close() = 0;
};

class StorageBackend {
 public:
  virtual ~StorageBackend() = default;

  virtual const char *type_tag() const = 0;

  virtual bool uri_allowed(const char *uri, std::string *reason) const = 0;

  virtual std::string resolve_channel_dir(const char *base_uri,
                                          const char *channel_name) const = 0;

  virtual bool ensure_channel_dir(const std::string &channel_dir) = 0;

  virtual bool wipe_channel_dir(const std::string &channel_dir) = 0;

  virtual bool file_exists(const std::string &dir,
                           const std::string &name) const = 0;

  virtual std::uint64_t file_size(const std::string &dir,
                                  const std::string &name) const = 0;

  virtual bool remove_file(const std::string &dir,
                           const std::string &name) = 0;

  virtual std::uint64_t total_bytes(const std::string &dir) const = 0;

  virtual bool index_load(const std::string &dir,
                          std::vector<std::string> &out) const = 0;

  virtual bool index_append(const std::string &dir,
                            const std::string &entry) = 0;

  virtual bool index_rewrite(const std::string &dir,
                             const std::vector<std::string> &entries) = 0;

  virtual bool sidecar_load(const std::string &dir, const std::string &name,
                            std::string &out) const = 0;

  virtual bool sidecar_store(const std::string &dir, const std::string &name,
                             const std::string &data) = 0;

  // --- Streaming file I/O (used by archive write/read/encryption) ---

  /// Open a file for append-only writing. Creates the file if it doesn't exist.
  virtual std::unique_ptr<StorageWriteStream> open_write(
      const std::string &dir, const std::string &name) = 0;

  /// Open a file for random-access reading.
  virtual std::unique_ptr<StorageReadStream> open_read(
      const std::string &dir, const std::string &name) const = 0;

  /// Overwrite the first `len` bytes of a file (for encryption header rewrite).
  virtual bool rewrite_header(const std::string &dir, const std::string &name,
                              const unsigned char *data, size_t len) = 0;

  /// Truncate a file to the given size (for crash recovery).
  virtual bool truncate_file(const std::string &dir, const std::string &name,
                             uint64_t new_size) = 0;
};

}  // namespace binlog_server

#endif /* BINLOG_SERVER_STORAGE_BACKEND_H */
