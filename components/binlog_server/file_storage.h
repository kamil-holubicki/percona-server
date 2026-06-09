/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef BINLOG_SERVER_FILE_STORAGE_H
#define BINLOG_SERVER_FILE_STORAGE_H

#include "storage_backend.h"

#include <cstdint>
#include <string>
#include <vector>

namespace binlog_server {

class FileStorage final : public StorageBackend {
 public:
  FileStorage();
  ~FileStorage() override = default;

  const char *type_tag() const override;
  bool uri_allowed(const char *uri, std::string *reason) const override;
  std::string resolve_channel_dir(const char *base_uri,
                                  const char *channel_name) const override;
  bool ensure_channel_dir(const std::string &channel_dir) override;
  bool wipe_channel_dir(const std::string &channel_dir) override;
  bool file_exists(const std::string &dir,
                   const std::string &name) const override;
  std::uint64_t file_size(const std::string &dir,
                          const std::string &name) const override;
  bool remove_file(const std::string &dir, const std::string &name) override;
  std::uint64_t total_bytes(const std::string &dir) const override;
  bool index_load(const std::string &dir,
                  std::vector<std::string> &out) const override;
  bool index_append(const std::string &dir,
                    const std::string &entry) override;
  bool index_rewrite(const std::string &dir,
                     const std::vector<std::string> &entries) override;
  bool sidecar_load(const std::string &dir, const std::string &name,
                    std::string &out) const override;
  bool sidecar_store(const std::string &dir, const std::string &name,
                     const std::string &data) override;

  std::unique_ptr<StorageWriteStream> open_write(
      const std::string &dir, const std::string &name) override;
  std::unique_ptr<StorageReadStream> open_read(
      const std::string &dir, const std::string &name) const override;
  bool rewrite_header(const std::string &dir, const std::string &name,
                      const unsigned char *data, size_t len) override;
  bool truncate_file(const std::string &dir, const std::string &name,
                     uint64_t new_size) override;
};

}  // namespace binlog_server

#endif /* BINLOG_SERVER_FILE_STORAGE_H */
