/* Copyright (c) 2026, Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef BINLOG_SERVER_STORAGE_BACKEND_H
#define BINLOG_SERVER_STORAGE_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace binlog_server {

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
};

}  // namespace binlog_server

#endif /* BINLOG_SERVER_STORAGE_BACKEND_H */
